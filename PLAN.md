# Cratchet — Project Plan

> Bob Cratchit kept the ledger. Cratchet is C++ that only moves forward.

A C++20 market data core (feed handler + order book engine) that drops into the
[LedgerFlux](https://github.com/BobaFettyW4p/LedgerFlux) pipeline, plus an agent
layer that operates and evaluates it.

## Goals

1. **Learn and demonstrate modern C++** on a real, measurable system.
2. **Portfolio piece for SWE / platform roles**: benchmarks, tests, sanitizers,
   CI, containers, Kubernetes, observability.
3. **Agentic engineering**: an MCP server over the engine, an agent that uses it,
   and an eval harness that scores the agent.

## Non-goals (scope guard)

No matching engine, no FIX protocol, no multi-exchange support, no trading
strategies until Stages 1–3 are solid. Depth beats breadth.

## Integration contract with LedgerFlux

Cratchet must be a drop-in for the Python ingestor/normalizer, so the rest of
LedgerFlux (gateway, snapshotter, Postgres, Grafana) keeps working.

- **NATS stream:** `market_ticks`, subjects `market.ticks.<shard>`, `num_shards = 4`
- **Message:** JSON `Tick` (see `services/common/models.py` in LedgerFlux):

```json
{
  "v": 1,
  "type": "tick",
  "product": "BTC-USD",
  "seq": 12345,
  "ts_event": 1700000000000000000,
  "ts_ingest": 1700000000000000000,
  "fields": {
    "last_trade": {"px": 0.0, "qty": 0.0},
    "best_bid":   {"px": 0.0, "qty": 0.0},
    "best_ask":   {"px": 0.0, "qty": 0.0}
  }
}
```

- Timestamps are nanoseconds. Shard assignment must match the Python hashing
  of `product` (**TODO: read `services/ingestor/app.py` and replicate exactly**).
- **Gap to close:** the current ingestor subscribes to Coinbase `ticker` +
  `heartbeat` only (top of book). The order book needs depth data
  (`level2` / `level2_batch`). **Verify** which channels Coinbase Exchange
  currently allows unauthenticated before Stage 2.

## Infrastructure

| Need | Where |
|---|---|
| Development | Mac, inside a Dev Container (reproducible Clang + CMake + vcpkg) |
| Benchmarks | Linux desktop (bare metal, `perf`, pinned cores). Document hardware, kernel, and tuning in the README |
| CI | GitHub Actions: build, tests, sanitizers, lint. **Not** for absolute perf numbers (noisy runners) |
| Always-on demo (optional) | ~$5–10/mo VM running k3s, provisioned with Terraform, alongside LedgerFlux services |

## Tech choices (defaults, revisit if they hurt)

- C++20 (decided; small in-house `Result<T,E>` instead of `std::expected`), Clang, CMake, vcpkg
- Networking: Boost.Beast + OpenSSL; JSON: simdjson; NATS: `nats.c`
- Tests: GoogleTest; fuzzing: libFuzzer; benchmarks: Google Benchmark
- Metrics: `prometheus-cpp`
- Lint/format: clang-tidy, clang-format

## Stages

### Stage 0 — Scaffolding (a few days)

**Decisions (made before starting):**

- **Standard:** C++20, with a small `Result<T,E>` wrapped in one header.
- **Toolchain:** Ubuntu 24.04 base, Clang 18, GCC 13, vcpkg baseline pinned in
  `vcpkg.json`. (Benchmark server's installed toolchain is unknown; revisit at Stage 2.)
- **Environment:** one Dockerfile shared by the Dev Container and CI (and later the
  Stage 3 runtime image's builder stage).
- **Architectures:** dev on aarch64 (Apple Silicon), CI and benchmarks on x86_64.
  No `-march=native` in default builds; note this in the README. aarch64 is a
  useful extra check for Stage 2 memory-ordering code.
- **Layout:** `cratchet_core` static library; thin executables in `apps/`;
  `tests/` and `bench/` link against the library. CMake presets: `dev`, `asan`
  (ASan + UBSan), `tsan` (defined now, enabled in CI in Stage 2), `release`.
  Presets are the single build entry point for local and CI.
- **Dependencies in the manifest:** GoogleTest and Google Benchmark only. Boost,
  OpenSSL, simdjson, `nats.c` arrive with Stage 1. vcpkg binary cache in CI from day one.
- **Sanitizers in Stage 0 CI:** ASan + UBSan. TSan joins in Stage 2.
- **Lint/format:** real `.clang-tidy` (bugprone, performance, modernize,
  cppcoreguidelines subset) and `.clang-format` committed now; both are required CI checks.
- **Conventions:** namespace `cratchet`, `snake_case` for files and identifiers,
  MIT license.
- **Repo protection:** `main` is protected; the CI checks (build/test on Clang and
  GCC, ASan+UBSan, clang-tidy, format) are required to merge.
- **Sequencing:** LedgerFlux contract research (`docs/contract.md`, Coinbase channel
  check) starts after Stage 0, as the opening of Stage 1.

**Tasks:**
- [ ] Dockerfile + Dev Container (Clang 18, GCC 13, CMake, vcpkg)
- [ ] CMake skeleton, `CMakePresets.json`, vcpkg manifest (pinned baseline),
      `src/` + `apps/` + `tests/` + `bench/` layout, `Result<T,E>` header
- [ ] `.clang-format`, `.clang-tidy`, `LICENSE` (MIT)
- [ ] GitHub Actions: build + test on Linux (Clang + GCC), ASan+UBSan job,
      clang-tidy job, format check, vcpkg binary cache
- [ ] Branch protection on `main` with the above checks required
- [ ] README with the tagline, a one-paragraph architecture sketch, and the
      architecture/`-march` note

**Done when:** a "hello" test builds and passes in CI with sanitizers, and `main`
requires the CI checks to merge.

### Stage 1 — Feed handler + normalizer (~2–3 weeks)
- [ ] WebSocket client (Beast) with TLS, reconnect + backoff, heartbeat handling
- [ ] Parse Coinbase messages with simdjson into typed structs
      (`std::variant` for message types, `std::string_view` / `std::span` on the hot path)
- [ ] Normalize to the `Tick` schema; publish to NATS JetStream with the right sharding
- [ ] Config via JSON (match LedgerFlux `ingestor.json` shape)
- [ ] Prometheus metrics: messages/sec, parse latency, publish latency, reconnects
- [ ] Record raw feed to a file (needed for Stage 2 replay)
- [ ] Contract test: Python `Tick` model validates every message Cratchet emits

**Done when:** Cratchet replaces the Python ingestor in LedgerFlux and the
Grafana dashboards still work. First latency/throughput comparison captured.

### Stage 2 — Order book engine (~3–4 weeks)
- [ ] Limit order book from level2 snapshots + incremental updates
- [ ] Cache-friendly layout (flat sorted arrays / intrusive structures, not `std::map`)
- [ ] Arena / custom allocator for hot-path allocations
- [ ] SPSC lock-free ring buffer between network thread and book thread,
      with the memory-ordering rationale documented in comments
- [ ] **Replay tool**: deterministic playback of recorded feeds at configurable
      speed (1x → 100x+), so benchmarks and tests are reproducible
- [ ] Google Benchmark suite; `perf` flamegraphs; before/after write-up
- [ ] TSan + stress tests for all concurrent code
- [ ] Book invariants checked in tests (uncrossed, sorted, sizes non-negative)

**Done when:** the benchmark write-up is in `docs/` and explains what was
optimized, why, and what the numbers were on documented hardware.

### Stage 3 — Quality + platform (~1–2 weeks, overlaps Stages 1–2)
- [ ] libFuzzer target on the message parser
- [ ] clang-tidy clean in CI
- [ ] Multi-stage Dockerfile, small runtime image
- [ ] Kubernetes manifests (+ Skaffold) compatible with LedgerFlux's namespace
- [ ] Terraform for the optional always-on VM
- [ ] Benchmark regression tracking (relative, not absolute, in CI)

### Stage 4 — Agent layer (~2–3 weeks)
- [ ] MCP server exposing: `get_book(symbol, depth)`, `get_latency_stats`,
      `replay(from, to, speed)`
- [ ] Minimal agent loop on the raw Messages API (to learn the mechanics)
- [ ] Rebuild the surveillance/ops agent on the Claude Agent SDK; compare in a write-up
- [ ] Fault injection into replayed feeds: sequence gaps, crossed books,
      spread blowouts, volume spikes, stalled consumers
- [ ] **Eval harness**: scenarios with ground truth, scored for detection,
      false positives, and report quality

## Risks

- **Misleading benchmarks.** Coinbase's live rate is low, so Python may not be the
  bottleneck. Replay at high speed is what makes the comparison honest.
- **Lock-free bugs.** TSan, stress tests, and written rationale are part of the deliverable.
- **Scope creep.** See non-goals.
- **Coinbase API changes** (channel access, auth requirements). Verify early; the
  recorded-feed replay keeps development unblocked either way.

## First-week checklist

1. Stage 0 scaffolding and CI green, `main` protected.
2. Read LedgerFlux `services/ingestor/app.py` and `services/normalizer/app.py`;
   record exact shard hashing, field mapping, and sequence semantics in `docs/contract.md`.
3. Check which Coinbase channels are available unauthenticated; capture a
   few minutes of raw `ticker` and `level2` traffic to `data/` (gitignored).
4. Start Stage 1: WebSocket connect → print parsed messages.

## Open questions

- Always-on public deployment, or reproducible `make up` only?
- Is the Linux benchmark machine definitely available, and what toolchain does it have?

## Resolved

- Dev Container vs. plain Docker: one shared Dockerfile used by both.
- C++23 vs. C++20: C++20 with a small `Result<T,E>`.
