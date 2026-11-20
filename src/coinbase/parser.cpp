#include "cratchet/coinbase/parser.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <system_error>
#include <utility>

#include "cratchet/timestamp.hpp"

namespace cratchet::coinbase {

std::string_view to_string(parse_error_code code) noexcept {
  switch (code) {
    case parse_error_code::invalid_json:
      return "invalid_json";
    case parse_error_code::not_an_object:
      return "not_an_object";
    case parse_error_code::missing_type:
      return "missing_type";
    case parse_error_code::missing_field:
      return "missing_field";
    case parse_error_code::wrong_type:
      return "wrong_type";
    case parse_error_code::bad_number:
      return "bad_number";
    case parse_error_code::bad_timestamp:
      return "bad_timestamp";
    case parse_error_code::bad_level:
      return "bad_level";
  }
  return "unknown";
}

namespace {

namespace od = simdjson::ondemand;
using parse_result = result<message, parse_error>;
// Per-field decode outcome: nullopt on success.
using field_status = std::optional<parse_error>;

constexpr field_status ok{};

parse_error error(parse_error_code code, std::string_view field = {}) { return {code, field}; }

// Decimal string to double, correctly rounded. Rejects trailing junk and non-finite values.
std::optional<double> parse_decimal(std::string_view text) noexcept {
  double value = 0.0;
  const char* const last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), last, value);
  if (ec != std::errc{} || ptr != last || !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

field_status read_string(od::value& value, std::string_view key, std::string_view& out) {
  if (value.get_string().get(out) != simdjson::SUCCESS) {
    return error(parse_error_code::wrong_type, key);
  }
  return ok;
}

field_status read_int(od::value& value, std::string_view key, std::int64_t& out) {
  std::int64_t parsed = 0;
  const auto code = value.get_int64().get(parsed);
  if (code == simdjson::INCORRECT_TYPE) {
    return error(parse_error_code::wrong_type, key);
  }
  if (code != simdjson::SUCCESS) {
    return error(parse_error_code::bad_number, key);
  }
  out = parsed;
  return ok;
}

field_status read_decimal(od::value& value, std::string_view key, std::optional<double>& out) {
  std::string_view text;
  if (auto status = read_string(value, key, text)) {
    return status;
  }
  out = parse_decimal(text);
  if (!out) {
    return error(parse_error_code::bad_number, key);
  }
  return ok;
}

field_status read_time(od::value& value, std::string_view key, std::int64_t& out) {
  std::string_view text;
  if (auto status = read_string(value, key, text)) {
    return status;
  }
  const auto ns = parse_rfc3339_ns(text);
  if (!ns) {
    return error(parse_error_code::bad_timestamp, key);
  }
  out = *ns;
  return ok;
}

// Reads up to N strings from a JSON array into `out`; returns how many the array holds, or
// nullopt if it is not an array of strings.
template <std::size_t N>
std::optional<std::size_t> read_string_tuple(od::value& value,
                                             std::array<std::string_view, N>& out) {
  od::array array;
  if (value.get_array().get(array) != simdjson::SUCCESS) {
    return std::nullopt;
  }
  std::size_t count = 0;
  for (auto element : array) {
    std::string_view text;
    if (element.get_string().get(text) != simdjson::SUCCESS) {
      return std::nullopt;
    }
    if (count < N) {
      out.at(count) = text;
    }
    ++count;
  }
  return count;
}

// "bids"/"asks": [[price, size], ...]
field_status read_levels(od::value& value, std::string_view key, std::vector<price_level>& out) {
  out.clear();
  od::array array;
  if (value.get_array().get(array) != simdjson::SUCCESS) {
    return error(parse_error_code::wrong_type, key);
  }
  for (auto element : array) {
    od::value entry;
    std::array<std::string_view, 2> parts{};
    if (element.get(entry) != simdjson::SUCCESS) {
      return error(parse_error_code::invalid_json, key);
    }
    const auto count = read_string_tuple(entry, parts);
    if (!count || *count < 2) {
      return error(parse_error_code::bad_level, key);
    }
    const auto price = parse_decimal(parts[0]);
    const auto size = parse_decimal(parts[1]);
    if (!price || !size) {
      return error(parse_error_code::bad_level, key);
    }
    out.push_back({*price, *size});
  }
  return ok;
}

// "changes": [[side, price, size], ...]
field_status read_changes(od::value& value, std::string_view key, std::vector<level_change>& out) {
  out.clear();
  od::array array;
  if (value.get_array().get(array) != simdjson::SUCCESS) {
    return error(parse_error_code::wrong_type, key);
  }
  for (auto element : array) {
    od::value entry;
    std::array<std::string_view, 3> parts{};
    if (element.get(entry) != simdjson::SUCCESS) {
      return error(parse_error_code::invalid_json, key);
    }
    const auto count = read_string_tuple(entry, parts);
    if (!count || *count < 3) {
      return error(parse_error_code::bad_level, key);
    }
    side change_side{};
    if (parts[0] == "buy") {
      change_side = side::buy;
    } else if (parts[0] == "sell") {
      change_side = side::sell;
    } else {
      return error(parse_error_code::bad_level, key);
    }
    const auto price = parse_decimal(parts[1]);
    const auto size = parse_decimal(parts[2]);
    if (!price || !size) {
      return error(parse_error_code::bad_level, key);
    }
    out.push_back({change_side, *price, *size});
  }
  return ok;
}

// Calls handler(key, value) for every field of `object`, stopping at the first error.
template <typename Handler>
field_status for_each_field(od::object& object, const Handler& handler) {
  for (auto element : object) {
    od::field field;
    std::string_view key;
    if (std::move(element).get(field) != simdjson::SUCCESS ||
        field.unescaped_key().get(key) != simdjson::SUCCESS) {
      return error(parse_error_code::invalid_json);
    }
    if (auto status = handler(key, field.value())) {
      return status;
    }
  }
  return ok;
}

// Tracks which required fields a message has seen, by bit.
class required_fields {
 public:
  void see(std::uint8_t bit) noexcept { seen_ |= 1U << bit; }
  [[nodiscard]] bool has(std::uint8_t bit) const noexcept { return (seen_ & (1U << bit)) != 0; }

 private:
  unsigned seen_ = 0;
};

parse_result parse_ticker(od::object& object) {
  ticker msg;
  enum : std::uint8_t { product_bit, sequence_bit, time_bit };
  required_fields seen;
  auto status = for_each_field(object, [&](std::string_view key, od::value& value) {
    if (key == "product_id") {
      seen.see(product_bit);
      return read_string(value, key, msg.product_id);
    }
    if (key == "sequence") {
      seen.see(sequence_bit);
      return read_int(value, key, msg.sequence);
    }
    if (key == "time") {
      seen.see(time_bit);
      return read_time(value, key, msg.time_ns);
    }
    if (key == "price") {
      return read_decimal(value, key, msg.price);
    }
    if (key == "last_size") {
      return read_decimal(value, key, msg.last_size);
    }
    if (key == "best_bid") {
      return read_decimal(value, key, msg.best_bid);
    }
    if (key == "best_bid_size") {
      return read_decimal(value, key, msg.best_bid_size);
    }
    if (key == "best_ask") {
      return read_decimal(value, key, msg.best_ask);
    }
    if (key == "best_ask_size") {
      return read_decimal(value, key, msg.best_ask_size);
    }
    return ok;
  });
  if (status) {
    return parse_result::fail(*status);
  }
  if (!seen.has(product_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "product_id"));
  }
  if (!seen.has(sequence_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "sequence"));
  }
  if (!seen.has(time_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "time"));
  }
  return parse_result::ok(msg);
}

