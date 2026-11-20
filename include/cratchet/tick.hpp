#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cratchet {

struct price_qty {
  double px{};
  double qty{};

  bool operator==(const price_qty&) const = default;
};

// LedgerFlux's Tick (services/common/models.py; docs/contract.md §2). The constant fields
// v = 1 and type = "tick" are implicit. Timestamps are nanoseconds since the Unix epoch.
struct tick {
  std::string_view product;  // borrowed, typically from coinbase::parser
  std::int64_t seq{};
  std::int64_t ts_event{};
  std::int64_t ts_ingest{};
  std::optional<price_qty> last_trade;
  std::optional<price_qty> best_bid;
  std::optional<price_qty> best_ask;
};

// Why the LedgerFlux normalizer rules reject a tick (docs/contract.md §5).
enum class drop_reason : std::uint8_t {
  empty_product,
  non_positive_trade_price,
  non_positive_bid_price,
  non_positive_ask_price,
  crossed_book,  // ask <= bid, i.e. crossed or locked
};

std::string_view to_string(drop_reason reason) noexcept;

// Applies the normalizer's validation rules in its order; nullopt means the tick is valid.
std::optional<drop_reason> validate(const tick& t) noexcept;

// Appends the tick as compact JSON, byte-identical to pydantic's Tick.model_dump_json():
// same key order, absent sub-objects as null, floats formatted as json::append_double.
void append_json(std::string& out, const tick& t);

}  // namespace cratchet
