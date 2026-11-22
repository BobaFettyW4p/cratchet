# Cratchet: notes for coding agents

C++20 market-data core (Coinbase feed handler, normalizer, NATS JetStream publisher) that
replaces LedgerFlux's Python ingestor. Read `README.md` for the layout, `PLAN.md` for the
roadmap and scope, and `docs/contract.md` for the LedgerFlux contract.

## Environment

The toolchain (Clang 18, GCC 13, CMake, Ninja, vcpkg) is already on the path when you run
in the sandbox image, so call the tools directly. `scripts/in_container.sh` wraps
`docker run` and does not work there: **do not use it**. (On a developer machine without
the toolchain, `scripts/in_container.sh <cmd>` runs the same commands in the `cratchet-dev`
image.)

## Build, test, format

```bash
cmake --preset dev -DCRATCHET_WERROR=ON     # CI configures with warnings as errors
cmake --build --preset dev
ctest --preset dev
```

Before you finish, run what CI runs:

```bash
cmake --preset asan -DCRATCHET_WERROR=ON && cmake --build --preset asan && ctest --preset asan
git ls-files '*.cpp' '*.hpp' | xargs clang-format-18 --dry-run --Werror      # format
find src apps tests bench -name '*.cpp' | xargs clang-tidy-18 -p build/dev --quiet   # lint (needs the dev configure)
```

Fix formatting with `clang-format-18 -i <files>`. Both `build/` and `.cache/` are gitignored.
Add or update GoogleTest tests in `tests/` for any behavior you change (register new files
in `tests/CMakeLists.txt`).

## What you cannot run here

- **NATS integration tests** (`nats_publisher*`) are skipped unless `CRATCHET_NATS_URL` is
  set; there is no JetStream server in the sandbox. A skip is expected, not a failure.
  If you change `nats_publisher.cpp`, say in your final message that those tests did not run.
- **The LedgerFlux contract check** (`scripts/contract_check.py`, `scripts/nats_check.py`)
  needs a LedgerFlux checkout and is CI-only.
- **Benchmarks** (`bench/`): do not run them or quote numbers. They are only meaningful on
  the documented x86_64 machine.
- **The live Coinbase feed** (`cratchet run` without `--dry-run`, `scripts/capture_coinbase.py`).
  Use `tests/data/` captures and `cratchet normalize <file>` instead.

## Conventions

- Namespace `cratchet`; `snake_case` for files and identifiers; private members end in `_`.
- Formatting is `.clang-format`, linting is `.clang-tidy`; both are enforced in CI.
- Keep the vcpkg baseline in `vcpkg.json` in sync with `docker/dev.Dockerfile`. Do not add
  dependencies without being asked.

## Scope

Follow `PLAN.md`. Non-goals until Stages 1 to 3 are solid: no matching engine, no FIX, no
multi-exchange support, no trading strategies. If a task seems to need one of these, stop
and report instead of building it.

## Working rules

- Work on your assigned branch; never touch `main`. You have no git credentials, so do not
  try to push. Commit your work and stop.
- Keep changes small and focused. Do not reformat files you are not otherwise changing.
- If tests fail for reasons unrelated to your change, report that rather than "fixing" them.
