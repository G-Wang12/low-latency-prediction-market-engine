#!/usr/bin/env python3

import asyncio
import json
import os
import ssl
from pathlib import Path
from typing import Any, Optional

import websockets
from websockets.exceptions import ConnectionClosed


HISTORICAL_PATH = Path("historical_data.jsonl")

# Preserve burstiness, but avoid multi-minute gaps when recordings contain mixed
# markets or time discontinuities.
REPLAY_MAX_SLEEP_S = float(os.environ.get("REPLAY_MAX_SLEEP_S", "0.5"))


def _extract_local_timestamp_ns(line: str) -> Optional[int]:
    """Extract the local timestamp we added during recording.

    Returns None if the line isn't valid JSON or the field is missing.
    """
    try:
        obj: Any = json.loads(line)
    except json.JSONDecodeError:
        return None

    if not isinstance(obj, dict):
        return None

    ts = obj.get("local_timestamp_ns")
    if ts is None:
        return None

    try:
        return int(ts)
    except (TypeError, ValueError):
        return None


def _extract_replay_payload(line: str) -> str:
    """Return the websocket payload to replay.

    We record JSONL with additional fields (e.g. local_timestamp_ns, raw_message).
    For true replay fidelity, prefer the original inbound websocket text when
    it exists (raw_message/raw), otherwise fall back to the JSONL line.
    """
    try:
        obj: Any = json.loads(line)
    except json.JSONDecodeError:
        return line

    if not isinstance(obj, dict):
        return line

    raw_message = obj.get("raw_message")
    if isinstance(raw_message, str) and raw_message:
        return raw_message

    raw = obj.get("raw")
    if isinstance(raw, str) and raw:
        return raw

    return line


async def handler(ws) -> None:
    # websockets v15+ passes a single connection object.
    # We accept any target/path.

    async def drain_incoming() -> None:
        # Many clients send a subscribe message after connect.
        # We don't need it for replay, but draining keeps the connection healthy.
        try:
            async for _ in ws:
                pass
        except ConnectionClosed:
            return

    drain_task = asyncio.create_task(drain_incoming())

    try:
        while True:
            if not HISTORICAL_PATH.exists():
                raise FileNotFoundError(
                    f"{HISTORICAL_PATH} not found. Run tools/record_polymarket.py first (or place the file in repo root)."
                )

            prev_ts_ns: Optional[int] = None

            with HISTORICAL_PATH.open("r", encoding="utf-8") as f:
                for raw_line in f:
                    line = raw_line.rstrip("\n")
                    if not line:
                        continue

                    ts_ns = _extract_local_timestamp_ns(line)
                    if prev_ts_ns is not None and ts_ns is not None:
                        delta_ns = ts_ns - prev_ts_ns
                        if delta_ns > 0:
                            await asyncio.sleep(min(delta_ns / 1_000_000_000.0, REPLAY_MAX_SLEEP_S))

                    await ws.send(_extract_replay_payload(line))

                    if ts_ns is not None:
                        prev_ts_ns = ts_ns

            # EOF: loop back to the beginning.
    except ConnectionClosed:
        # Normal shutdown: client closed the websocket.
        return
    finally:
        drain_task.cancel()


async def main() -> None:
    host = "127.0.0.1"
    port = 8765

    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(certfile="tools/cert.pem", keyfile="tools/key.pem")

    async with websockets.serve(handler, host, port, ssl=ssl_ctx):
        print(f"replay wss server listening on wss://{host}:{port}/")
        print(f"replaying from: {HISTORICAL_PATH}")
        while True:
            await asyncio.sleep(1)


if __name__ == "__main__":
    asyncio.run(main())
