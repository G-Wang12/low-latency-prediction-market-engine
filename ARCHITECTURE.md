# Engine Architecture (Current Skeleton)

This repo is currently a minimal, low-latency **market-data → strategy** skeleton. It builds and runs, and the core components are wired end-to-end, but it is **not yet a complete production Polymarket/Kalshi execution engine** (no auth/subscribe, no order management, no risk, no reconnect logic).

The design goal is to keep hot paths predictable:

- fixed-size data structures (cache-friendly)
- single-producer/single-consumer handoff (no mutexes)
- busy-wait strategy loop (no scheduler wakeup latency)

## High-Level Data Flow

Two threads, one unidirectional pipeline:

1. **Network thread**
   - Connect to a `wss://` endpoint using Boost.Asio + Boost.Beast.
   - Read websocket frames into a fixed-capacity buffer.
   - Parse JSON payload into a small `MarketTick` struct.
   - Push the tick into a lock-free SPSC queue.

2. **Strategy thread**
   - Busy-wait (spin) on the queue.
   - Pop ticks, timestamp, update the order book.
   - Compute best bid/ask spread.
   - If spread is tight, trigger a mock "execution" and print tick→decision latency in microseconds.

## Components

### `MarketTick` and `LimitOrderBook`

- Location: `include/order_book.hpp`
- `MarketTick` is the internal normalized market-data unit:
  - `price`: integer cents (1..99)
  - `size`: resting size at that price level
  - `is_bid`: bid vs ask

- `LimitOrderBook` stores **price-level sizes** (not individual orders):
  - `std::array<uint32_t, 100> bids_` and `asks_`
  - Index is `price` (1..99); slot 0 is unused

Operations:

- `apply_tick(tick)`: O(1) update `bids_[price] = size` or `asks_[price] = size`
- `get_best_bid()`: scan 99→1
- `get_best_ask()`: scan 1→99

This is intentionally simple because prediction-market prices are discretized into a tiny fixed range.

### Lock-Free Queue (SPSC)

- Location: `include/spsc_queue.hpp`
- Purpose: hand off ticks from network thread to strategy thread without locks.

Properties:

- bounded ring buffer (`Size` must be a power of 2)
- `std::atomic<size_t>` read/write indices
- `push()` uses acquire/release ordering (returns false when full)
- `pop()` uses acquire/release ordering (returns false when empty)
- internal storage aligned to cache line size to reduce false sharing

Why SPSC works here:

- there is exactly **one producer** (`WebSocketClient`) and **one consumer** (`StrategyEngine`)
- the queue is the boundary so the order book stays single-writer (strategy thread only)

### Market Data Parser (simdjson on-demand)

- Location: `include/market_parser.hpp`
- Purpose: convert raw JSON bytes into `MarketTick`.

Constraints implemented:

- holds a pre-allocated `simdjson::ondemand::parser` as a member
- uses a fixed padded buffer (`std::array<char, ...>`) for parsing
- `parse_tick(std::string_view payload, MarketTick& out)` returns `true/false` (no exceptions)
- expects fields named `"price"`, `"size"`, `"side"`

Note: real venue payload schemas will likely differ. If your feed doesn’t match this schema, ticks will be dropped (parser returns false).

### WebSocket Client

- Header: `include/websocket_client.hpp`
- Implementation: `src/websocket_client.cpp`

Responsibilities:

- async resolve → TCP connect → TLS handshake → websocket handshake
- continuous `async_read()` loop
- read payload into a fixed-capacity Beast `flat_static_buffer<4096>`
- parse using `MarketParser`; on success, `queue.push(tick)`
- log errors to `std::cerr` (no exception throwing in hot path)

Current limitations:

- no auth/subscription message is sent after handshake
- no ping/pong or reconnect/backoff logic
- message size is capped by the static read buffer

### Strategy Engine

- Header: `include/strategy_engine.hpp`
- Implementation: `src/strategy_engine.cpp`

Responsibilities:

- `run()` is a `while (running_)` loop
- busy-waits on `queue.pop(tick)` (no `sleep_for`)
- when a tick is popped:
  - record time (`t_pop`)
  - `book.apply_tick(tick)`
  - compute `spread = best_ask - best_bid`
  - if `spread <= 2`, record time again and print latency in microseconds

The `spread <= 2` condition is a **placeholder trigger** to demonstrate decision timing. Replace with real strategy logic later.

## Wiring / Threading

- Location: `src/main.cpp`

What happens on startup:

- create shared objects: `LimitOrderBook`, `SpscQueue<MarketTick,1024>`, `io_context`, `ssl::context`
- create `StrategyEngine` (consumer) and `WebSocketClient` (producer)
- start network thread: `io_context.run()`
- start strategy thread: `StrategyEngine::run()`

Linux-only (optional): pin threads to specific CPU cores under `#ifdef __linux__`.

## Shutdown

- `SIGINT` / `SIGTERM` flips a global stop flag.
- main thread stops strategy loop and requests websocket close (best-effort), then stops the `io_context`.
- both threads are joined.

## How to Run

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(getconf _NPROCESSORS_ONLN)
```

Run:

```bash
./build/engine <host> <port> <target>
# Example:
./build/engine example.com 443 /ws
```

If your endpoint sends JSON frames matching:

```json
{ "price": 0.57, "size": 123, "side": "bid" }
```

then the strategy thread will eventually print lines like:

```
mock_exec latency_us=12 spread=2 bid=57 ask=59
```

## Next Steps (Practical)

1. Add venue-specific subscribe/auth write after websocket handshake.
2. Adapt `MarketParser` to the venue’s real message schema.
3. Add reconnection, ping/pong, and proper TLS verification policies.
4. Add order management + risk checks, and replace mock execution with real order sends.
