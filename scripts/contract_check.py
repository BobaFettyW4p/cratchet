#!/usr/bin/env python3
"""Check Cratchet's published ticks against LedgerFlux's own Python code (docs/contract.md §8).

Inputs: a LedgerFlux checkout, a feed capture, and the output of `cratchet normalize` on that
capture (one `<subject>\\t<Tick JSON>` line per published tick). For every tick:

1. the payload passes `Tick.model_validate`, as LedgerFlux consumers parse it, and seq and the
   timestamps are JSON integers;
2. LedgerFlux's ingestor transform and normalizer validation, run on the same capture, publish
   the same ticks in the same order;
3. the payload is byte-identical to pydantic's serialization of the Python tick, after taking
   ts_ingest from Cratchet's tick (it is wall-clock time) and checking that ts_event differs by
   less than 1 us (Python derives it through a float);
4. the subject is `market.ticks.<shard_product(product, 4)>`.

Run with LedgerFlux's environment:

    uv run --project <ledgerflux> --frozen python scripts/contract_check.py \\
        <ledgerflux> <capture.jsonl> <cratchet-output.tsv>
"""

import json
import sys
from pathlib import Path
from types import SimpleNamespace

NUM_SHARDS = 4
SUBJECT_PREFIX = "market.ticks"
MAX_TS_EVENT_DELTA_NS = 1_000


def expected_ticks(capture: Path, transform, validate) -> list:
    """What LedgerFlux's ingestor + normalizer would publish for `capture`, in order."""
    normalizer = SimpleNamespace(last_sequences={})
    ticks = []
    with capture.open(encoding="utf-8") as src:
        for line in src:
            msg = json.loads(json.loads(line)["msg"])
            if msg.get("type") != "ticker":
                continue
            try:
                tick = transform(msg)
            except (KeyError, TypeError, ValueError):
                continue  # the ingestor logs and drops these
            if validate(normalizer, tick):
                ticks.append(tick)
    return ticks


def main() -> int:
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    ledgerflux, capture, cratchet_output = (Path(a) for a in sys.argv[1:])
    sys.path.insert(0, str(ledgerflux))
    # pylint: disable=import-outside-toplevel
    from services.common import Tick, shard_product
    from services.ingestor.app import transform_coinbase_ticker
    from services.normalizer.app import Normalizer

    expected = expected_ticks(capture, transform_coinbase_ticker, Normalizer._validate_tick)
    actual = cratchet_output.read_text(encoding="utf-8").splitlines()

    failures = []

    def fail(index: int, message: str) -> None:
        failures.append(f"tick {index}: {message}")

    if len(actual) != len(expected):
        failures.append(f"Cratchet published {len(actual)} ticks, LedgerFlux {len(expected)}")

    for index, (line, want) in enumerate(zip(actual, expected)):
        subject, _, payload = line.partition("\t")
        raw = json.loads(payload)
        try:
            got = Tick.model_validate(raw)
        except Exception as exc:  # noqa: BLE001 - report any validation failure
            fail(index, f"Tick.model_validate failed: {exc}")
            continue
        for key in ("seq", "ts_event", "ts_ingest"):
            if type(raw.get(key)) is not int:
                fail(index, f"{key} is {type(raw.get(key)).__name__}, not a JSON integer")

        delta = got.ts_event - want.ts_event
        if abs(delta) >= MAX_TS_EVENT_DELTA_NS:
            fail(index, f"ts_event differs by {delta} ns")
        reference = want.model_copy(update={"ts_event": got.ts_event, "ts_ingest": got.ts_ingest})
        if payload != reference.model_dump_json():
            fail(index, f"payload differs\n  cratchet:   {payload}\n  ledgerflux: {reference.model_dump_json()}")

        want_subject = f"{SUBJECT_PREFIX}.{shard_product(got.product, NUM_SHARDS)}"
        if subject != want_subject:
            fail(index, f"subject {subject!r}, expected {want_subject!r}")

    for failure in failures[:20]:
        print(failure, file=sys.stderr)
    if failures:
        print(f"FAIL: {len(failures)} problems in {len(actual)} ticks", file=sys.stderr)
        return 1
    print(f"OK: {len(actual)} ticks match LedgerFlux")
    return 0


if __name__ == "__main__":
    sys.exit(main())
