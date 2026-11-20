// Parser and normalizer benchmarks over frames from the committed Coinbase capture
// (tests/data/coinbase_sample.jsonl). Numbers are only meaningful on the documented x86_64
// benchmark machine (README.md); CI and laptop runs are for spotting large regressions.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cratchet/capture.hpp"
#include "cratchet/coinbase/parser.hpp"
#include "cratchet/json.hpp"
#include "cratchet/normalizer.hpp"
#include "cratchet/timestamp.hpp"

namespace {

using cratchet::coinbase::parser;
using cratchet::coinbase::ticker;

struct sample {
  std::vector<std::string> frames;  // every frame, in capture order
  std::string ticker;               // first ticker frame
  std::string l2update;             // largest l2update frame
};

// Loads the capture once. Throws if it is missing, since every benchmark needs it.
const sample& load_sample() {
  static const sample loaded = [] {
    sample s;
    cratchet::capture_reader capture(std::string(CRATCHET_BENCH_DATA_DIR) +
                                     "/coinbase_sample.jsonl");
    while (const auto frame = capture.next()) {
      s.frames.emplace_back(frame->msg);
    }
    parser p;
    for (const auto& frame : s.frames) {
      auto parsed = p.parse(frame);
      if (!parsed) {
        continue;
      }
      if (s.ticker.empty() && std::holds_alternative<ticker>(parsed.value())) {
        s.ticker = frame;
      }
      if (std::holds_alternative<cratchet::coinbase::l2update>(parsed.value()) &&
          frame.size() > s.l2update.size()) {
        s.l2update = frame;
      }
    }
    return s;
  }();
  return loaded;
}

std::int64_t total_bytes(const std::vector<std::string>& frames) {
  std::int64_t bytes = 0;
  for (const auto& frame : frames) {
    bytes += static_cast<std::int64_t>(frame.size());
  }
  return bytes;
}

void parse_frame(benchmark::State& state, const std::string& frame) {
  parser p;
  for (auto _ : state) {
    auto parsed = p.parse(frame);
    benchmark::DoNotOptimize(parsed);
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(frame.size()));
}

void parse_ticker(benchmark::State& state) { parse_frame(state, load_sample().ticker); }
BENCHMARK(parse_ticker);

void parse_l2update_largest(benchmark::State& state) { parse_frame(state, load_sample().l2update); }
BENCHMARK(parse_l2update_largest);

// Ticker already parsed: Tick construction, validation, shard lookup and serialization.
void normalize_ticker(benchmark::State& state) {
  parser p;
  const auto parsed = p.parse(load_sample().ticker);
  const auto msg = std::get<ticker>(parsed.value());
  cratchet::normalizer n(4);
  std::int64_t seq = msg.sequence;
  for (auto _ : state) {
    ticker next = msg;
    next.sequence = ++seq;  // keep the in-order path
    auto out = n.normalize(next, 0);
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(normalize_ticker);

// The whole sample, in capture order: parse every frame and normalize every ticker.
// Mix: 604 ticker, 60 heartbeat, 41 l2update, 1 subscriptions.
void parse_and_normalize_sample(benchmark::State& state) {
  const auto& frames = load_sample().frames;
  parser p;
  cratchet::normalizer n(4);
  for (auto _ : state) {
    for (const auto& frame : frames) {
      auto parsed = p.parse(frame);
      if (parsed) {
        if (const auto* msg = std::get_if<ticker>(&parsed.value())) {
          auto out = n.normalize(*msg, 0);
          benchmark::DoNotOptimize(out);
        }
      }
      benchmark::DoNotOptimize(parsed);
    }
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(frames.size()));
  state.SetBytesProcessed(state.iterations() * total_bytes(frames));
}
BENCHMARK(parse_and_normalize_sample);

void parse_timestamp(benchmark::State& state) {
  constexpr std::string_view text = "2026-11-19T01:53:23.200696Z";
  for (auto _ : state) {
    auto ns = cratchet::parse_rfc3339_ns(text);
    benchmark::DoNotOptimize(ns);
  }
}
BENCHMARK(parse_timestamp);

void format_double(benchmark::State& state) {
  std::string out;
  for (auto _ : state) {
    out.clear();
    cratchet::json::append_double(out, 358.58911845);
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(format_double);

}  // namespace
