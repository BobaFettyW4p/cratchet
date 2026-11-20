# LedgerFlux integration contract

What Cratchet must do to replace LedgerFlux's Python ingestor without
breaking the gateway, the snapshotter, Postgres or Grafana.

Source: [LedgerFlux](https://github.com/BobaFettyW4p/LedgerFlux) at commit
`004a8c78165e2281076a226d6fbca26aa7ea223d`. File references below are relative
to that repo. Re-check this document if LedgerFlux changes `services/common/`.

## 1. Data flow today

```
Coinbase ws-feed ──► ingestor ──► NATS JetStream stream `market_ticks`
  (ticker, heartbeat)              subjects `market.ticks.<shard>` (0..3)
                                        │
          ┌─────────────────────────────┼─────────────────────────┐
          ▼                             ▼                         ▼
   normalizer-N (×4)             snapshotter-N (×4)          gateway (all shards)
   reads  market.ticks.N         reads market.ticks.N        reads market.ticks.0..3
   writes market.ticks.N  ⚠      writes Postgres +           fans out to WebSocket
                                 market.ticks.snapshot       clients
```

- Stream `market_ticks` is created by whichever service connects first, with
  subjects `market.ticks.*` and default retention
  (`services/common/stream.py:86`). `max_age` is read from config but **not
  applied** (comment at `stream.py:84`).
- The normalizer, snapshotter and gateway use durable pull consumers named
  `normalizer-<shard>`, `snapshotter-<host>-<shard>` and `gateway-<host>-<shard>`.
- Normalizer and snapshotter are StatefulSets with 4 replicas. The pod ordinal
  (the `-N` suffix of `HOSTNAME`) is the shard id (`normalizer/app.py:22`).

### ⚠ The normalizer republishes into its own input

`normalizer.json` sets `input_stream` and `output_stream` to the same value,
`market.ticks`. The output shard is the same hash as the input shard, so every
tick that passes validation is published back to the subject it was read from
(`normalizer/app.py:125-129`). Its durable consumer then reads that copy, and
so on. As written, each tick loops forever, and the gateway and snapshotter
see each tick many times.

**Fix (LedgerFlux branch `fix-normalizer-loop`, pending merge):** the ingestor
publishes to stream `market_raw` (`market.raw.<shard>`). The normalizer reads
that stream and writes `market_ticks` (`market.ticks.<shard>`), which the
gateway and snapshotter already consume. The normalizer now refuses to start
if its input and output are the same.

**Consequence for Cratchet:** Cratchet takes over the work of both the
ingestor and the normalizer (validation moves into Cratchet; see §5), so it
publishes validated ticks **directly to `market.ticks.<shard>`**. In the
Cratchet deployment, the Python ingestor and normalizer are not run.
The Python baseline for comparison is ingestor → `market.raw` → normalizer →
`market.ticks`.

## 2. Wire message: `Tick`

Defined in `services/common/models.py:8-26`. Consumers parse with
`Tick.model_validate(json.loads(...))`.

```json
{"v":1,"type":"tick","product":"BTC-USD","seq":12345,
 "ts_event":1700000000123456000,"ts_ingest":1700000000124000000,
 "fields":{"last_trade":{"px":43000.01,"qty":0.0125},
           "best_bid":{"px":43000.0,"qty":1.2},
           "best_ask":{"px":43000.02,"qty":0.5}}}
```

| Field | Type | Rule |
|---|---|---|
| `v` | int | always `1` |
| `type` | string | always `"tick"` |
| `product` | string | Coinbase `product_id`, upper-case (e.g. `BTC-USD`) |
| `seq` | int | Coinbase `sequence` (see §4) |
| `ts_event` | int, ns since epoch | Coinbase `time` (see §4) |
| `ts_ingest` | int, ns since epoch | wall clock when the tick is built |
| `fields.last_trade` | `{px, qty}` or `null` | from `price`, `last_size` |
| `fields.best_bid` | `{px, qty}` or `null` | from `best_bid`, `best_bid_size` |
| `fields.best_ask` | `{px, qty}` or `null` | from `best_ask`, `best_ask_size` |

Encoding rules:

- Python emits compact JSON (`model_dump_json`) in the field order above, with
  absent sub-objects as `null`. Consumers don't depend on key order or
  whitespace, but **Cratchet should match the order and compactness anyway**, so
  that byte-level diffs against the Python output stay readable.
- `px` and `qty` are JSON numbers (float64). Coinbase sends them as decimal
  strings; Python converts them with `float()`. Cratchet does the same and
  emits the shortest round-trip digits. The layout is pydantic's, not Python's
  `repr`: pydantic-core formats floats as Rust's ryu does, so `1e-05` is
  `0.00001`, `1e-07` is `1e-7` and `1.5e+16` is `1.5e16`. Integral values keep
  a `.0` (`43000.0`). `cratchet::json::append_double` reproduces this; its
  tests pin vectors taken from pydantic.
- `seq`, `ts_event` and `ts_ingest` must be JSON integers. A float would be
  accepted by pydantic in lax mode, but Postgres stores them as integers.
- A trade/bid/ask object is emitted only if **both** of its source fields are
  present in the Coinbase message (`ingestor/app.py:39-54`).

Other messages on the stream: the snapshotter publishes `Snapshot` objects to
`market.ticks.snapshot`. Cratchet never publishes there and must not consume it.

## 3. Sharding

`services/common/util.py:6-9`:

```python
int(hashlib.sha256(product.encode("utf-8")).hexdigest(), 16) % num_shards
```

That is, read the 32-byte SHA-256 digest of the UTF-8 product name as one
**big-endian 256-bit unsigned integer**, then take it modulo `num_shards`.

C++ implementation that is exact for any `num_shards` (no 256-bit type needed):

```cpp
uint64_t r = 0;
for (uint8_t b : digest) r = (r * 256 + b) % num_shards;
```

When `num_shards` is a power of two (the default, 4), this reduces to
`digest[31] % num_shards`. Use the general loop anyway, because
`num_shards` is configurable.

Subject: `<subject_prefix>.<shard>`, which is `market.ticks.<shard>` by default.

Reference vectors (a unit test must pin these):

| product | SHA-256 (hex) | shard (n=4) |
|---|---|---|
| `BTC-USD` | `2fd1a09ed73e366561d195e4ae8fa215eca2085e508ecca8411af1c61dccc815` | 1 |
| `ETH-USD` | `d50cffa23eb10f176af218c4989dc38cab359bc35cd83d2adcd80fa93faa515c` | 0 |
| `ADA-USD` | `e4dcec648c05204e7c3e15a58a7eac656a3a366e745b6888005914f421d2bd30` | 0 |
| `SOL-USD` | `1655865dd49646fc4c899c4bd946a5baa4946df7ebd0dab64bcc3554cccd16fb` | 3 |

Regenerate with `python3 -c "import hashlib;print(hashlib.sha256(b'BTC-USD').hexdigest())"`.

The shard depends only on `product` and `num_shards`. All ticks for a product go
to one subject, which is what lets each per-shard consumer see one product's
ticks in order.

## 4. Field semantics

### `ts_event`

Python: `datetime.fromisoformat(time).timestamp() * 1_000_000_000`, truncated
to an int (`ingestor/app.py:34-35`). Coinbase sends RFC 3339 UTC with
microsecond precision (`2026-11-18T01:02:03.123456Z`).

Going through a float64 loses precision: at ~1.7e18 ns, adjacent float64 values
are 256 ns apart. **Cratchet parses the timestamp straight to integer
nanoseconds** (exact: microseconds × 1000). So Python and Cratchet can disagree
on `ts_event` by up to a few hundred ns. The contract test must allow
`|Δ| < 1 µs` instead of requiring equality.

### `ts_ingest`

Python: `datetime.now().timestamp()` when the tick is built, i.e. after the
JSON parse, not on socket receive (`models.py:135`). Cratchet should use
`std::chrono::system_clock` at the same point: after parsing, before
publishing. If Cratchet wants a receive timestamp for latency metrics, keep it
internal; don't change the meaning of `ts_ingest`.

### `seq`

The Coinbase `sequence` field, passed through unchanged. On the Exchange
ws-feed, `sequence` is **per product** and shared across all channels for
that product, so the `ticker` channel alone **skips numbers by design**. Gaps
in `seq` on the tick stream are not data loss. Order is what matters: the
normalizer logs (but does not reject) `seq` lower than the last seen for that
product (`normalizer/app.py:182-189`).

Observed 2026-11-19: 713 ticker messages over 20 s for three products had 710
forward jumps and 0 backward steps. Gaps are the norm.

Stage 2 note: `level2_batch` messages carry **no** sequence number (see §9),
so book gap detection can't come from the depth channel or from `Tick.seq`.

### Heartbeats

The ingestor subscribes to `heartbeat` but ignores it (`ingestor/app.py:179`).
Cratchet should use heartbeats for liveness (stale-feed detection and
reconnect) and must not publish them as ticks.

### Other message types

`subscriptions` is logged. Anything else is logged as unknown and dropped.
Coinbase `error` messages are currently unhandled. Cratchet should log them
and count them as a metric.

## 5. Validation (moved from the normalizer)

Because the normalizer won't run (§1), Cratchet applies its rules before
publishing (`normalizer/app.py:154-195`). A tick is **dropped** if:

1. `product` is empty;
2. any present `px` is `<= 0` (last_trade, best_bid, best_ask);
3. both `best_bid` and `best_ask` are present and `ask.px <= bid.px`
   (crossed or locked book).

An out-of-order `seq` (lower than the last seen for the product) is **logged and
counted, not dropped**. Dropped ticks are counted by reason.

## 6. Configuration

Service config (`k8s/services/configs/ingestor.json`, mounted at
`/app/services/ingestor/config.json`):

```json
{
  "products": ["BTC-USD", "ETH-USD", "ADA-USD"],
  "channels": ["ticker", "heartbeat"],
  "ws_uri": "wss://ws-feed.exchange.coinbase.com",
  "num_shards": 4,
  "stream_name": "market_ticks",
  "subject_prefix": "market.ticks"
}
```

Defaults when a key is missing: `channels` = `["ticker","heartbeat"]`,
`num_shards` = 4, `stream_name` = `market_ticks`, `subject_prefix` =
`market.ticks`, `ws_uri` as above, `health_port` = 8080. Products are trimmed
and upper-cased.

NATS config (`nats.config.json`, mounted at `/app/services/common/`). The
precedence is: explicit argument > env var > file > default
(`services/common/config.py:51-124`):

| Key | Env override | Default |
|---|---|---|
| `urls` | `NATS_URLS` | `nats://localhost:4222` (k8s: `nats://nats:4222`) |
| `stream_name` | `JS_STREAM_NAME` | `market_ticks`; must not contain `.` |
| `subject_prefix` | `JS_SUBJECT_PREFIX` | `market.ticks` |
| `retention_minutes` | `JS_RETENTION_MINUTES` | 30; must be > 0 |
| `max_age_seconds` | `JS_MAX_AGE_SECONDS` | `retention_minutes × 60`; must be > 0 |
| `delete_existing` | `NATS_DELETE_EXISTING` | false |

Stream bootstrap must match Python: if `stream_info` succeeds, use the
existing stream. Otherwise, optionally delete it, then create it with
`subjects = ["<prefix>.*"]`. NATS connect retries with exponential backoff
(1, 2, 4, 8 s…) for up to 60 s.

Cratchet reads the **same two files at the same paths**, so the existing
ConfigMaps work unchanged.

## 7. Operational surface

The k8s Service is `ingestor:8080`. Prometheus scrapes the static target
`ingestor:8080/metrics` under `job="ingestor"`. Cratchet's Service and port stay
the same.

| Endpoint | Response | Used by |
|---|---|---|
| `GET /health` | `{"status":"ok","messages_received":N,"messages_published":N,"errors":N}` | liveness probe |
| `GET /ready` | `{"status":"ready"\|"not_ready","broker":"connected"\|"disconnected","websocket":"connected"\|"disconnected"}` | readiness probe |
| `GET /metrics` | Prometheus text format | Prometheus |

Metrics Cratchet **must** export with these names and labels. Grafana uses
the first three (`market-data.json`, `system-health.json`):

| Metric | Type | Labels |
|---|---|---|
| `ledgerflux_ingestor_ticks_received_total` | counter | `product` |
| `ledgerflux_ingestor_websocket_connected` | gauge (0/1) | — |
| `ledgerflux_ingestor_ticks_published_total` | counter | `product`, `shard` |
| `ledgerflux_ingestor_websocket_reconnects_total` | counter | — |
| `ledgerflux_ingestor_processing_latency_seconds` | histogram, buckets `.001 .005 .01 .025 .05 .1 .25 .5 1` | `product` |
| `ledgerflux_ingestor_gaps_detected_total` | counter | `product` |
| `ledgerflux_build_info` | info (gauge `=1`) | `service="ingestor"`, `version`, `commit` |

Python defines the last four but never updates them. Cratchet should populate
them. Cratchet-specific metrics (parse latency, drop reasons, …) use a
`cratchet_` prefix so they don't collide with LedgerFlux names.

The latency histogram's smallest bucket is 1 ms, which is too coarse to show
Cratchet's numbers. Keep the LedgerFlux histogram for dashboard compatibility,
and add `cratchet_*` histograms with µs-scale buckets for the benchmark story.

## 8. Contract test (Stage 1 deliverable)

1. Feed the recorded Coinbase `ticker` messages (`data/`) through Cratchet and
   capture the emitted JSON.
2. Validate every message with LedgerFlux's `Tick.model_validate` (pinned
   LedgerFlux commit, run via `uv` in CI).
