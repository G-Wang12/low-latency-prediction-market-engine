#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
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
    static constexpr std::size_t kMaxPayloadBytes = 65536;

    MarketParser() noexcept
    {
        const simdjson::error_code err = parser_.allocate(kMaxPayloadBytes + simdjson::SIMDJSON_PADDING);
        initialized_ = (err == simdjson::SUCCESS);
    }

    // Parse a single L2 message and emit one MarketTick per updated price level.
    // Supported schemas:
    //   1) Internal mock feed (arrays of [price,size] string pairs):
    //      {"bids":[["0.57","1200"],["0.56","300"]],"asks":[["0.59","500"]]}
    //   2) Polymarket Market Channel book snapshot:
    //      {"event_type":"book","bids":[{"price":".48","size":"30"},...],"asks":[...]}
    //   3) Polymarket Market Channel price_change delta:
    //      {"event_type":"price_change","price_changes":[{"price":"0.5","size":"200","side":"BUY"},...]}
    // Prices/sizes are strings to avoid float precision loss.
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

        // Polymarket may send either a single object message or an array of object messages.
        simdjson::ondemand::object obj;
        if (doc.get_object().get(obj) == simdjson::SUCCESS)
        {
            return parse_message_object(obj, out_queue);
        }

        simdjson::ondemand::array arr;
        if (doc.get_array().get(arr) == simdjson::SUCCESS)
        {
            for (simdjson::ondemand::value v : arr)
            {
                simdjson::ondemand::object elem;
                if (v.get_object().get(elem) != simdjson::SUCCESS)
                {
                    // Unexpected element type; skip it.
                    continue;
                }
                (void)parse_message_object(elem, out_queue);
            }
            return true;
        }

        return false;
    }

