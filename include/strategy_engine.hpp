#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "async_logger.hpp"
#include "alpha_base.hpp"
#include "engine_config.hpp"
#include "order_book.hpp"
#include "position_manager.hpp"
#include "spsc_queue.hpp"

enum class StrategySelection
{
    momentum,
    ofi,
    both,
};

class StrategyEngine
{
public:
    StrategyEngine(SpscQueue<MarketTick, engine_config::kTickQueueSize> &queue,
                   LimitOrderBook &book,
                   PositionManager &position_manager,
                   AsyncLogger &logger,
                   StrategySelection selection);

    StrategyEngine(const StrategyEngine &) = delete;
    StrategyEngine &operator=(const StrategyEngine &) = delete;

    void run();
    void stop() noexcept;

private:
    static constexpr double kTradeThreshold = 0.60;
    static constexpr int kDefaultTradeSize = 10;

    SpscQueue<MarketTick, engine_config::kTickQueueSize> &queue_;
    LimitOrderBook &book_;
    PositionManager &position_manager_;
    AsyncLogger &logger_;
    std::atomic<bool> running_{true};

    std::vector<std::unique_ptr<IAlphaSignal>> signals_;
    bool emitted_initial_p_{false};
};