3. For the same inputs, run Python's `transform_coinbase_ticker` and compare
   field by field: exact for `product`, `seq`, `px` and `qty`; `|Δ| < 1 µs` for
   `ts_event`; `ts_ingest` is not compared.
4. Assert the shard for every product matches `shard_product`.

Status: all four steps run in the CI `contract` job. `cratchet normalize`
replays the committed sample (`tests/data/`) and prints each tick as it would be
published (`<subject>\t<json>`). `scripts/contract_check.py` then runs
LedgerFlux's own transform, normalizer validation, `Tick` model and
`shard_product` at the pinned commit, and requires every payload to match
pydantic's output byte for byte apart from `ts_event` and `ts_ingest`.
`tests/replay_test.cpp` makes the same comparison without Python, against a
golden file from `scripts/make_golden_ticks.py`.

## 9. Coinbase channel access (checked 2026-11-19)

Probed `wss://ws-feed.exchange.coinbase.com` without credentials:

| Channel | Unauthenticated? | Notes |
|---|---|---|
| `ticker` | yes | what LedgerFlux uses today |
| `heartbeat` | yes | 1/s per product; carries `sequence` and `last_trade_id` |
| `level2_batch` | yes | server reports it as `level2_50` (50 ms batches) |
| `level2`, `level3`, `full` | **no** | `"level2, level3, and full channels now require authentication"` |

