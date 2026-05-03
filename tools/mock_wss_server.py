#!/usr/bin/env python3

import asyncio
import json
import ssl
from typing import Any, Dict

import websockets
from websockets.exceptions import ConnectionClosed


def fmt_price(p: float) -> str:
    # Real L2 feeds commonly send numeric fields as strings.
    # Keep two decimals to map directly into integer cents.
    return f"{p:.2f}"


def make_l2(bids, asks) -> str:
    # Schema:
    #   {"bids":[["0.57","1200"],...],"asks":[["0.59","500"],...]}
    msg: Dict[str, Any] = {"bids": bids, "asks": asks}
    return json.dumps(msg, separators=(",", ":"))


async def handler(ws) -> None:
    # websockets v15+ passes a single connection object.
    # We accept any target/path.
    # Emit alternating bid/ask updates that will occasionally create a tight spread.
    bid = 0.57
    ask = 0.59
    size = 100
    prev_bid = bid
    prev_ask = ask

    async def drain_incoming() -> None:
        try:
            async for _ in ws:
                pass
        except ConnectionClosed:
            return

    drain_task = asyncio.create_task(drain_incoming())

    try:
        while True:
            # Clear the previous levels so the book doesn't accumulate depth forever.
            # This keeps best bid/ask consistent with a single-level-per-side toy feed.
            await ws.send(
                make_l2(
                    bids=[[fmt_price(prev_bid), "0"], [fmt_price(bid), str(size)]],
                    asks=[[fmt_price(prev_ask), "0"], [fmt_price(ask), str(size)]],
                )
            )

            # Bid update
            await asyncio.sleep(0)  # yield, but do not block meaningfully

            # Ask update
            await asyncio.sleep(0)

            # Walk prices a bit to vary book state.
            prev_bid = bid
            prev_ask = ask
            bid += 0.01
            ask += 0.01
            if bid >= 0.90:
                bid = 0.10
                ask = 0.12

            # Slow down a touch so you can read output.
            await asyncio.sleep(0.01)
    except ConnectionClosed:
        # Normal shutdown: client closed the websocket (e.g. Ctrl+C on engine).
        return
    finally:
        drain_task.cancel()


async def main() -> None:
    host = "127.0.0.1"
    port = 8765

    ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ssl_ctx.load_cert_chain(certfile="tools/cert.pem", keyfile="tools/key.pem")

    async with websockets.serve(handler, host, port, ssl=ssl_ctx):
        print(f"mock wss server listening on wss://{host}:{port}/")
        while True:
            await asyncio.sleep(1)


if __name__ == "__main__":
    asyncio.run(main())
