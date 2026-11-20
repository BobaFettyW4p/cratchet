#include "cratchet/pipeline.hpp"

#include <chrono>
#include <format>
#include <type_traits>
#include <utility>
#include <variant>

#include "cratchet/log.hpp"

namespace cratchet {

namespace {

std::int64_t system_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string_view preview(std::string_view frame) {
  constexpr std::size_t max = 200;
  return frame.substr(0, max);
}

// Log the first few of a recurring problem, then one in every 1000, so a feed change that
// breaks every frame cannot flood the log.
bool should_log(std::uint64_t count) { return count <= 10 || count % 1000 == 0; }

}  // namespace

pipeline::pipeline(const ingestor_config& config, sink_fn sink, capture_writer* recorder,
                   clock_fn clock)
    : normalizer_(config.num_shards),
      sink_(std::move(sink)),
      recorder_(recorder),
      clock_(clock ? std::move(clock) : clock_fn(system_now_ns)) {
  subjects_.reserve(config.num_shards);
  for (std::uint32_t shard = 0; shard < config.num_shards; ++shard) {
    subjects_.push_back(config.subject_prefix + "." + std::to_string(shard));
  }
}

void pipeline::on_frame(std::string_view frame, std::int64_t recv_ns) {
  ++stats_.frames;
  if (recorder_ != nullptr) {
    recorder_->write(recv_ns, frame);
  }

  auto parsed = parser_.parse(frame);
  if (!parsed) {
    on_parse_error(parsed.error(), frame);
    return;
  }

  std::visit(
      [&](const auto& msg) {
        using message_type = std::decay_t<decltype(msg)>;
        if constexpr (std::is_same_v<message_type, coinbase::ticker>) {
          ++stats_.tickers;
          // ts_ingest: after parsing, before publishing (docs/contract.md §4).
          auto out = normalizer_.normalize(msg, clock_());
          if (!out) {
            ++stats_.dropped;
            const auto reason = to_string(out.error());
            const auto count = ++stats_.drops_by_reason[std::string(reason)];
            if (should_log(count)) {
              log::warn("dropped {} tick seq={}: {} (#{})", msg.product_id, msg.sequence, reason,
                        count);
            }
            return;
          }
          if (out.value().out_of_order) {
            ++stats_.out_of_order;
            log::warn("out-of-order seq {} for {}", msg.sequence, msg.product_id);
          }
          ++stats_.published;
          if (sink_) {
            sink_(subjects_[out.value().shard], out.value());
          }
        } else if constexpr (std::is_same_v<message_type, coinbase::heartbeat>) {
          ++stats_.heartbeats;
        } else if constexpr (std::is_same_v<message_type, coinbase::snapshot>) {
          ++stats_.snapshots;
        } else if constexpr (std::is_same_v<message_type, coinbase::l2update>) {
          ++stats_.l2updates;
        } else if constexpr (std::is_same_v<message_type, coinbase::error_message>) {
          ++stats_.coinbase_errors;
          log::error("feed error: {} ({})", msg.message, msg.reason);
        } else if constexpr (std::is_same_v<message_type, coinbase::subscriptions>) {
          ++stats_.other;
          log::info("subscribed: {}", preview(frame));
        } else {
          ++stats_.other;
          log::debug("ignoring message type {}", msg.type);
        }
      },
      parsed.value());
}

void pipeline::on_parse_error(const coinbase::parse_error& error, std::string_view frame) {
  ++stats_.parse_errors;
  const auto code = to_string(error.code);
  const auto count = ++stats_.parse_errors_by_code[std::string(code)];
  if (should_log(count)) {
    log::warn("parse error {} field '{}' (#{}): {}", code, error.field, count, preview(frame));
  }
}

std::string format_stats(const pipeline_stats& stats) {
  std::string out = std::format(
      "frames={} tickers={} published={} dropped={} out_of_order={} heartbeats={} "
      "l2updates={} snapshots={} feed_errors={} parse_errors={}",
      stats.frames, stats.tickers, stats.published, stats.dropped, stats.out_of_order,
      stats.heartbeats, stats.l2updates, stats.snapshots, stats.coinbase_errors,
      stats.parse_errors);
  for (const auto& [reason, count] : stats.drops_by_reason) {
    out += std::format(" dropped.{}={}", reason, count);
  }
  for (const auto& [code, count] : stats.parse_errors_by_code) {
    out += std::format(" parse_error.{}={}", code, count);
  }
  return out;
}

}  // namespace cratchet
