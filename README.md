# Cratchet

> Bob Cratchit kept the ledger. Cratchet is C++ that only moves forward.

A C++20 market data core — feed handler plus order book engine — that drops into
the [LedgerFlux](https://github.com/BobaFettyW4p/LedgerFlux) pipeline in place of its
Python ingestor/normalizer, with an agent layer that operates and evaluates it.

## Architecture (sketch)

A WebSocket feed handler ingests exchange messages, parses them into typed structs,
and normalizes them to LedgerFlux's `Tick` schema, publishing to NATS JetStream
with matching shard assignment. A separate order book engine consumes the same
stream over a lock-free SPSC ring buffer and maintains a cache-friendly limit
order book. Recorded feeds can be replayed deterministically for tests and
benchmarks. An MCP server exposes the engine to an agent, and an eval harness
scores that agent against fault-injected replays. See [PLAN.md](PLAN.md) for the
staged roadmap; the project is in Stage 1 (feed handler and normalizer).

## Layout

| Path | Purpose |
|---|---|
| `include/cratchet/` | Public headers |
| `src/` | `cratchet_core` static library |
| `apps/` | Thin executables linking `cratchet_core` |
| `tests/` | GoogleTest unit tests |
| `bench/` | Google Benchmark suites |
| `docker/dev.Dockerfile` | Toolchain image shared by the Dev Container and CI |

## Building

The toolchain (Clang 18, GCC 13, CMake, Ninja, vcpkg pinned to the baseline in
`vcpkg.json`) lives in a Docker image. Open the repo in the Dev Container, or
use the same image from a plain shell:

```bash
docker build -f docker/dev.Dockerfile -t cratchet-dev .
scripts/in_container.sh cmake --preset dev
scripts/in_container.sh cmake --build --preset dev
scripts/in_container.sh ctest --preset dev
```

Inside the Dev Container, drop the `scripts/in_container.sh` prefix.

| Preset | What it does |
|---|---|
| `dev` | Debug build |
| `asan` | Debug + AddressSanitizer + UBSan |
| `tsan` | Debug + ThreadSanitizer (defined now; enabled in CI from Stage 2) |
| `release` | Optimized build |

Pass `-DCRATCHET_WERROR=ON` at configure time to make warnings errors (CI does).

### Architectures

Day-to-day development happens on aarch64 (Apple Silicon, via the Dev Container);
CI and benchmarks run on x86_64. Default builds avoid `-march=native` so the same
configuration works on both. Benchmark numbers are only meaningful from the
documented x86_64 machine, never from CI runners or a laptop.

## Running

```bash
# Live feed to NATS JetStream, as the LedgerFlux ingestor does:
build/dev/apps/cratchet run --config path/to/ingestor.json --nats-config path/to/nats.config.json

# No NATS: print ticks instead, and record raw frames for replay:
build/dev/apps/cratchet run --config path/to/ingestor.json --dry-run --record feed.jsonl

# Replay a recording through the parser and normalizer (add --publish to send it to NATS):
build/dev/apps/cratchet normalize feed.jsonl
```

`cratchet run` reads the same `config.json` and `nats.config.json` as the LedgerFlux
ingestor (by default at the paths its Deployment mounts), with the same environment
overrides (`NATS_URLS`, ...). It reconnects to the feed with exponential backoff and
treats 5 s without any frame as a stale feed (heartbeats arrive every second per
product); the NATS connection reconnects on its own. Run `cratchet` with no arguments
for all options.

The NATS publisher tests need a JetStream server and are skipped otherwise:

```bash
docker network create cratchet-test
docker run -d --rm --name cratchet-nats --network cratchet-test nats:2.10.22 -js
CRATCHET_DOCKER_NETWORK=cratchet-test CRATCHET_NATS_URL=nats://cratchet-nats:4222 \
  scripts/in_container.sh ctest --preset dev
```

## Conventions

Namespace `cratchet`, `snake_case` for files and identifiers (private members get
a trailing `_`), formatting by `.clang-format`, linting by `.clang-tidy`. Both are
enforced in CI.

## License

MIT, see [LICENSE](LICENSE).
