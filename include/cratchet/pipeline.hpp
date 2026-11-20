#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "cratchet/capture.hpp"
#include "cratchet/coinbase/parser.hpp"
#include "cratchet/config.hpp"
#include "cratchet/normalizer.hpp"

namespace cratchet {

struct pipeline_stats {
  std::uint64_t frames = 0;
  std::uint64_t parse_errors = 0;
  std::uint64_t tickers = 0;
  std::uint64_t published = 0;
  std::uint64_t dropped = 0;
  std::uint64_t out_of_order = 0;
  std::uint64_t heartbeats = 0;
  std::uint64_t snapshots = 0;
  std::uint64_t l2updates = 0;
  std::uint64_t coinbase_errors = 0;  // "type":"error" messages from the feed
  std::uint64_t other = 0;            // subscriptions and unknown types
  std::map<std::string, std::uint64_t, std::less<>> parse_errors_by_code;
  std::map<std::string, std::uint64_t, std::less<>> drops_by_reason;
};

// What happens to each feed frame: optionally record it raw, parse it, and for tickers build,
// validate and serialize the Tick and hand it to the sink with its subject
// ("<subject_prefix>.<shard>"). Heartbeats, depth messages and errors are counted (errors are
// logged too) and not published, as in LedgerFlux. Single-threaded.
class pipeline {
 public:
  // Called for every tick to publish. `subject` and `tick.json` are valid only during the call.
  using sink_fn = std::function<void(std::string_view subject, const normalized_tick& tick)>;
  using clock_fn = std::function<std::int64_t()>;  // wall clock, ns since epoch

  // `recorder` may be null. `clock` defaults to std::chrono::system_clock.
  pipeline(const ingestor_config& config, sink_fn sink, capture_writer* recorder = nullptr,
           clock_fn clock = {});

  void on_frame(std::string_view frame, std::int64_t recv_ns);

  [[nodiscard]] const pipeline_stats& stats() const noexcept { return stats_; }

 private:
  void on_parse_error(const coinbase::parse_error& error, std::string_view frame);

  coinbase::parser parser_;
  normalizer normalizer_;
  std::vector<std::string> subjects_;  // by shard
  sink_fn sink_;
  capture_writer* recorder_;
  clock_fn clock_;
  pipeline_stats stats_;
};

// One-line summary of the counters, for periodic logging.
std::string format_stats(const pipeline_stats& stats);

}  // namespace cratchet
