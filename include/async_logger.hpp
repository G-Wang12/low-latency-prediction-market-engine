#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <utility>

#include "spsc_queue.hpp"

struct LogEvent
{
    std::uint64_t timestamp_us;
    char event_type; // e.g. 'T' trade, 'P' pnl update
    double price;
    int size;
    double realized_pnl;
};

class AsyncLogger
{
public:
    static constexpr std::size_t kQueueSize = 4096;

    explicit AsyncLogger(std::string csv_path = "trading_log.csv")
        : csv_path_(std::move(csv_path)),
          running_(true),
          worker_(&AsyncLogger::run, this)
    {
    }

    AsyncLogger(const AsyncLogger &) = delete;
    AsyncLogger &operator=(const AsyncLogger &) = delete;

    ~AsyncLogger()
    {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    // Lock-free, non-blocking hot-path call.
    // Returns false if the queue is full and the event was dropped.
    [[nodiscard]] bool log_event(std::uint64_t timestamp_us,
                                 char event_type,
                                 double price,
                                 int size,
                                 double realized_pnl) noexcept
    {
        LogEvent ev{};
        ev.timestamp_us = timestamp_us;
        ev.event_type = event_type;
        ev.price = price;
        ev.size = size;
        ev.realized_pnl = realized_pnl;
        return queue_.push(ev);
    }

private:
    void run()
    {
        std::ofstream out(csv_path_, std::ios::out | std::ios::trunc);
        if (!out.is_open())
        {
            // Can't log; keep draining queue (to prevent backpressure) until shutdown.
            drain_without_io();
            return;
        }

        out << "timestamp_us,event_type,price,size,realized_pnl\n";

        std::uint64_t events_since_flush = 0U;
        auto last_flush = std::chrono::steady_clock::now();

        LogEvent ev{};
        while (running_.load(std::memory_order_acquire))
        {
            if (!queue_.pop(ev))
            {
                std::this_thread::yield();
                maybe_flush(out, events_since_flush, last_flush);
                continue;
            }

            out << ev.timestamp_us << ','
                << ev.event_type << ','
                << ev.price << ','
                << ev.size << ','
                << ev.realized_pnl << '\n';

            ++events_since_flush;
            maybe_flush(out, events_since_flush, last_flush);
        }

        // Drain any remaining events after shutdown is requested.
        while (queue_.pop(ev))
        {
            out << ev.timestamp_us << ','
                << ev.event_type << ','
                << ev.price << ','
                << ev.size << ','
                << ev.realized_pnl << '\n';
        }

        out.flush();
    }

    static void maybe_flush(std::ofstream &out,
                            std::uint64_t &events_since_flush,
                            std::chrono::steady_clock::time_point &last_flush)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - last_flush;

        if (events_since_flush >= 1000U || elapsed >= std::chrono::seconds(1))
        {
            out.flush();
            events_since_flush = 0U;
            last_flush = now;
        }
    }

    void drain_without_io() noexcept
    {
        LogEvent ev{};
        while (running_.load(std::memory_order_acquire))
        {
            if (!queue_.pop(ev))
            {
                std::this_thread::yield();
            }
        }

        while (queue_.pop(ev))
        {
            // drain
        }
    }

    SpscQueue<LogEvent, kQueueSize> queue_;
    std::string csv_path_;

    std::atomic<bool> running_{false};
    std::thread worker_;
};
