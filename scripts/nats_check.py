#!/usr/bin/env python3
"""Read every tick Cratchet published to JetStream and check it the way LedgerFlux consumes it.

Fetches all messages on `<subject_prefix>.*` from the stream with an ephemeral pull consumer
(nats-py, as LedgerFlux's NATSStreamManager uses) and parses each with
`Tick.model_validate(json.loads(data))`, as the gateway and snapshotter do. Checks that each
message's subject matches `shard_product(product, num_shards)`.

If an expected file is given (the `<subject>\\t<json>` output of `cratchet normalize --publish`),
also requires the stream to hold exactly those messages, in order, byte for byte.

    uv run --project <ledgerflux> --frozen python scripts/nats_check.py \\
        <ledgerflux> <nats_url> <stream> <subject_prefix> [expected.tsv]
"""

import asyncio
import json
import sys
from pathlib import Path

NUM_SHARDS = 4


async def fetch_all(url: str, stream: str, prefix: str) -> list:
    import nats  # pylint: disable=import-outside-toplevel

    nc = await nats.connect(url)
    try:
        js = nc.jetstream()
        info = await js.stream_info(stream)
        total = info.state.messages
        sub = await js.pull_subscribe(f"{prefix}.*", stream=stream)
        messages = []
        while len(messages) < total:
            batch = await sub.fetch(min(500, total - len(messages)), timeout=5)
            for msg in batch:
                await msg.ack()
                messages.append((msg.subject, msg.data.decode("utf-8")))
        return messages
    finally:
        await nc.close()


def main() -> int:
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    ledgerflux, url, stream, prefix = Path(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
    expected_path = Path(sys.argv[5]) if len(sys.argv) == 6 else None
    sys.path.insert(0, str(ledgerflux))
    # pylint: disable=import-outside-toplevel
    from services.common import Tick, shard_product

    messages = asyncio.run(fetch_all(url, stream, prefix))
    failures = []
    for index, (subject, payload) in enumerate(messages):
        try:
            tick = Tick.model_validate(json.loads(payload))
        except Exception as exc:  # noqa: BLE001
            failures.append(f"message {index}: Tick.model_validate failed: {exc}")
            continue
        want = f"{prefix}.{shard_product(tick.product, NUM_SHARDS)}"
        if subject != want:
            failures.append(f"message {index}: subject {subject!r}, expected {want!r}")

    if expected_path is not None:
        expected = [
            tuple(line.split("\t", 1))
            for line in expected_path.read_text(encoding="utf-8").splitlines()
        ]
        if messages != expected:
            first = next(
                (i for i, (a, b) in enumerate(zip(messages, expected)) if a != b),
                min(len(messages), len(expected)),
            )
            failures.append(
                f"stream holds {len(messages)} messages, expected {len(expected)}; "
                f"first difference at {first}"
            )

    for failure in failures[:20]:
        print(failure, file=sys.stderr)
    if failures:
        print(f"FAIL: {len(failures)} problems in {len(messages)} messages", file=sys.stderr)
        return 1
    products = sorted({json.loads(p)["product"] for _, p in messages})
    print(f"OK: {len(messages)} messages in {stream} parse as LedgerFlux Ticks ({', '.join(products)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
