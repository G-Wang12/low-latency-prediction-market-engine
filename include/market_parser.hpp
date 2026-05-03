#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <simdjson/ondemand.h>
#include <simdjson/padded_string_view-inl.h>

#include "order_book.hpp"
#include "spsc_queue.hpp"

class MarketParser
{
public:
    // Real L2 snapshots can be much larger than a single-tick mock payload.
    // This remains fixed-capacity for predictable performance.
    static constexpr std::size_t kMaxPayloadBytes = 16384;

    MarketParser() noexcept
    {
        const simdjson::error_code err = parser_.allocate(kMaxPayloadBytes + simdjson::SIMDJSON_PADDING);
        initialized_ = (err == simdjson::SUCCESS);
    }

    // Parse a single L2 message and emit one MarketTick per updated price level.
    // Expected schema:
    //   {"bids":[["0.57","1200"],["0.56","300"]],"asks":[["0.59","500"]]}
    // Prices and sizes are strings to avoid float precision loss.
    template <std::size_t QueueSize>
    [[nodiscard]] bool parse_tick(std::string_view json_payload, SpscQueue<MarketTick, QueueSize> &out_queue) noexcept
    {
        if (!initialized_)
        {
            return false;
        }

        const std::size_t len = json_payload.size();
        if (len == 0U || len > kMaxPayloadBytes)
        {
            return false;
        }

        std::memcpy(buffer_.data(), json_payload.data(), len);
        std::memset(buffer_.data() + len, 0, simdjson::SIMDJSON_PADDING);

        simdjson::ondemand::document doc;
        const simdjson::padded_string_view view(buffer_.data(), len, buffer_.size());
        if (parser_.iterate(view).get(doc) != simdjson::SUCCESS)
        {
            return false;
        }

        simdjson::ondemand::object obj;
        if (doc.get_object().get(obj) != simdjson::SUCCESS)
        {
            return false;
        }

        // Snapshot/delta messages contain arrays of [price, size] string pairs.
        // Emit one tick per updated level.
        if (!parse_levels(obj, "bids", true, out_queue))
        {
            return false;
        }
        if (!parse_levels(obj, "asks", false, out_queue))
        {
            return false;
        }

        return true;
    }

private:
    static constexpr bool in_price_range(std::uint32_t cents) noexcept
    {
        return cents >= static_cast<std::uint32_t>(LimitOrderBook::kMinPrice) &&
               cents <= static_cast<std::uint32_t>(LimitOrderBook::kMaxPrice);
    }

    [[nodiscard]] static bool parse_u32(std::string_view sv, std::uint32_t &out) noexcept
    {
        if (sv.empty())
        {
            return false;
        }

        std::uint32_t value = 0U;
        const char *const begin = sv.data();
        const char *const end = sv.data() + sv.size();
        const auto res = std::from_chars(begin, end, value);
        if (res.ec != std::errc{} || res.ptr != end)
        {
            return false;
        }
        out = value;
        return true;
    }

    // Convert a string price like "0.57" to integer cents (57) without floats.
    // Also accepts "57" (already-cents) for flexibility.
    [[nodiscard]] static bool parse_price_cents(std::string_view sv, std::uint32_t &out_cents) noexcept
    {
        if (sv.empty())
        {
            return false;
        }

        const std::size_t dot = sv.find('.');
        if (dot == std::string_view::npos)
        {
            std::uint32_t cents = 0U;
            if (!parse_u32(sv, cents))
            {
                return false;
            }
            out_cents = cents;
            return true;
        }

        const std::string_view int_part = sv.substr(0, dot);
        const std::string_view frac_part = sv.substr(dot + 1);
        if (frac_part.empty())
        {
            return false;
        }

        std::uint32_t whole = 0U;
        if (!int_part.empty())
        {
            if (!parse_u32(int_part, whole))
            {
                return false;
            }
        }

        // Take up to two decimal digits; pad with zeros if needed.
        auto digit_or_zero = [&](std::size_t idx) -> std::uint32_t
        {
            if (idx >= frac_part.size())
            {
                return 0U;
            }
            const char c = frac_part[idx];
            if (c < '0' || c > '9')
            {
                return 10U; // sentinel invalid
            }
            return static_cast<std::uint32_t>(c - '0');
        };

        const std::uint32_t d0 = digit_or_zero(0);
        const std::uint32_t d1 = digit_or_zero(1);
        if (d0 > 9U || d1 > 9U)
        {
            return false;
        }

        for (std::size_t i = 2; i < frac_part.size(); ++i)
        {
            const char c = frac_part[i];
            if (c < '0' || c > '9')
            {
                return false;
            }
        }

        out_cents = whole * 100U + (d0 * 10U + d1);
        return true;
    }

    template <std::size_t QueueSize>
    [[nodiscard]] bool parse_levels(simdjson::ondemand::object &obj,
                                    const char *field,
                                    bool is_bid,
                                    SpscQueue<MarketTick, QueueSize> &out_queue) noexcept
    {
        simdjson::ondemand::value levels_val;
        const simdjson::error_code find_err = obj.find_field_unordered(field).get(levels_val);
        if (find_err == simdjson::NO_SUCH_FIELD)
        {
            return true; // field is optional
        }
        if (find_err != simdjson::SUCCESS)
        {
            return false;
        }

        simdjson::ondemand::array levels;
        if (levels_val.get_array().get(levels) != simdjson::SUCCESS)
        {
            return false;
        }

        for (simdjson::ondemand::value level_val : levels)
        {
            simdjson::ondemand::array pair;
            if (level_val.get_array().get(pair) != simdjson::SUCCESS)
            {
                return false;
            }

            auto it = pair.begin();
            if (it == pair.end())
            {
                return false;
            }

            std::string_view price_sv;
            if ((*it).get_string().get(price_sv) != simdjson::SUCCESS)
            {
                // Price must be a JSON string
                continue;
            }
            ++it;
            if (it == pair.end())
            {
                return false;
            }

            std::string_view size_sv;
            if ((*it).get_string().get(size_sv) != simdjson::SUCCESS)
            {
                continue;
            }
            ++it;
            if (it != pair.end())
            {
                // Enforce exactly two elements per level.
                return false;
            }

            std::uint32_t price_cents = 0U;
            if (!parse_price_cents(price_sv, price_cents) || !in_price_range(price_cents))
            {
                continue;
            }

            std::uint32_t size = 0U;
            if (!parse_u32(size_sv, size))
            {
                continue;
            }

            MarketTick tick{};
            tick.price = static_cast<std::uint8_t>(price_cents);
            tick.size = size;
            tick.is_bid = is_bid;
            (void)out_queue.push(tick);
        }

        return true;
    }

    simdjson::ondemand::parser parser_{};
    bool initialized_{false};

    alignas(64) std::array<char, kMaxPayloadBytes + simdjson::SIMDJSON_PADDING> buffer_{};
};