Message shapes seen:

- `ticker`: `sequence`, `product_id`, `price`, `last_size`, `best_bid`,
  `best_bid_size`, `best_ask`, `best_ask_size`, `side`, `time`, `trade_id`,
  plus 24 h/30 d stats. Prices and sizes are decimal strings.
- `snapshot` (once per product, on subscribe): `product_id`, `time`, full-depth
  `bids`/`asks` as `[price, size]` string pairs. ADA-USD had 668 bid and
  7,128 ask levels. **No sequence number.**
- `l2update`: `product_id`, `time`, `changes: [[side, price, new_size], …]`,
  where `side` is `buy`/`sell` and size `"0"` removes the level. On average 25
  changes per message, up to 237. **No sequence number.**

Rates (three products, 20 s sample): ~36 ticker/s, ~41 l2update/s
(~1,000 level changes/s), 3 heartbeat/s. That's low, which confirms that the
benchmark story has to rely on high-speed replay.

**Consequences for Stage 2 (book integrity without sequence numbers):**

- A dropped `l2update` can't be detected from the depth channel itself.
- Usable consistency checks:
  1. **Ticker cross-check:** `ticker` carries a sequenced `best_bid`/`best_ask`.
     The book's top of book should match at roughly the same time.
  2. **Periodic REST snapshot:** compare the book against
     `GET /products/<id>/book?level=2`.
  3. **Resubscribe on disconnect:** a fresh `snapshot` resets the book.
- Alternative: use the authenticated `level2` channel or the FIX market data
  gateway, which needs a free API key. Decide at the start of Stage 2.

Recording: `scripts/capture_coinbase.py` writes JSON Lines of
`{"recv_ns", "msg"}` with the raw frame text, into `data/` (gitignored).

## Open items

- **Stage 2 depth source:** unauthenticated `level2_batch` plus consistency
  checks (§9), or authenticated `level2` / FIX for real incremental updates.
- **Python baseline for benchmarks**: measure only after the LedgerFlux loop fix
  (§1) is merged and deployed. Numbers from before the fix are not meaningful.
