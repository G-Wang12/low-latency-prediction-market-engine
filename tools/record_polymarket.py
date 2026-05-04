#!/usr/bin/env python3
"""Record Polymarket CLOB Market Channel websocket messages to JSONL.

This is the standard "build your own historical data" workflow:
- Connect to Polymarket's public market data websocket.
- Subscribe to one (or more) asset IDs (aka token IDs).
- Append every raw JSON message to a JSON Lines file, with a local timestamp.

Docs (Market Channel):
- Endpoint: wss://ws-subscriptions-clob.polymarket.com/ws/market
- Subscribe payload:
  {
    "assets_ids": ["<token_id_1>", ...],
    "type": "market",
    "custom_feature_enabled": true,
    "initial_dump": true,
    "level": 2
  }

Note: Polymarket uses asset IDs / token IDs for websocket subscriptions.
The phrase "market_id" is commonly used informally, but the subscribe message
expects `assets_ids`.
"""

from __future__ import annotations

import asyncio
import json
import os
import signal
import ssl
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Optional

import websockets
from websockets.exceptions import ConnectionClosed

try:
    # websockets>=15
    from websockets.asyncio.client import ClientConnection
except Exception:  # pragma: no cover
    ClientConnection = Any  # type: ignore[assignment]

WS_URL = "wss://ws-subscriptions-clob.polymarket.com/ws/market"

# --- Configuration ---
# Replace with a real token/asset id from Polymarket (string).
# You can subscribe to multiple assets, but start with one.
ASSET_IDS = [
    "96351650250139397447438653380483970772060142397849794315678720298272472897874",
]

SUB_LEVEL = 2
CUSTOM_FEATURE_ENABLED = True
INITIAL_DUMP = True

OUTPUT_PATH = Path("historical_data.jsonl")

# If True, include the exact inbound websocket message text as `raw_message`.
# This increases file size, but makes later replay/debugging lossless.
INCLUDE_RAW_MESSAGE = True

# Polymarket docs specify sending a text "PING" every 10 seconds.
PING_INTERVAL_S = 10

# Reconnect backoff
BACKOFF_INITIAL_S = 0.5
BACKOFF_MAX_S = 30.0

# Flush file periodically so you don't lose much on crash.
FLUSH_EVERY_N_LINES = 200


def _build_ssl_context() -> Optional[ssl.SSLContext]:
    """Build an SSL context for wss connections.

    On macOS, some Python installs / virtualenvs don't have access to a usable
    system CA store, which can lead to CERTIFICATE_VERIFY_FAILED.

    Priority order:
    1) If POLYMARKET_INSECURE_SKIP_VERIFY is set, disable verification (not recommended).
    2) If POLYMARKET_CA_BUNDLE is set, use that CA bundle file.
    3) If certifi is installed, use its CA bundle.
    4) Fallback to Python defaults.
    """

    insecure = os.getenv("POLYMARKET_INSECURE_SKIP_VERIFY", "").strip().lower() in {
        "1",
        "true",
        "yes",
        "y",
    }
    if insecure:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx

    ca_bundle = os.getenv("POLYMARKET_CA_BUNDLE")
    if ca_bundle:
        return ssl.create_default_context(cafile=ca_bundle)

    try:
        import certifi  # type: ignore

        return ssl.create_default_context(cafile=certifi.where())
    except Exception:
        return ssl.create_default_context()


@dataclass(frozen=True)
class RecorderStats:
    messages: int = 0
    bytes: int = 0


def _compact_json(obj: Any) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


async def _ping_loop(ws: ClientConnection, stop: asyncio.Event) -> None: # type: ignore
    """Send the required text PING keepalive."""
    while not stop.is_set():
        try:
            await ws.send("PING")
        except Exception:
            return
        try:
            await asyncio.wait_for(stop.wait(), timeout=PING_INTERVAL_S)
        except asyncio.TimeoutError:
            continue


