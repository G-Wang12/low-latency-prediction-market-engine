#include "strategy_engine.hpp"

#include <chrono>
#include <cstdint>

#include "momentum_alpha.hpp"
#include "ofi_alpha.hpp"

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
                               AsyncLogger &logger,
                               StrategySelection selection)
    : queue_(queue),
      book_(book), position_manager_(position_manager), logger_(logger)
{
    // Construct signals once (outside the hot path). No allocations in run().
    switch (selection)
    {
    case StrategySelection::momentum:
        signals_.reserve(1);
        signals_.push_back(std::make_unique<MomentumAlpha>());
        break;
    case StrategySelection::ofi:
        signals_.reserve(1);
        signals_.push_back(std::make_unique<OFIAlpha>());
        break;
    case StrategySelection::both:
    default:
        signals_.reserve(2);
        signals_.push_back(std::make_unique<MomentumAlpha>());
        signals_.push_back(std::make_unique<OFIAlpha>());
        break;
    }
}

void StrategyEngine::stop() noexcept
{
    running_.store(false, std::memory_order_relaxed);
}

void StrategyEngine::run()
{
    MarketTick tick{};
    std::uint64_t ticks_processed = 0U;

    while (running_.load(std::memory_order_relaxed))
    {
        if (!queue_.pop(tick))
        {
            // Busy-wait (spin) to minimize wake-up latency.
            continue;
        }

        const std::uint64_t tick_start_us = now_us();

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

        if (!emitted_initial_p_)
        {
            const double mid_price = (cents_to_price(best_bid) + cents_to_price(best_ask)) * 0.5;
            const double equity_pnl = position_manager_.realized_pnl() +
                                      position_manager_.get_unrealized_pnl(mid_price);
            const std::uint64_t latency_us = now_us() - tick_start_us;
            (void)logger_.log_event(now_us(), 'P', mid_price, position_manager_.position_size(), equity_pnl, latency_us);
            emitted_initial_p_ = true;
        }

        // Update all alpha signals and average their confidence scores.
        double score_sum = 0.0;
        const std::size_t n = signals_.size();
        for (std::size_t i = 0; i < n; ++i)
        {
            score_sum += signals_[i]->update(book_, tick);
        }
        const double combined_score = (n == 0U) ? 0.0 : (score_sum / static_cast<double>(n));

        int fill_size = 0;
        double fill_price = 0.0;
        if (combined_score >= kTradeThreshold)
        {
            // Strong positive confidence -> buy at best ask.
            fill_size = kDefaultTradeSize;
            fill_price = cents_to_price(best_ask);
        }
        else if (combined_score <= -kTradeThreshold)
        {
            // Strong negative confidence -> sell at best bid.
            fill_size = -kDefaultTradeSize;
            fill_price = cents_to_price(best_bid);
        }

        if (fill_size != 0)
        {
            position_manager_.add_fill(fill_size, fill_price);
            const std::uint64_t latency_us = now_us() - tick_start_us;
            (void)logger_.log_event(now_us(), 'T', fill_price, fill_size, position_manager_.realized_pnl(), latency_us);
        }

        // Periodic mark-to-market update so the dashboard has a smooth curve.
        if ((ticks_processed % 1000U) == 0U)
        {
            const double mid_price = (cents_to_price(best_bid) + cents_to_price(best_ask)) * 0.5;
            const double equity_pnl = position_manager_.realized_pnl() +
                                      position_manager_.get_unrealized_pnl(mid_price);
            const std::uint64_t latency_us = now_us() - tick_start_us;
            (void)logger_.log_event(now_us(), 'P', mid_price, position_manager_.position_size(), equity_pnl, latency_us);
        }
    }
}