parse_result parse_heartbeat(od::object& object) {
  heartbeat msg;
  enum : std::uint8_t { product_bit, sequence_bit, trade_bit, time_bit };
  required_fields seen;
  auto status = for_each_field(object, [&](std::string_view key, od::value& value) {
    if (key == "product_id") {
      seen.see(product_bit);
      return read_string(value, key, msg.product_id);
    }
    if (key == "sequence") {
      seen.see(sequence_bit);
      return read_int(value, key, msg.sequence);
    }
    if (key == "last_trade_id") {
      seen.see(trade_bit);
      return read_int(value, key, msg.last_trade_id);
    }
    if (key == "time") {
      seen.see(time_bit);
      return read_time(value, key, msg.time_ns);
    }
    return ok;
  });
  if (status) {
    return parse_result::fail(*status);
  }
  if (!seen.has(product_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "product_id"));
  }
  if (!seen.has(sequence_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "sequence"));
  }
  if (!seen.has(trade_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "last_trade_id"));
  }
  if (!seen.has(time_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "time"));
  }
  return parse_result::ok(msg);
}

parse_result parse_error_message(od::object& object) {
  error_message msg;
  bool has_message = false;
  auto status = for_each_field(object, [&](std::string_view key, od::value& value) {
    if (key == "message") {
      has_message = true;
      return read_string(value, key, msg.message);
    }
    if (key == "reason") {
      return read_string(value, key, msg.reason);
    }
    return ok;
  });
  if (status) {
    return parse_result::fail(*status);
  }
  if (!has_message) {
    return parse_result::fail(error(parse_error_code::missing_field, "message"));
  }
  return parse_result::ok(msg);
}

