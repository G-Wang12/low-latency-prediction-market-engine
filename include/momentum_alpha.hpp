#pragma once

#include <cstdint>

#include "alpha_base.hpp"

class MomentumAlpha final : public IAlphaSignal
{
public:
    MomentumAlpha() = default;

    double update(const LimitOrderBook &book, const MarketTick &tick) override
    {
        (void)tick;

        const std::uint8_t best_bid = book.get_best_bid();
        const std::uint8_t best_ask = book.get_best_ask();
        if (best_bid == LimitOrderBook::kNoBid || best_ask == LimitOrderBook::kNoAsk)
        {
            return 0.0;
        }

        const std::uint32_t best_bid_size = book.get_bid_size(best_bid);
        const std::uint32_t best_ask_size = book.get_ask_size(best_ask);
        const std::uint32_t denom = best_bid_size + best_ask_size;
        if (denom == 0U)
        {
            return 0.0;
        }

        const double microprice_cents =
            (static_cast<double>(best_bid) * static_cast<double>(best_ask_size) +
             static_cast<double>(best_ask) * static_cast<double>(best_bid_size)) /
            static_cast<double>(denom);

        if (!has_prev_)
        {
            prev_microprice_cents_ = microprice_cents;
            has_prev_ = true;
            return 0.0;
        }

        const double delta = microprice_cents - prev_microprice_cents_;
        prev_microprice_cents_ = microprice_cents;

        // Old behavior: trade when |delta| > 1 cent. Map that to max confidence.
        if (delta > 1.0)
        {
            return 1.0;
        }
        if (delta < -1.0)
        {
            return -1.0;
        }
        return 0.0;
    }

private:
    bool has_prev_{false};
    double prev_microprice_cents_{0.0};
};
