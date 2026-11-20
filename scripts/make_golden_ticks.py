#!/usr/bin/env python3
"""Run recorded Coinbase ticker frames through LedgerFlux's own transform and save the result.

For each "ticker" frame in a capture written by capture_coinbase.py, writes one line holding
the JSON that LedgerFlux's transform_coinbase_ticker() + Tick.model_dump_json() produce, with
ts_ingest set to 0 (it is wall-clock time). Cratchet's tests compare against this file
(docs/contract.md §8).

Run it with LedgerFlux's environment:

    uv run --project ~/repos/LedgerFlux python scripts/make_golden_ticks.py \\
        ~/repos/LedgerFlux tests/data/coinbase_sample.jsonl tests/data/coinbase_sample.ticks.jsonl
"""

import json
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    ledgerflux, capture, output = (Path(a) for a in sys.argv[1:])
    sys.path.insert(0, str(ledgerflux))
    from services.ingestor.app import transform_coinbase_ticker  # noqa: PLC0415

    count = 0
    with capture.open(encoding="utf-8") as src, output.open("w", encoding="utf-8") as dst:
        for line in src:
            msg = json.loads(json.loads(line)["msg"])
            if msg.get("type") != "ticker":
                continue
            tick = transform_coinbase_ticker(msg)
            tick.ts_ingest = 0
            dst.write(tick.model_dump_json() + "\n")
            count += 1
    print(f"wrote {count} ticks to {output}")


if __name__ == "__main__":
    main()