parse_result parse_snapshot(od::object& object, std::vector<price_level>& bids,
                            std::vector<price_level>& asks) {
  snapshot msg;
  enum : std::uint8_t { product_bit, bids_bit, asks_bit };
  required_fields seen;
  auto status = for_each_field(object, [&](std::string_view key, od::value& value) {
    if (key == "product_id") {
      seen.see(product_bit);
      return read_string(value, key, msg.product_id);
    }
    if (key == "time") {
      std::int64_t time_ns = 0;
      auto time_status = read_time(value, key, time_ns);
      if (!time_status) {
        msg.time_ns = time_ns;
      }
      return time_status;
    }
    if (key == "bids") {
      seen.see(bids_bit);
      return read_levels(value, key, bids);
    }
    if (key == "asks") {
      seen.see(asks_bit);
      return read_levels(value, key, asks);
    }
    return ok;
  });
  if (status) {
    return parse_result::fail(*status);
  }
  if (!seen.has(product_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "product_id"));
  }
  if (!seen.has(bids_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "bids"));
  }
  if (!seen.has(asks_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "asks"));
  }
  msg.bids = bids;
  msg.asks = asks;
  return parse_result::ok(msg);
}

parse_result parse_l2update(od::object& object, std::vector<level_change>& changes) {
  l2update msg;
  enum : std::uint8_t { product_bit, time_bit, changes_bit };
  required_fields seen;
  auto status = for_each_field(object, [&](std::string_view key, od::value& value) {
    if (key == "product_id") {
      seen.see(product_bit);
      return read_string(value, key, msg.product_id);
    }
    if (key == "time") {
      seen.see(time_bit);
      return read_time(value, key, msg.time_ns);
    }
    if (key == "changes") {
      seen.see(changes_bit);
      return read_changes(value, key, changes);
    }
    return ok;
  });
  if (status) {
    return parse_result::fail(*status);
  }
  if (!seen.has(product_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "product_id"));
  }
  if (!seen.has(time_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "time"));
  }
  if (!seen.has(changes_bit)) {
    return parse_result::fail(error(parse_error_code::missing_field, "changes"));
  }
  msg.changes = changes;
  return parse_result::ok(msg);
}

}  // namespace

result<message, parse_error> parser::parse(std::string_view frame) {
  buffer_.resize(frame.size() + simdjson::SIMDJSON_PADDING);
  std::copy(frame.begin(), frame.end(), buffer_.begin());
  const simdjson::padded_string_view padded(buffer_.data(), frame.size(), buffer_.size());

  od::document document;
  if (json_.iterate(padded).get(document) != simdjson::SUCCESS) {
    return parse_result::fail(error(parse_error_code::invalid_json));
  }
  od::object object;
  if (const auto code = document.get_object().get(object); code != simdjson::SUCCESS) {
    return parse_result::fail(error(code == simdjson::INCORRECT_TYPE
                                        ? parse_error_code::not_an_object
                                        : parse_error_code::invalid_json));
  }

  // Coinbase sends "type" first, so this lookup is normally one field; then rewind and decode
  // the whole object in one pass.
  std::string_view type;
  if (const auto code = object.find_field_unordered("type").get_string().get(type);
      code != simdjson::SUCCESS) {
    return parse_result::fail(
        error(code == simdjson::NO_SUCH_FIELD || code == simdjson::INCORRECT_TYPE
                  ? parse_error_code::missing_type
                  : parse_error_code::invalid_json,
              "type"));
  }

  if (type == "subscriptions") {
    return parse_result::ok(subscriptions{});
  }
  const bool known = type == "ticker" || type == "heartbeat" || type == "error" ||
                     type == "snapshot" || type == "l2update";
  if (!known) {
    return parse_result::ok(unknown{type});
  }

  bool empty = false;
  if (object.reset().get(empty) != simdjson::SUCCESS) {
    return parse_result::fail(error(parse_error_code::invalid_json));
  }
  if (type == "ticker") {
    return parse_ticker(object);
  }
  if (type == "heartbeat") {
    return parse_heartbeat(object);
  }
  if (type == "error") {
    return parse_error_message(object);
  }
  if (type == "snapshot") {
    return parse_snapshot(object, bids_, asks_);
  }
  return parse_l2update(object, changes_);
}

}  // namespace cratchet::coinbase