async def _record_once(stop: asyncio.Event, out_path: Path) -> RecorderStats:
    """Connect once, subscribe, and record until the connection drops or stop is set."""
    subscribe_payload: Dict[str, Any] = {
        "assets_ids": ASSET_IDS,
        "type": "market",
        "custom_feature_enabled": CUSTOM_FEATURE_ENABLED,
        "initial_dump": INITIAL_DUMP,
        "level": SUB_LEVEL,
    }

    ssl_context = _build_ssl_context()

    # Disable library-level ping/pong since the server expects application-level PING/PONG.
    async with websockets.connect(WS_URL, ping_interval=None, ssl=ssl_context) as ws:
        await ws.send(_compact_json(subscribe_payload))
        print(f"subscribed assets_ids={ASSET_IDS} level={SUB_LEVEL}")

        ping_stop = asyncio.Event()
        ping_task = asyncio.create_task(_ping_loop(ws, ping_stop))

        stats = RecorderStats(messages=0, bytes=0)
        lines_since_flush = 0

        try:
            with out_path.open("a", encoding="utf-8") as f:
                while not stop.is_set():
                    try:
                        msg = await ws.recv()
                    except ConnectionClosed:
                        break

                    if not isinstance(msg, str):
                        # websockets can return bytes; keep it but wrap it.
                        payload: Dict[str, Any] = {
                            "local_timestamp_ns": time.time_ns(),
                            "raw_bytes": msg.hex(),
                            "raw_is_bytes": True,
                        }
                        line = _compact_json(payload)
                        msg_len = len(msg)
                    else:
                        if msg == "PONG":
                            continue

                        # Attach local timestamp (machine receipt time). Keep original fields intact.
                        try:
                            payload = json.loads(msg)
                            if isinstance(payload, dict):
                                payload["local_timestamp_ns"] = time.time_ns()
                                payload["raw_is_bytes"] = False
                                if INCLUDE_RAW_MESSAGE:
                                    payload["raw_message"] = msg
                                line = _compact_json(payload)
                            else:
                                # Unexpected: JSON but not an object
                                line = _compact_json(
                                    {
                                        "local_timestamp_ns": time.time_ns(),
                                        "raw": msg,
                                        "raw_is_bytes": False,
                                        "note": "non-object JSON payload",
                                    }
                                )
                        except json.JSONDecodeError:
                            # Unexpected: non-JSON message
                            line = _compact_json(
                                {
                                    "local_timestamp_ns": time.time_ns(),
                                    "raw": msg,
                                    "raw_is_bytes": False,
                                    "note": "non-JSON payload",
                                }
                            )
                        msg_len = len(msg)

                    f.write(line)
                    f.write("\n")
                    stats = RecorderStats(messages=stats.messages + 1, bytes=stats.bytes + msg_len)

                    lines_since_flush += 1
                    if lines_since_flush >= FLUSH_EVERY_N_LINES:
                        f.flush()
                        lines_since_flush = 0
        finally:
            ping_stop.set()
            try:
                await ping_task
            except Exception:
                pass

        return stats


async def main() -> None:
    if not ASSET_IDS or ASSET_IDS == ["<PUT_REAL_ASSET_ID_HERE>"]:
        raise SystemExit(
            "Set ASSET_IDS at the top of tools/record_polymarket.py to a real Polymarket asset_id/token_id."
        )

    stop = asyncio.Event()
    loop = asyncio.get_running_loop()

    def _request_stop() -> None:
        stop.set()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, _request_stop)
        except NotImplementedError:
            # Fallback for environments where add_signal_handler isn't supported.
            signal.signal(sig, lambda *_: stop.set())

    backoff = BACKOFF_INITIAL_S
    print(f"recording to {OUTPUT_PATH} (append mode)")
    print(f"connecting to {WS_URL}")

    while not stop.is_set():
        try:
            stats = await _record_once(stop, OUTPUT_PATH)
            if stats.messages > 0:
                mb = stats.bytes / (1024.0 * 1024.0)
                print(f"connection ended; recorded messages={stats.messages} bytes={stats.bytes} ({mb:.2f} MiB)")
        except Exception as exc:
            print(f"connection error: {type(exc).__name__}: {exc}")

        if stop.is_set():
            break

        print(f"reconnecting in {backoff:.1f}s...")
        try:
            await asyncio.wait_for(stop.wait(), timeout=backoff)
        except asyncio.TimeoutError:
            pass
        backoff = min(backoff * 2.0, BACKOFF_MAX_S)

    print("shutdown requested; exiting")


if __name__ == "__main__":
    asyncio.run(main())