private:
    template <std::size_t QueueSize>
    [[nodiscard]] bool parse_message_object(simdjson::ondemand::object &obj,
                                            SpscQueue<MarketTick, QueueSize> &out_queue) noexcept
    {
        // Detect Polymarket-style messages (event_type present) vs our internal mock schema.
        simdjson::ondemand::value event_type_val;
        const simdjson::error_code event_err = obj.find_field_unordered("event_type").get(event_type_val);
        if (event_err == simdjson::SUCCESS)
        {
            std::string_view event_type_sv;
            if (event_type_val.get_string().get(event_type_sv) == simdjson::SUCCESS)
            {
                if (event_type_sv == "book")
                {
                    if (!parse_object_levels(obj, "bids", true, out_queue))
                    {
                        return false;
                    }
                    if (!parse_object_levels(obj, "asks", false, out_queue))
                    {
                        return false;
                    }
                    return true;
                }

                if (event_type_sv == "price_change")
                {
                    return parse_price_change(obj, out_queue);
                }

                // Ignore other event types (e.g., last_trade_price, tick_size_change, new_market).
                return true;
            }
        }
        else if (event_err != simdjson::NO_SUCH_FIELD)
        {
            return false;
        }

        // Internal mock feed schema: arrays of [price, size] string pairs.
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

    // Parse a size string which is usually an integer, but may be a decimal.
    // If a decimal is present, we scale by 1e6 (micro-units) to keep it integral.
    // If the decimal part is all zeros (e.g. "10.0"), treat it as an integer.
    [[nodiscard]] static bool parse_size_u32(std::string_view sv, std::uint32_t &out) noexcept
    {
        if (sv.empty())
        {
            return false;
        }

        const std::size_t dot = sv.find('.');
        if (dot == std::string_view::npos)
        {
            return parse_u32(sv, out);
        }

        const std::string_view int_part = sv.substr(0, dot);
        const std::string_view frac_part = sv.substr(dot + 1);

        std::uint32_t whole = 0U;
        if (!int_part.empty())
        {
            if (!parse_u32(int_part, whole))
            {
                return false;
            }
        }

        if (frac_part.empty())
        {
            out = whole;
            return true;
        }

        bool any_nonzero = false;
        for (char c : frac_part)
        {
            if (c < '0' || c > '9')
            {
                return false;
            }
            if (c != '0')
            {
                any_nonzero = true;
            }
        }

        if (!any_nonzero)
        {
            out = whole;
            return true;
        }

        // Scale fractional part to 6 digits.
        std::uint32_t frac_scaled = 0U;
        std::size_t digits = 0U;
        for (; digits < 6U && digits < frac_part.size(); ++digits)
        {
            frac_scaled = frac_scaled * 10U + static_cast<std::uint32_t>(frac_part[digits] - '0');
        }
        for (; digits < 6U; ++digits)
        {
            frac_scaled *= 10U;
        }

        const std::uint64_t scaled = static_cast<std::uint64_t>(whole) * 1000000ULL + static_cast<std::uint64_t>(frac_scaled);
        if (scaled > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return false;
        }

        out = static_cast<std::uint32_t>(scaled);
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

        std::uint32_t rounding_digit = 0U;
        if (frac_part.size() >= 3U)
        {
            const char c = frac_part[2];
            if (c < '0' || c > '9')
            {
                return false;
            }
            rounding_digit = static_cast<std::uint32_t>(c - '0');
        }

        for (std::size_t i = 3; i < frac_part.size(); ++i)
        {
            const char c = frac_part[i];
            if (c < '0' || c > '9')
            {
                return false;
            }
        }

        std::uint32_t cents = whole * 100U + (d0 * 10U + d1);
        if (rounding_digit >= 5U)
        {
            ++cents;
        }
        out_cents = cents;
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
            if (!parse_size_u32(size_sv, size))
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

    template <std::size_t QueueSize>
    [[nodiscard]] bool parse_object_levels(simdjson::ondemand::object &obj,
                                           const char *field,
                                           bool is_bid,
                                           SpscQueue<MarketTick, QueueSize> &out_queue) noexcept
    {
        simdjson::ondemand::value levels_val;
        const simdjson::error_code find_err = obj.find_field_unordered(field).get(levels_val);
        if (find_err == simdjson::NO_SUCH_FIELD)
        {
            return true;
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
            simdjson::ondemand::object level_obj;
            if (level_val.get_object().get(level_obj) != simdjson::SUCCESS)
            {
                return false;
            }

            simdjson::ondemand::value price_val;
            if (level_obj.find_field_unordered("price").get(price_val) != simdjson::SUCCESS)
            {
                continue;
            }
            std::string_view price_sv;
            if (price_val.get_string().get(price_sv) != simdjson::SUCCESS)
            {
                continue;
            }

            simdjson::ondemand::value size_val;
            if (level_obj.find_field_unordered("size").get(size_val) != simdjson::SUCCESS)
            {
                continue;
            }
            std::string_view size_sv;
            if (size_val.get_string().get(size_sv) != simdjson::SUCCESS)
            {
                continue;
            }

            std::uint32_t price_cents = 0U;
            if (!parse_price_cents(price_sv, price_cents) || !in_price_range(price_cents))
            {
                continue;
            }

            std::uint32_t size = 0U;
            if (!parse_size_u32(size_sv, size))
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

    template <std::size_t QueueSize>
    [[nodiscard]] bool parse_price_change(simdjson::ondemand::object &obj,
                                          SpscQueue<MarketTick, QueueSize> &out_queue) noexcept
    {
        simdjson::ondemand::value changes_val;
        const simdjson::error_code find_err = obj.find_field_unordered("price_changes").get(changes_val);
        if (find_err == simdjson::NO_SUCH_FIELD)
        {
            return true;
        }
        if (find_err != simdjson::SUCCESS)
        {
            return false;
        }

        simdjson::ondemand::array changes;
        if (changes_val.get_array().get(changes) != simdjson::SUCCESS)
        {
            return false;
        }

        for (simdjson::ondemand::value change_val : changes)
        {
            simdjson::ondemand::object change_obj;
            if (change_val.get_object().get(change_obj) != simdjson::SUCCESS)
            {
                return false;
            }

            std::string_view price_sv;
            {
                simdjson::ondemand::value price_val;
                if (change_obj.find_field_unordered("price").get(price_val) != simdjson::SUCCESS)
                {
                    continue;
                }
                if (price_val.get_string().get(price_sv) != simdjson::SUCCESS)
                {
                    continue;
                }
            }

            std::string_view size_sv;
            {
                simdjson::ondemand::value size_val;
                if (change_obj.find_field_unordered("size").get(size_val) != simdjson::SUCCESS)
                {
                    continue;
                }
                if (size_val.get_string().get(size_sv) != simdjson::SUCCESS)
                {
                    continue;
                }
            }

            std::string_view side_sv;
            {
                simdjson::ondemand::value side_val;
                if (change_obj.find_field_unordered("side").get(side_val) != simdjson::SUCCESS)
                {
                    continue;
                }
                if (side_val.get_string().get(side_sv) != simdjson::SUCCESS)
                {
                    continue;
                }
            }

            const bool is_bid = (side_sv == "BUY");
            const bool is_ask = (side_sv == "SELL");
            if (!is_bid && !is_ask)
            {
                continue;
            }

            std::uint32_t price_cents = 0U;
            if (!parse_price_cents(price_sv, price_cents) || !in_price_range(price_cents))
            {
                continue;
            }

            std::uint32_t size = 0U;
            if (!parse_size_u32(size_sv, size))
            {
                continue;
            }

            MarketTick tick{};
            tick.price = static_cast<std::uint8_t>(price_cents);
            tick.size = size;
            tick.is_bid = is_bid;
            (void)out_queue.push(tick);

            // Some Polymarket deltas also include top-of-book hints. If present,
            // emit minimal ticks so downstream strategy can establish a two-sided book.
            {
                simdjson::ondemand::value best_bid_val;
                if (change_obj.find_field_unordered("best_bid").get(best_bid_val) == simdjson::SUCCESS)
                {
                    std::string_view best_bid_sv;
                    if (best_bid_val.get_string().get(best_bid_sv) == simdjson::SUCCESS)
                    {
                        std::uint32_t best_bid_cents = 0U;
                        if (parse_price_cents(best_bid_sv, best_bid_cents) &&
                            in_price_range(best_bid_cents) &&
                            best_bid_cents != 0U)
                        {
                            MarketTick bb{};
                            bb.price = static_cast<std::uint8_t>(best_bid_cents);
                            bb.size = 1U;
                            bb.is_bid = true;
                            (void)out_queue.push(bb);
                        }
                    }
                }

                simdjson::ondemand::value best_ask_val;
                if (change_obj.find_field_unordered("best_ask").get(best_ask_val) == simdjson::SUCCESS)
                {
                    std::string_view best_ask_sv;
                    if (best_ask_val.get_string().get(best_ask_sv) == simdjson::SUCCESS)
                    {
                        std::uint32_t best_ask_cents = 0U;
                        if (parse_price_cents(best_ask_sv, best_ask_cents) &&
                            in_price_range(best_ask_cents) &&
                            best_ask_cents != 0U)
                        {
                            MarketTick ba{};
                            ba.price = static_cast<std::uint8_t>(best_ask_cents);
                            ba.size = 1U;
                            ba.is_bid = false;
                            (void)out_queue.push(ba);
                        }
                    }
                }
            }
        }

        return true;
    }

    simdjson::ondemand::parser parser_{};
    bool initialized_{false};

    alignas(64) std::array<char, kMaxPayloadBytes + simdjson::SIMDJSON_PADDING> buffer_{};
};
