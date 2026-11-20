#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

// Typed Coinbase Exchange ws-feed messages (wss://ws-feed.exchange.coinbase.com).
// Shapes as observed on 2026-11-19; see docs/contract.md §9. Prices and sizes arrive as decimal
// strings and are converted to double with correct rounding, as Python's float() does.
//
// All string_views and spans point into the coinbase::parser that produced the message and stay
// valid only until that parser's next parse() call.
namespace cratchet::coinbase {

struct ticker {
  std::string_view product_id;
  std::int64_t sequence{};
  std::int64_t time_ns{};
  // Each is set only if its key is present in the message. LedgerFlux emits a Tick sub-object
  // only when both keys of a pair are present (price/last_size, best_bid/best_bid_size, ...).
  std::optional<double> price;
  std::optional<double> last_size;
  std::optional<double> best_bid;
  std::optional<double> best_bid_size;
  std::optional<double> best_ask;
  std::optional<double> best_ask_size;
};

struct heartbeat {
  std::string_view product_id;
  std::int64_t sequence{};
  std::int64_t last_trade_id{};
  std::int64_t time_ns{};
};

// Subscription acknowledgement. Contents are not needed yet.
struct subscriptions {};

struct error_message {
  std::string_view message;
  std::string_view reason;  // empty if absent
};

struct price_level {
  double price{};
  double size{};
};

// Full-depth level2 book, sent once per product on subscribe. Carries no sequence number.
struct snapshot {
  std::string_view product_id;
  std::optional<std::int64_t> time_ns;
  std::span<const price_level> bids;
  std::span<const price_level> asks;
};

enum class side : std::uint8_t { buy, sell };

struct level_change {
  coinbase::side side{};
  double price{};
  double size{};  // new total size at this price; 0 removes the level
};

// Incremental level2 update. Carries no sequence number.
struct l2update {
  std::string_view product_id;
  std::int64_t time_ns{};
  std::span<const level_change> changes;
};

// Any message with a "type" not listed above.
struct unknown {
  std::string_view type;
};

using message =
    std::variant<ticker, heartbeat, subscriptions, error_message, snapshot, l2update, unknown>;

}  // namespace cratchet::coinbase
