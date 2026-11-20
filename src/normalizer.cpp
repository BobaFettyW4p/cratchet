#include "cratchet/normalizer.hpp"

#include <stdexcept>

#include "cratchet/shard.hpp"

namespace cratchet {

namespace {

std::optional<price_qty> pair(const std::optional<double>& px, const std::optional<double>& qty) {
  if (px && qty) {
    return price_qty{*px, *qty};
  }
  return std::nullopt;
}

}  // namespace

tick make_tick(const coinbase::ticker& msg, std::int64_t ts_ingest_ns) {
  return tick{
      .product = msg.product_id,
      .seq = msg.sequence,
      .ts_event = msg.time_ns,
      .ts_ingest = ts_ingest_ns,
      .last_trade = pair(msg.price, msg.last_size),
      .best_bid = pair(msg.best_bid, msg.best_bid_size),
      .best_ask = pair(msg.best_ask, msg.best_ask_size),
  };
}

normalizer::normalizer(std::uint32_t num_shards) : num_shards_(num_shards) {
  if (num_shards == 0) {
    throw std::invalid_argument("num_shards must be positive");
  }
}

normalizer::product_state& normalizer::state_for(std::string_view product) {
  if (auto it = products_.find(product); it != products_.end()) {
    return it->second;
  }
  return products_.emplace(std::string(product), product_state{shard_for(product, num_shards_), {}})
      .first->second;
}

result<normalized_tick, drop_reason> normalizer::normalize(const coinbase::ticker& msg,
                                                           std::int64_t ts_ingest_ns) {
  using normalize_result = result<normalized_tick, drop_reason>;

  const tick value = make_tick(msg, ts_ingest_ns);
  if (const auto reason = validate(value)) {
    return normalize_result::fail(*reason);
  }

  // As in the LedgerFlux normalizer, only valid ticks advance the last-seen sequence.
  product_state& state = state_for(value.product);
  const bool out_of_order = state.last_seq && value.seq < *state.last_seq;
  state.last_seq = value.seq;

  json_.clear();
  append_json(json_, value);
  return normalize_result::ok(normalized_tick{value, state.shard, json_, out_of_order});
}

}  // namespace cratchet
