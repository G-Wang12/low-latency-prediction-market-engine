#pragma once

#include <algorithm>
#include <cstdint>

#include "alpha_base.hpp"

class OFIAlpha final : public IAlphaSignal
{
public:
    OFIAlpha() = default;

    double update(const LimitOrderBook &book, const MarketTick &tick) override
    {
        (void)tick;

        const std::uint8_t best_bid = book.get_best_bid();
        const std::uint8_t best_ask = book.get_best_ask();
        if (best_bid == LimitOrderBook::kNoBid || best_ask == LimitOrderBook::kNoAsk)
        {
            initialized_ = false;
            return 0.0;
        }

        const std::uint32_t best_bid_size = book.get_bid_size(best_bid);
        const std::uint32_t best_ask_size = book.get_ask_size(best_ask);

        if (!initialized_)
        {
            prev_best_bid_ = best_bid;
            prev_best_ask_ = best_ask;
            prev_best_bid_size_ = best_bid_size;
            prev_best_ask_size_ = best_ask_size;
            initialized_ = true;
            return 0.0;
        }

        // Top-of-book OFI-style update (Cont et al.-style intuition):
        // - Bid pressure increases when best bid improves or its size increases.
        // - Ask pressure increases when best ask improves (drops) or its size increases.
        // We compute OFI in size units and normalize to [-1, 1].
        double ofi = 0.0;

        // Bid contribution
        if (best_bid > prev_best_bid_)
        {
            ofi += static_cast<double>(best_bid_size);
        }
        else if (best_bid == prev_best_bid_)
        {
            ofi += static_cast<double>(static_cast<std::int64_t>(best_bid_size) - static_cast<std::int64_t>(prev_best_bid_size_));
        }
        else
        {
            ofi -= static_cast<double>(prev_best_bid_size_);
        }

        // Ask contribution
        if (best_ask < prev_best_ask_)
        {
            ofi -= static_cast<double>(best_ask_size);
        }
        else if (best_ask == prev_best_ask_)
        {
            ofi -= static_cast<double>(static_cast<std::int64_t>(best_ask_size) - static_cast<std::int64_t>(prev_best_ask_size_));
        }
        else
        {
            ofi += static_cast<double>(prev_best_ask_size_);
        }

        // "Book pressure" term based on microprice location within the spread.
        const std::uint32_t denom_sizes = best_bid_size + best_ask_size;
        const double microprice_cents = (denom_sizes == 0U)
                                            ? (static_cast<double>(best_bid) + static_cast<double>(best_ask)) * 0.5
                                            : (static_cast<double>(best_bid) * static_cast<double>(best_ask_size) +
                                               static_cast<double>(best_ask) * static_cast<double>(best_bid_size)) /
                                                  static_cast<double>(denom_sizes);

        const double mid_cents = (static_cast<double>(best_bid) + static_cast<double>(best_ask)) * 0.5;
        const double spread_cents = std::max(1.0, static_cast<double>(static_cast<int>(best_ask) - static_cast<int>(best_bid)));
        const double book_pressure = std::clamp((microprice_cents - mid_cents) / spread_cents, -1.0, 1.0);

        const double norm_denom = std::max(1.0, static_cast<double>(denom_sizes));
        const double ofi_norm = std::clamp(ofi / norm_denom, -1.0, 1.0);

        // Blend: OFI captures changes; microprice-within-spread captures current imbalance.
        const double confidence = std::clamp(0.65 * ofi_norm + 0.35 * book_pressure, -1.0, 1.0);

        prev_best_bid_ = best_bid;
        prev_best_ask_ = best_ask;
        prev_best_bid_size_ = best_bid_size;
        prev_best_ask_size_ = best_ask_size;

        return confidence;
    }

private:
    bool initialized_{false};
    std::uint8_t prev_best_bid_{LimitOrderBook::kNoBid};
    std::uint8_t prev_best_ask_{LimitOrderBook::kNoAsk};
    std::uint32_t prev_best_bid_size_{0U};
    std::uint32_t prev_best_ask_size_{0U};
};
