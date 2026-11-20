#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cratchet/coinbase/messages.hpp"
#include "cratchet/result.hpp"
#include "cratchet/tick.hpp"

namespace cratchet {

// Builds a Tick from a Coinbase ticker as LedgerFlux's transform_coinbase_ticker() does:
// each of last_trade, best_bid and best_ask is set only if both of its source fields are present.
tick make_tick(const coinbase::ticker& msg, std::int64_t ts_ingest_ns);

struct normalized_tick {
  tick value;
  std::uint32_t shard{};
  std::string_view json;  // the serialized Tick; valid until the next normalize() call
  // seq is lower than the last valid tick's for this product. Logged and counted, not dropped.
  bool out_of_order = false;
};

// Does the work of LedgerFlux's ingestor transform plus its normalizer (docs/contract.md §1, §5):
// build the Tick, validate it, track per-product sequence order, pick the shard and serialize.
// Single-threaded; one instance per feed connection.
class normalizer {
 public:
  // Throws std::invalid_argument if num_shards is 0.
  explicit normalizer(std::uint32_t num_shards);

  // `ts_ingest_ns` is the wall-clock time after parsing, as LedgerFlux defines ts_ingest.
  result<normalized_tick, drop_reason> normalize(const coinbase::ticker& msg,
                                                 std::int64_t ts_ingest_ns);

  [[nodiscard]] std::uint32_t num_shards() const noexcept { return num_shards_; }

 private:
  struct product_state {
    std::uint32_t shard{};
    std::optional<std::int64_t> last_seq;
  };

  struct string_hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept {
      return std::hash<std::string_view>{}(text);
    }
  };

  product_state& state_for(std::string_view product);

  std::uint32_t num_shards_;
  std::unordered_map<std::string, product_state, string_hash, std::equal_to<>> products_;
  std::string json_;
};

}  // namespace cratchet
