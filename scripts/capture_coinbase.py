#!/usr/bin/env python3
"""Record raw Coinbase Exchange ws-feed traffic to JSON Lines.

Each output line is {"recv_ns": <wall-clock ns at receive>, "msg": <raw frame text>}.
The raw frame is kept as a string so replay sees exactly the bytes Coinbase sent.

Usage:
  uv run --with websockets scripts/capture_coinbase.py \
      --channels ticker heartbeat level2_batch --seconds 300 --out data/capture.jsonl
"""

import argparse
import asyncio
import json
import sys
import time

import websockets

WS_URI = "wss://ws-feed.exchange.coinbase.com"


async def capture(products, channels, seconds, out_path):
    sub = {"type": "subscribe", "product_ids": products, "channels": channels}
    counts: dict[str, int] = {}
    deadline = time.monotonic() + seconds
    async with websockets.connect(WS_URI, max_size=None) as ws:
        await ws.send(json.dumps(sub))
        with open(out_path, "w", encoding="utf-8") as out:
            while (remaining := deadline - time.monotonic()) > 0:
                try:
                    frame = await asyncio.wait_for(ws.recv(), timeout=remaining)
                except asyncio.TimeoutError:
                    break
                recv_ns = time.time_ns()
                text = frame.decode() if isinstance(frame, bytes) else frame
                out.write(json.dumps({"recv_ns": recv_ns, "msg": text}) + "\n")
                mtype = json.loads(text).get("type", "?")
                counts[mtype] = counts.get(mtype, 0) + 1
                if mtype in ("error", "subscriptions"):
                    print(text, file=sys.stderr)
    return counts


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--products", nargs="+", default=["BTC-USD", "ETH-USD", "ADA-USD"])
    p.add_argument("--channels", nargs="+", default=["ticker", "heartbeat"])
    p.add_argument("--seconds", type=float, default=60)
    p.add_argument("--out", required=True)
    a = p.parse_args()
    counts = asyncio.run(capture(a.products, a.channels, a.seconds, a.out))
    total = sum(counts.values())
    print(f"{total} messages in {a.seconds:g}s -> {a.out}", file=sys.stderr)
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {k:12} {v:8}  ({v / a.seconds:.1f}/s)", file=sys.stderr)


if __name__ == "__main__":
    main()
