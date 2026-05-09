#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>

#include "async_logger.hpp"
#include "engine_config.hpp"
#include "order_book.hpp"
#include "position_manager.hpp"
#include "spsc_queue.hpp"
#include "strategy_engine.hpp"
#include "websocket_client.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace
{
    std::atomic<bool> g_stop{false};

    void on_signal(int) noexcept
    {
        g_stop.store(true, std::memory_order_relaxed);
    }

#ifdef __linux__
    void pin_thread_to_core(std::thread &t, int core_id) noexcept
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        (void)pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset);
    }
#endif
} // namespace

int main(int argc, char **argv)
{
    // Endpoint config: host port target
    // Example:
    //   ./engine example.com 443 /ws
    const std::string host = (argc > 1) ? std::string(argv[1]) : std::string("example.com");
    const std::string port = (argc > 2) ? std::string(argv[2]) : std::string("443");
    const std::string target = (argc > 3) ? std::string(argv[3]) : std::string("/");

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    LimitOrderBook book;
    SpscQueue<MarketTick, engine_config::kTickQueueSize> tick_queue;

    PositionManager position_manager;
    AsyncLogger logger; // writes to trading_log.csv by default

    boost::asio::io_context ioc;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tls_client);

    // In a real venue adapter you should verify the peer and load CA roots.
    // Keep config minimal for now; failures are reported by WebSocketClient.
    ssl_ctx.set_default_verify_paths();

    StrategyEngine strategy(tick_queue, book, position_manager, logger);
    WebSocketClient ws_client(ioc, ssl_ctx, tick_queue);

    // Kick off async resolve/connect/handshakes.
    ws_client.run(host, port, target);

    std::thread network_thread([&ioc]()
                               { ioc.run(); });

    std::thread strategy_thread([&strategy]()
                                { strategy.run(); });

#ifdef __linux__
    // Bonus: pin threads to dedicated cores.
    // Core IDs are 0-based; request Core 1 for network and Core 2 for strategy.
    pin_thread_to_core(network_thread, 1);
    pin_thread_to_core(strategy_thread, 2);
#endif

    while (!g_stop.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Clean shutdown: stop strategy loop and stop network event loop.
    strategy.stop();

    // Ask websocket to close on the io_context thread (best-effort).
    boost::asio::post(ioc, [&ws_client]()
                      { ws_client.close(); });

    ioc.stop();

    if (strategy_thread.joinable())
    {
        strategy_thread.join();
    }
    if (network_thread.joinable())
    {
        network_thread.join();
    }

    std::cout << "Shutdown complete." << std::endl;
    return EXIT_SUCCESS;
}
