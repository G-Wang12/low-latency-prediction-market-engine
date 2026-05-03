#pragma once

#include <cstddef>

class PositionManager
{
public:
    PositionManager() noexcept = default;

    // Positive position_size_ = long, negative = short.
    void add_fill(int fill_size, double fill_price) noexcept
    {
        if (fill_size == 0)
        {
            return;
        }

        // If we are flat, any fill opens a new position with entry at fill_price.
        if (position_size_ == 0)
        {
            position_size_ = fill_size;
            average_entry_price_ = fill_price;
            return;
        }

        const bool same_direction = (position_size_ > 0 && fill_size > 0) || (position_size_ < 0 && fill_size < 0);
        if (same_direction)
        {
            // Increasing position: update VWAP.
            const int old_abs = (position_size_ >= 0) ? position_size_ : -position_size_;
            const int add_abs = (fill_size >= 0) ? fill_size : -fill_size;
            const int new_abs = old_abs + add_abs;

            // new_abs can't be 0 here.
            const double weighted = average_entry_price_ * static_cast<double>(old_abs) +
                                    fill_price * static_cast<double>(add_abs);
            average_entry_price_ = weighted / static_cast<double>(new_abs);
            position_size_ += fill_size;
            return;
        }

        // Opposite direction: this fill reduces existing position, possibly flipping.
        const int pos_abs = (position_size_ >= 0) ? position_size_ : -position_size_;
        const int fill_abs = (fill_size >= 0) ? fill_size : -fill_size;
        const int closing_abs = (fill_abs < pos_abs) ? fill_abs : pos_abs;

        // Realized PnL from the closing portion.
        // Long close (sell): pnl += qty * (sell - avg)
        // Short close (buy): pnl += qty * (avg - buy)
        if (position_size_ > 0)
        {
            realized_pnl_ += static_cast<double>(closing_abs) * (fill_price - average_entry_price_);
        }
        else
        {
            realized_pnl_ += static_cast<double>(closing_abs) * (average_entry_price_ - fill_price);
        }

        position_size_ += fill_size;

        if (position_size_ == 0)
        {
            // Flat: reset entry price.
            average_entry_price_ = 0.0;
            return;
        }

        // If we flipped through zero, the remaining quantity opens a new position at fill_price.
        const bool flipped = (position_size_ > 0 && fill_size > 0) || (position_size_ < 0 && fill_size < 0);
        if (flipped)
        {
            average_entry_price_ = fill_price;
        }
        // Else: we partially closed but did not flip, so average_entry_price_ remains unchanged.
    }

    [[nodiscard]] double get_unrealized_pnl(double current_mid_price) const noexcept
    {
        return static_cast<double>(position_size_) * (current_mid_price - average_entry_price_);
    }

    [[nodiscard]] int position_size() const noexcept { return position_size_; }
    [[nodiscard]] double realized_pnl() const noexcept { return realized_pnl_; }
    [[nodiscard]] double average_entry_price() const noexcept { return average_entry_price_; }

private:
    int position_size_{0};
    double realized_pnl_{0.0};
    double average_entry_price_{0.0};
};
