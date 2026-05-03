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
    # Keep a constant 2-cent spread and move the mid-price around.
    mid = 0.58
    spread = 0.02
    bid = mid - spread / 2.0
    ask = mid + spread / 2.0

    # Size imbalance is what drives microprice momentum in our toy strategy.
    # Important: StrategyEngine processes individual MarketTick updates, so we want
    # the microprice to jump by > 1 cent on a *single* update to reliably trigger trades.
    # We do that by flipping only the best-bid size while keeping the best-ask size fixed.
    phase_len = 200  # number of messages per phase
    bid_big = 5000
    bid_small = 10
    ask_fixed = 1000
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
        i = 0
        while True:
            phase = (i // phase_len) % 2
            bid_size = bid_big if phase == 0 else bid_small
            ask_size = ask_fixed

            # Clear the previous levels so the book doesn't accumulate depth forever.
            # This keeps best bid/ask consistent with a single-level-per-side toy feed.
            await ws.send(
                make_l2(
                    bids=[[fmt_price(prev_bid), "0"], [fmt_price(bid), str(bid_size)]],
                    asks=[[fmt_price(prev_ask), "0"], [fmt_price(ask), str(ask_size)]],
                )
            )

            # Bid update
            await asyncio.sleep(0)  # yield, but do not block meaningfully

            # Ask update
            await asyncio.sleep(0)

            # Move mid-price in the direction implied by the imbalance phase.
            # This makes the toy momentum strategy less trivially loss-making.
            prev_bid = bid
            prev_ask = ask

            step = 0.01 if phase == 0 else -0.01
            mid = mid + step

            # Clamp to avoid drifting out of our 1..99 cent book range.
            if mid > 0.90:
                mid = 0.90
            if mid < 0.10:
                mid = 0.10

            bid = mid - spread / 2.0
            ask = mid + spread / 2.0

            i += 1

            # Slow down a touch so you can read output.
            await asyncio.sleep(0.001)
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
