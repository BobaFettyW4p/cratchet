#!/usr/bin/env python3
"""Generate the seed corpus for fuzz/coinbase_parser_fuzz.

Splits tests/data/coinbase_sample.jsonl into one file per frame, plus a few small hand-written
edge cases, under build/fuzz/corpus/coinbase_parser (the "fuzz" preset's build directory). The
corpus is generated and not committed. Then run, from the repository root:

    cmake --preset fuzz && cmake --build --preset fuzz
    scripts/make_fuzz_corpus.py
    build/fuzz/fuzz/coinbase_parser_fuzz build/fuzz/corpus/coinbase_parser -max_total_time=60
"""

import argparse
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Small seeds for shapes the capture lacks: it has no snapshot (about 1 MB each, removed) and
# no error frame. Each is a minimal frame the parser should handle.
EDGE_CASES = {
    # snapshot with one bid and one ask: reaches the bids/asks span code.
    "edge_snapshot": b'{"type":"snapshot","product_id":"BTC-USD","bids":[["1.5","2"]],'
    b'"asks":[["1.6","3"]],"time":"2026-11-19T01:53:23.200696Z"}',
    # error frame with and without "reason".
    "edge_error": b'{"type":"error","message":"Failed to subscribe","reason":"bad product"}',
    "edge_error_no_reason": b'{"type":"error","message":"x"}',
    # l2update with a malformed level: bad_level path.
    "edge_bad_level": b'{"type":"l2update","product_id":"BTC-USD","time":"2026-11-19T01:53:23Z",'
    b'"changes":[["buy","1"]]}',
    # unknown type, and a non-object top level.
    "edge_unknown": b'{"type":"status","products":[]}',
    "edge_array": b"[1,2,3]",
    # empty input, and a lone string with escapes.
    "edge_empty": b"",
    "edge_escapes": b'{"type":"ticker","product_id":"A\\u00e9\\n\\"B","sequence":1,"time":"x"}',
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--input", type=Path, default=ROOT / "tests/data/coinbase_sample.jsonl")
    ap.add_argument("--out", type=Path, default=ROOT / "build/fuzz/corpus/coinbase_parser")
    args = ap.parse_args()

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    count = 0
    with args.input.open("rb") as f:
        for number, line in enumerate(f):
            frame = line.rstrip(b"\r\n")
            if frame:
                (args.out / f"frame_{number:05d}").write_bytes(frame)
                count += 1
    for name, data in EDGE_CASES.items():
        (args.out / name).write_bytes(data)

    print(f"wrote {count} frames and {len(EDGE_CASES)} edge cases to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
