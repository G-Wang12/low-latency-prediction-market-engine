#pragma once

#include <atomic>

#include "async_logger.hpp"
#include "engine_config.hpp"
#include "order_book.hpp"
#include "position_manager.hpp"
#include "spsc_queue.hpp"

class StrategyEngine
{
public:
    StrategyEngine(SpscQueue<MarketTick, engine_config::kTickQueueSize> &queue,
                   LimitOrderBook &book,
                   PositionManager &position_manager,
                   AsyncLogger &logger) noexcept;

    StrategyEngine(const StrategyEngine &) = delete;
    StrategyEngine &operator=(const StrategyEngine &) = delete;

    void run();
    void stop() noexcept;

private:
    SpscQueue<MarketTick, engine_config::kTickQueueSize> &queue_;
    LimitOrderBook &book_;
    PositionManager &position_manager_;
    AsyncLogger &logger_;
    std::atomic<bool> running_{true};
};
