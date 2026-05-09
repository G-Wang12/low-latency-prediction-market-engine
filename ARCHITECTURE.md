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

- Generate a mock "execution" decision and log events to `trading_log.csv` (via `AsyncLogger`).

## Components

### `MarketTick` and `LimitOrderBook`

- Location: `include/order_book.hpp`
- `MarketTick` is the internal normalized market-data unit:
  - `price`: integer cents (0..100)
  - `size`: resting size at that price level (0 means delete)
  - `is_bid`: bid vs ask

- `LimitOrderBook` stores **price-level sizes** (not individual orders):
  - `std::array<uint32_t, 101> bids_` and `asks_`
  - Index is `price` (0..100); all slots are valid

Operations:

- `apply_tick(tick)`: O(1) update `bids_[price] = size` or `asks_[price] = size`
- `get_best_bid()`: scan 100→0 and return the highest level with `size > 0` (returns `kNoBid == 255` when empty)
- `get_best_ask()`: scan 0→100 and return the lowest level with `size > 0` (returns `kNoAsk == 255` when empty)

Deletion semantics (L2 feeds):

- exchanges commonly send `size == 0` to indicate a price level was deleted (filled/canceled)
- this is represented as a `MarketTick` update that sets that level’s stored size to `0`, so subsequent best-price scans naturally skip it

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
- emits **one `MarketTick` per updated price level** (snapshot or delta)
- supports multiple inbound schemas (all numeric values are treated as strings):
  - internal mock feed: `{"bids":[["0.57","1200"],...],"asks":[["0.59","500"]]}`
  - Polymarket Market Channel snapshots: `{"event_type":"book","bids":[{"price":".48","size":"30"},...],"asks":[...]}`
  - Polymarket Market Channel deltas: `{"event_type":"price_change","price_changes":[{"price":"0.5","size":"200","side":"BUY"},...]}`
- accepts either a single message object or a **top-level JSON array** of message objects (batched messages like `[{...},{...}]`)

Snapshot vs deltas (typical L2 feed behavior):

- On connect, venues often send a full **snapshot** of the book (many price levels)
- After that, they send **deltas** (small updates) whenever a level changes

Note: real venue payload schemas will likely differ. If your feed doesn’t match this schema, updates will be dropped.

Important architectural note: the parser normalizes “feed-shaped” messages into a single internal representation (`MarketTick`). This keeps downstream code (queue → book → strategy) feed-agnostic.

### WebSocket Client

- Header: `include/websocket_client.hpp`
- Implementation: `src/websocket_client.cpp`

Responsibilities:

- async resolve → TCP connect → TLS handshake → websocket handshake
- after websocket handshake, send a **subscription** message via `async_write()`
- continuous `async_read()` loop
- read payload into a fixed-capacity Beast `flat_static_buffer<kReadBufferBytes>` (currently 16 KiB)
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
  - `book.apply_tick(tick)`
  - compute best bid/ask and the **microprice** using best-level sizes
  - generate a simple **microprice momentum** signal and trigger mock fills
  - update `PositionManager` and emit CSV events via `AsyncLogger`

Microprice momentum signal (current placeholder strategy):

- microprice (in cents):
  - `microprice = (best_bid * best_ask_size + best_ask * best_bid_size) / (best_bid_size + best_ask_size)`
- if `microprice - prev_microprice > 1.0` (momentum up): BUY 10 at best ask
- if `prev_microprice - microprice > 1.0` (momentum down): SELL 10 at best bid

Logging/PnL:

- on each trade, log a `'T'` event with the fill price/size and current realized PnL
- every 1,000 processed ticks, log a `'P'` event with mark-to-market equity PnL (realized + unrealized) using the current mid-price

### Position & PnL Manager (Mock Trading)

- Location: `include/position_manager.hpp`
- Purpose: track a simulated position and PnL without touching any real exchange execution APIs.

State tracked:

- `position_size` (int): positive = long, negative = short
- `average_entry_price` (double): current VWAP entry for the open position
- `realized_pnl` (double): PnL locked in by closing fills

Key methods:

- `add_fill(fill_size, fill_price)`: updates position size, updates VWAP on increases, and computes realized PnL when a fill reduces/offsets an existing position (including flip-through-zero)
- `get_unrealized_pnl(current_mid_price)`: mark-to-market PnL for the open position if we closed it at the given mid

### Asynchronous Cold-Path Logger

- Location: `include/async_logger.hpp`
- Purpose: write trades/PnL events to disk without blocking the latency-critical strategy thread.

Design:

- strategy thread calls `AsyncLogger::log_event(...)` which does a lock-free `push()` into a dedicated `SpscQueue<LogEvent, 4096>`
- a background I/O thread spins on `pop()` and appends formatted CSV rows to `std::ofstream`
- the file is flushed periodically (every ~1000 events or ~1 second) so external tooling can tail/read it

Output:

- default log file is `trading_log.csv` with columns: `timestamp_us,event_type,price,size,realized_pnl`

Local dashboard:

- see `tools/dashboard.py` for a Streamlit app that polls `trading_log.csv` every second and plots PnL over time (trade events plus periodic mark-to-market updates) with trade price markers

## Real Live Data Recording (Real Venue Capture)

- Location: `tools/record_polymarket.py`
- Purpose: record Polymarket's _live_ market data websocket messages to `historical_data.jsonl` so you can replay them later.

Recorder behavior:

- connects to Polymarket Market Channel: `wss://ws-subscriptions-clob.polymarket.com/ws/market`
- subscribes using `assets_ids` (token IDs), not a "market_id" (see Polymarket docs)
- appends each incoming JSON message as one JSONL line with an extra `local_timestamp_ns` field (machine receipt time)
- preserves the original websocket payload so a replay can reproduce the on-the-wire format (e.g., under `raw` / `raw_message`)
- auto-reconnects on disconnects with exponential backoff

## Offline Replay (Historical Data)

- Location: `tools/replay_server.py`
- Purpose: serve a local `wss://127.0.0.1:8765/` websocket that replays `historical_data.jsonl` back into the C++ engine.

Design constraints:

- Preserve “burstiness” by using recorded `local_timestamp_ns` deltas.
- Avoid pathological stalls if the recording contains timestamp discontinuities (e.g., mixed sessions/markets in one file) by capping maximum inter-message sleep.
- Replay the original websocket payload when possible so the C++ client sees the same JSON shape as it would live.

## Wiring / Threading

- Location: `src/main.cpp`

What happens on startup:

- create shared objects: `LimitOrderBook`, `SpscQueue<MarketTick, engine_config::kTickQueueSize>`, `io_context`, `ssl::context`
- create `StrategyEngine` (consumer) and `WebSocketClient` (producer)
- start network thread: `io_context.run()`
- start strategy thread: `StrategyEngine::run()`

Linux-only (optional): pin threads to specific CPU cores under `#ifdef __linux__`.

## Shutdown

- `SIGINT` / `SIGTERM` flips a global stop flag.
- main thread stops strategy loop and requests websocket close (best-effort), then stops the `io_context`.
- both threads are joined.

## Operational docs

This document describes architecture and invariants. For exact commands and local workflows (build/run/tests, recorder, replay server, Streamlit dashboard), see `README.md`.

## Next Steps (Practical)

1. Add venue-specific subscribe/auth for real execution adapters.
2. Add per-asset routing (separate books) and message sequencing/consistency checks.
3. Add reconnection, ping/pong, and explicit TLS verification policies.
4. Add order management + risk checks, and replace mock execution with real order sends.
