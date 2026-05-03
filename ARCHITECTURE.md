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
- Purpose: convert raw JSON bytes into `MarketTick` updates.

Constraints implemented:

- holds a pre-allocated `simdjson::ondemand::parser` as a member
- uses a fixed padded buffer (`std::array<char, ...>`) for parsing
- `parse_tick(std::string_view payload, queue)` returns `true/false` (no exceptions)
- expects an L2-style schema with nested arrays and **string** numeric fields:
  - `"bids": [["0.57", "1200"], ...]`
  - `"asks": [["0.59", "500"], ...]`
- iterates `bids` and `asks` and **enqueues one `MarketTick` per updated price level** (snapshot or delta)
- converts string price → integer cents (no floats) and string size → integer using `std::from_chars` (no `std::string` allocations)

Snapshot vs deltas (typical L2 feed behavior):

- On connect, venues often send a full **snapshot** of the book (many price levels)
- After that, they send **deltas** (small updates) whenever a level changes

Note: real venue payload schemas will likely differ. If your feed doesn’t match this schema, updates will be dropped.

### WebSocket Client

- Header: `include/websocket_client.hpp`
- Implementation: `src/websocket_client.cpp`

Responsibilities:

- async resolve → TCP connect → TLS handshake → websocket handshake
- after websocket handshake, send a **subscription** message via `async_write()`
- continuous `async_read()` loop
- read payload into a fixed-capacity Beast `flat_static_buffer<16384>`
- parse using `MarketParser`; on success, enqueue one `MarketTick` per updated level
- log errors to `std::cerr` (no exception throwing in hot path)

Current limitations:

- subscription payload is currently a **static placeholder** JSON string (real venues require venue-specific subscribe + often auth)
- no ping/pong or reconnect/backoff logic
- message size is capped by the static read buffer

#### Subscription Handshake (Outbound Write)

Many real exchanges (including prediction markets) will not send any market data until the client explicitly subscribes.

Current behavior:

- after `ws_.async_handshake(...)` completes, `WebSocketClient` calls `do_subscribe()`
- `do_subscribe()` sends a JSON subscription payload using `ws_.async_write()`
- the ingestion loop (`do_read()`) only begins after the write completes successfully

Implementation note:

- the subscription payload buffer is kept alive across the async write by capturing an owning `std::shared_ptr<std::string>` in the completion handler

This is intentionally minimal scaffolding so the next step can be replacing the placeholder JSON with Polymarket's real subscribe/auth schema.

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

## How to Test (Recommended)

You have two layers of testing:

### 1) Unit tests (fast)

```bash
cmake --build build -j$(getconf _NPROCESSORS_ONLN)
ctest --test-dir build -V
```

### 2) End-to-end local test (no external venue required)

This repo includes a tiny local **mock WSS server** that emits JSON ticks in the exact schema expected by `MarketParser`:

1. Create a repo-local Python venv + install deps (one time):

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip websockets
```

2. Generate a local self-signed cert (one time):

```bash
bash tools/gen_self_signed_cert.sh
```

3. Start the mock WSS server:

```bash
python tools/mock_wss_server.py
```

4. In another terminal, run the engine against it:

```bash
./build/engine 127.0.0.1 8765 /
```

If everything is wired correctly, you should see `mock_exec latency_us=...` prints from the strategy thread.

If your endpoint sends JSON frames matching:

```json
{
  "bids": [
    ["0.57", "1200"],
    ["0.56", "300"]
  ],
  "asks": [["0.59", "500"]]
}
```

then the strategy thread will eventually print lines like:

```
mock_exec latency_us=12 spread=2 bid=57 ask=59
```

## Next Steps (Practical)

1. Add venue-specific subscribe/auth write after websocket handshake.
2. Replace the placeholder L2 schema with Polymarket’s real snapshot/delta schema (field names, market identifiers, sequencing).
3. Add reconnection, ping/pong, and proper TLS verification policies.
4. Add order management + risk checks, and replace mock execution with real order sends.
