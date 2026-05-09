#include "strategy_engine.hpp"

#include <chrono>
#include <cstdint>

namespace
{
    [[nodiscard]] std::uint64_t now_us() noexcept
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }

    [[nodiscard]] double cents_to_price(std::uint8_t price_cents) noexcept
    {
        return static_cast<double>(price_cents) / 100.0;
    }
} // namespace

StrategyEngine::StrategyEngine(SpscQueue<MarketTick, engine_config::kTickQueueSize> &queue,
                               LimitOrderBook &book,
                               PositionManager &position_manager,
                               AsyncLogger &logger) noexcept
    : queue_(queue),
      book_(book), position_manager_(position_manager), logger_(logger)
{
}

void StrategyEngine::stop() noexcept
{
    running_.store(false, std::memory_order_relaxed);
}

void StrategyEngine::run()
{
    MarketTick tick{};

    bool has_prev_microprice = false;
    double prev_microprice_cents = 0.0;
    std::uint64_t ticks_processed = 0U;

    while (running_.load(std::memory_order_relaxed))
    {
        if (!queue_.pop(tick))
        {
            // Busy-wait (spin) to minimize wake-up latency.
            continue;
        }

        ++ticks_processed;

        book_.apply_tick(tick);

        const std::uint8_t best_bid = book_.get_best_bid();
        const std::uint8_t best_ask = book_.get_best_ask();

        if (best_bid == LimitOrderBook::kNoBid || best_ask == LimitOrderBook::kNoAsk)
        {
            continue;
        }

        const std::uint32_t best_bid_size = book_.get_bid_size(best_bid);
        const std::uint32_t best_ask_size = book_.get_ask_size(best_ask);
        const std::uint32_t denom = best_bid_size + best_ask_size;
        if (denom == 0U)
        {
            continue;
        }

        const double microprice_cents =
            (static_cast<double>(best_bid) * static_cast<double>(best_ask_size) +
             static_cast<double>(best_ask) * static_cast<double>(best_bid_size)) /
            static_cast<double>(denom);

        // First time we can compute a microprice, emit a mark-to-market update
        // immediately so the dashboard has data without waiting for 1000 ticks.
        if (!has_prev_microprice)
        {
            const double mid_price = (cents_to_price(best_bid) + cents_to_price(best_ask)) * 0.5;
            const double equity_pnl = position_manager_.realized_pnl() +
                                      position_manager_.get_unrealized_pnl(mid_price);
            (void)logger_.log_event(now_us(), 'P', mid_price, position_manager_.position_size(), equity_pnl);

            prev_microprice_cents = microprice_cents;
            has_prev_microprice = true;
            continue;
        }

        const double up = microprice_cents - prev_microprice_cents;
        const double down = prev_microprice_cents - microprice_cents;

        int fill_size = 0;
        double fill_price = 0.0;
        if (up > 1.0)
        {
            // Momentum up -> buy at best ask.
            fill_size = 10;
            fill_price = cents_to_price(best_ask);
        }
        else if (down > 1.0)
        {
            // Momentum down -> sell at best bid.
            fill_size = -10;
            fill_price = cents_to_price(best_bid);
        }

        if (fill_size != 0)
        {
            position_manager_.add_fill(fill_size, fill_price);
            (void)logger_.log_event(now_us(), 'T', fill_price, fill_size, position_manager_.realized_pnl());
        }

        prev_microprice_cents = microprice_cents;
        has_prev_microprice = true;

        // Periodic mark-to-market update so the dashboard has a smooth curve.
        if ((ticks_processed % 1000U) == 0U)
        {
            const double mid_price = (cents_to_price(best_bid) + cents_to_price(best_ask)) * 0.5;
            const double equity_pnl = position_manager_.realized_pnl() +
                                      position_manager_.get_unrealized_pnl(mid_price);
            (void)logger_.log_event(now_us(), 'P', mid_price, position_manager_.position_size(), equity_pnl);
        }
    }
}
