#include <benchmark/benchmark.h>

#include <string>

#include "cratchet/result.hpp"

// Placeholder benchmark so the bench/ target builds from day one.
// Real benchmarks arrive with Stage 2.
static void result_ok_roundtrip(benchmark::State& state) {
  for (auto _ : state) {
    auto r = cratchet::result<int, std::string>::ok(42);
    benchmark::DoNotOptimize(r.value());
  }
}
BENCHMARK(result_ok_roundtrip);
