#!/usr/bin/env python3
"""Reference order book for tests/order_book_test.cpp, independent of the C++ implementation.

Replays (a) the hand-built scenarios the unit tests use and (b) tests/data/coinbase_sample.jsonl
into plain dict-based books keyed by integer tick counts (decimal arithmetic, no floats for keys),
and prints the expected values that are copied into the tests.

    python3 scripts/book_reference.py [path/to/coinbase_sample.jsonl]

The capture has no snapshot frames (tests/data/README.md), so each product's book starts empty
and is built from l2updates alone.
"""
import json
import sys
from decimal import Decimal as D
from pathlib import Path

TICKS = {"BTC-USD": D("0.01"), "ETH-USD": D("0.01"), "ADA-USD": D("0.00001")}
CHECKPOINTS = (1, 10, 20, 30, 41)  # 1-based ordinal among the capture's l2update messages


class Book:
    def __init__(self, tick):
        self.tick = D(tick)
        self.side = {"buy": {}, "sell": {}}
        self.rejected = 0
        self.absent = 0

    def ticks(self, price):
        q = D(price) / self.tick
        if q != q.to_integral_value():
            return None
        return int(q)

    def snapshot(self, bids, asks):
        self.side = {"buy": {}, "sell": {}}
        for name, levels in (("buy", bids), ("sell", asks)):
            for price, size in levels:
                t = self.ticks(price)
                if t is None:
                    self.rejected += 1
                elif D(size) > 0:
                    self.side[name][t] = D(size)

    def update(self, changes):
        for name, price, size in changes:
            t = self.ticks(price)
            if t is None:
                self.rejected += 1
            elif D(size) > 0:
                self.side[name][t] = D(size)
            elif self.side[name].pop(t, None) is None:
                self.absent += 1

    def bids(self):
        return sorted(self.side["buy"].items(), reverse=True)

    def asks(self):
        return sorted(self.side["sell"].items())

    def crossed(self):
        b, a = self.bids(), self.asks()
        return bool(b and a and b[0][0] >= a[0][0])

    def dump(self):
        fmt = lambda ls: "{" + ", ".join(f"{{{t}, {float(s)!r}}}" for t, s in ls) + "}"
        b, a = self.bids(), self.asks()
        top = f"top bid={fmt(b[:1])} ask={fmt(a[:1])}"
        return (f"{top}\n    bids={fmt(self.bids())}\n    asks={fmt(self.asks())}\n"
                f"    counts={len(self.side['buy'])}/{len(self.side['sell'])} "
                f"rejected={self.rejected} absent={self.absent} crossed={self.crossed()}")


def hand_built():
    snap = (
        [("100.00", "1"), ("99.50", "2"), ("99.00", "3")],
        [("100.50", "1"), ("101.00", "2"), ("101.50", "3")],
    )
    book = Book("0.01")
    steps = [
        ("snapshot", lambda: book.snapshot(*snap)),
        ("add top/mid/bottom of each side", lambda: book.update([
            ("buy", "100.20", "4"), ("buy", "99.75", "5"), ("buy", "98.00", "6"),
            ("sell", "100.30", "1.5"), ("sell", "100.75", "2.5"), ("sell", "102.00", "3.5")])),
        ("resize top/mid/bottom of each side", lambda: book.update([
            ("buy", "100.20", "7"), ("buy", "99.50", "8"), ("buy", "98.00", "9"),
            ("sell", "100.30", "1.25"), ("sell", "101.00", "2.25"), ("sell", "102.00", "3.25")])),
        ("remove top/mid/bottom of each side", lambda: book.update([
            ("buy", "100.20", "0"), ("buy", "99.75", "0"), ("buy", "98.00", "0"),
            ("sell", "100.30", "0"), ("sell", "100.75", "0"), ("sell", "102.00", "0")])),
        ("remove absent level (twice, one per side)", lambda: book.update([
            ("buy", "50.00", "0"), ("sell", "200.00", "0")])),
        ("duplicate prices in one update", lambda: book.update([
            ("buy", "99.00", "5"), ("buy", "99.00", "6"), ("sell", "101.50", "0"),
            ("sell", "101.50", "4"), ("buy", "98.50", "1"), ("buy", "98.50", "0")])),
        ("crossed: bid at 101.00 over ask 100.50", lambda: book.update([("buy", "101.00", "1")])),
        ("off-grid price 100.005 and 100.0051 rejected", lambda: book.update([
            ("buy", "100.005", "1"), ("sell", "100.0051", "1"), ("buy", "97.00", "1")])),
        ("new snapshot replaces everything", lambda: book.snapshot(
            [("50.00", "1")], [("60.00", "2"), ("60.00", "3")])),
        ("empty snapshot", lambda: book.snapshot([], [])),
    ]
    print("== hand-built (tick 0.01); format {price_ticks, size}")
    for name, step in steps:
        step()
        print(f"[{name}]\n    {book.dump()}")


def capture(path):
    books = {p: Book(t) for p, t in TICKS.items()}
    crossed_after = {p: 0 for p in TICKS}
    ordinal = 0
    print("\n== capture: BTC-USD/ETH-USD tick 0.01, ADA-USD tick 0.00001")
    for index, line in enumerate(Path(path).read_text().splitlines()):
        msg = json.loads(json.loads(line)["msg"])
        if msg["type"] == "snapshot":
            books[msg["product_id"]].snapshot(
                [(p, s) for p, s in msg["bids"]], [(p, s) for p, s in msg["asks"]])
        elif msg["type"] != "l2update":
            continue
        else:
            ordinal += 1
            product = msg["product_id"]
            books[product].update(msg["changes"])
            if books[product].crossed():
                crossed_after[product] += 1
            if ordinal in CHECKPOINTS:
                b = books[product]
                print(f"l2update #{ordinal} (file line index {index}) {product}\n    {b.dump()}")
    print("\nmessages after which the book was crossed:", crossed_after)
    for p, b in books.items():
        print(f"final {p}: counts={len(b.side['buy'])}/{len(b.side['sell'])} "
              f"rejected={b.rejected} absent={b.absent}")


if __name__ == "__main__":
    hand_built()
    capture(sys.argv[1] if len(sys.argv) > 1 else
            Path(__file__).resolve().parent.parent / "tests/data/coinbase_sample.jsonl")
