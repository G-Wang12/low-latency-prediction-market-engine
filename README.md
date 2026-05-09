# Low Latency Prediction Market Engine

A high-performance C++20 execution engine designed for low-latency prediction market trading, order matching, and real-time market data processing. Optimized for sub-microsecond critical paths using native compilation, SIMD JSON parsing, and Boost.Asio networking.

**Current Status**: Foundational infrastructure (CMake + core dependencies). Ready for extension with matching engine, risk systems, and venue adapters.

## Features

- **C++20** with strict compiler enforcement (`-Wall -Wextra -Wpedantic -Werror`)
- **Performance**: Release builds use `-O3 -march=native` for maximum speed on target hardware
- **Zero-copy JSON**: [simdjson](https://github.com/simdjson/simdjson) for ultra-fast market data ingestion
- **Networking & Concurrency**: Boost.System + Boost.Thread (extendable to Boost.Asio)
- **Testing**: GoogleTest integration with CMake FetchContent
- **Build System**: Modern CMake 3.20+ with FetchContent for dependencies

## Quick Start

### Prerequisites

- CMake ≥ 3.20
- C++20 compiler (GCC 11+, Clang 13+, MSVC 2019+)
- Boost (system + thread components) — install via:
  ```bash
  # macOS
  brew install boost
  # Ubuntu
  sudo apt install libboost-system-dev libboost-thread-dev
  ```
- Git

### Build & Run

```bash
# Clone and configure
git clone <your-repo-url>
cd low-latency-prediction-market-engine

# Clean previous build (important after dependency/search-path changes)
rm -rf build

# Configure (uses FetchContent for simdjson + GoogleTest)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release

# Build (engine + tests)
cmake --build build -j$(getconf _NPROCESSORS_ONLN)

# Run the engine
./build/engine

# Run tests
ctest --test-dir build -V
```

### Recommended Environment Setup (Conda + Python tooling)

This repo has two independent “worlds”:

- **C++ build/run** (CMake + Clang/GCC + system/Homebrew libs)
- **Python tooling** (used for local websocket tooling in `tools/`: recorder, replay server, mock server, dashboard)

To keep the C++ toolchain deterministic on macOS, it’s recommended to **deactivate Conda `base`** in the terminal you use for building/running C++:

```bash
conda deactivate
```

For the Python mock server, use a repo-local virtualenv instead of installing packages into Conda `base`:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip websockets certifi
```

Practical workflow: run the Python server in one terminal (with `(.venv)` active), and build/run the engine in another terminal (with neither `(.venv)` nor `(base)` active).

### Recording real Polymarket data (JSONL)

The recorder in `tools/record_polymarket.py` connects to Polymarket’s public Market Channel websocket and appends every incoming JSON message to `historical_data.jsonl`.

1. Ensure the repo-local venv has `websockets` installed:

```bash
source .venv/bin/activate
python -m pip install -U pip websockets certifi
```

2. Edit `ASSET_IDS` in `tools/record_polymarket.py` (these are Polymarket _asset IDs / token IDs_).

3. Run the recorder:

```bash
source .venv/bin/activate
python tools/record_polymarket.py
```

### Replaying recorded Polymarket data (local TLS websocket)

The replay server in `tools/replay_server.py` serves `historical_data.jsonl` over a local TLS websocket and replays messages in order.

It prefers replaying the original on-the-wire websocket payload when available (`raw_message` / `raw` fields), and uses `local_timestamp_ns` to reproduce short-term burstiness.

1. Ensure the repo-local venv has dependencies installed:

```bash
source .venv/bin/activate
python -m pip install -U pip websockets
```

2. Ensure you have a local TLS cert/key for the server:

```bash
./tools/gen_self_signed_cert.sh
```

This generates `tools/cert.pem` and `tools/key.pem` (used by the replay server).

3. Run the replay server:

```bash
source .venv/bin/activate
python tools/replay_server.py
```

Optional: cap inter-message sleep time to avoid multi-minute gaps if your JSONL contains time discontinuities (default is `0.5` seconds):

```bash
REPLAY_MAX_SLEEP_S=0.1 python tools/replay_server.py
```

4. Point the engine at the replay server:

```bash
./build/engine 127.0.0.1 8765 /
```

At this point `trading_log.csv` should start accumulating rows, and the Streamlit dashboard should stop showing the “headers only” warning.

### Real-time Dashboard (Streamlit)

The Streamlit dashboard reads a CSV file named `trading_log.csv` (by default) with columns:

`timestamp_us,event_type,price,size,realized_pnl`

1. Install dashboard deps (recommended inside the repo-local `(.venv)`):

```bash
source .venv/bin/activate
python -m pip install -U streamlit pandas plotly streamlit-autorefresh
```

2. Run the dashboard:

```bash
streamlit run tools/dashboard.py
```

3. Point it at a different log file (optional):

```bash
TRADING_LOG_PATH=/path/to/trading_log.csv streamlit run tools/dashboard.py
```

Note: the dashboard will show a warning until `trading_log.csv` exists and has data.

Optional (quick sanity check): generate a tiny sample log file:

```bash
python - <<'PY'
import csv
import time

rows = [
  (0, 'P', 0.0, 0, 0.0),
  (500_000, 'T', 0.59, 10, 0.0),
  (1_000_000, 'P', 0.0, 0, 0.12),
  (1_500_000, 'T', 0.57, -10, 0.12),
  (2_000_000, 'P', 0.0, 0, 0.20),
]

with open('trading_log.csv', 'w', newline='') as f:
  w = csv.writer(f)
  w.writerow(['timestamp_us', 'event_type', 'price', 'size', 'realized_pnl'])
  base = int(time.time() * 1_000_000)
  for t, et, price, size, pnl in rows:
    w.writerow([base + t, et, price, size, pnl])
print('wrote trading_log.csv')
PY
```

**Note for macOS users in conda `(base)`**: conda often injects search paths that can cause a mixed Boost install to be detected (e.g., Homebrew BoostConfig + conda `boost_system`). The build defaults to ignoring `CONDA_PREFIX` during dependency discovery; override with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLPME_IGNORE_CONDA_PREFIX=OFF
```

**Expected output** (from `./build/engine`) includes the startup banner confirming optimizations and readiness.

## Project Structure

```
.
├── CMakeLists.txt          # Root build configuration (strict flags, deps)
├── include/                # Public API headers (orderbook, engine interfaces)
├── src/
│   └── main.cpp            # Entry point
├── tests/
│   └── test_main.cpp       # GoogleTest suite
├── tools/
│   ├── mock_wss_server.py   # Local TLS websocket tick generator
│   ├── record_polymarket.py  # Record Polymarket Market Channel JSONL
│   ├── replay_server.py      # Replay historical_data.jsonl over local WSS
│   └── dashboard.py         # Streamlit dashboard for trading_log.csv
├── .gitignore
├── README.md
└── build/                  # Generated (ignored)
```

## Development

### Adding Components

1. Place headers in `include/`
2. Implementation in `src/`
3. Tests in `tests/`
4. Update `CMakeLists.txt` targets as modules grow (consider `add_library` for core engine)

### Testing

```bash
# After building (see Build & Run)
ctest --test-dir build -V
```

### Performance Tuning

- Profile with `perf`, Valgrind, or Tracy
- Ensure `-march=native` matches production hardware
- Monitor cache misses and branch prediction in hot paths (order matching, risk checks)

## Dependencies (Managed by CMake)

- **simdjson** (v3.6.3): Zero-allocation JSON parsing
- **GoogleTest** (v1.15.2): Unit testing
- **Boost**: System + Thread (local install required; extend with Asio, Beast, etc.)

## GitHub Setup

This repository includes:

- Comprehensive `.gitignore` for C++/CMake/IDE artifacts
- GitHub Actions CI (see `.github/workflows/ci.yml` — add for automated builds/tests across platforms)
- Modern CMake with dependency management and macOS/conda-friendly Boost discovery

### CI/CD Recommendations

- Add GitHub Actions workflow for:
  - Linux (Ubuntu) + macOS matrix
  - Release builds with sanitizers (`-fsanitize=address,undefined`)
  - Benchmarking and performance regression tests
- Use `clang-tidy` and `include-what-you-use` for static analysis
- Consider `conan` or `vcpkg` for full dependency management in larger projects

## Roadmap

- [ ] Order book implementation with lock-free data structures
- [ ] Real-time market data adapter (WebSocket + simdjson)
- [ ] Matching engine with low-latency priority queues
- [ ] Risk management and position tracking
- [ ] Benchmark suite (latency histograms, throughput)
- [ ] Python bindings (via pybind11) for research

## License

This project is currently unlicensed (all rights reserved).

## Contributing

Not currently accepting external contributions.

---

_Built for high-frequency prediction market execution. Questions? Open an issue._
