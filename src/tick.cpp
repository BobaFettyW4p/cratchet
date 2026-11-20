#include "cratchet/tick.hpp"

#include "cratchet/json.hpp"

namespace cratchet {

std::string_view to_string(drop_reason reason) noexcept {
  switch (reason) {
    case drop_reason::empty_product:
      return "empty_product";
    case drop_reason::non_positive_trade_price:
      return "non_positive_trade_price";
    case drop_reason::non_positive_bid_price:
      return "non_positive_bid_price";
    case drop_reason::non_positive_ask_price:
      return "non_positive_ask_price";
    case drop_reason::crossed_book:
      return "crossed_book";
  }
  return "unknown";
}

std::optional<drop_reason> validate(const tick& t) noexcept {
  if (t.product.empty()) {
    return drop_reason::empty_product;
  }
  if (t.last_trade && t.last_trade->px <= 0.0) {
    return drop_reason::non_positive_trade_price;
  }
  if (t.best_bid && t.best_bid->px <= 0.0) {
    return drop_reason::non_positive_bid_price;
  }
  if (t.best_ask && t.best_ask->px <= 0.0) {
    return drop_reason::non_positive_ask_price;
  }
  if (t.best_bid && t.best_ask && t.best_ask->px <= t.best_bid->px) {
    return drop_reason::crossed_book;
  }
  return std::nullopt;
}

namespace {

void append_price_qty(std::string& out, const std::optional<price_qty>& value) {
  if (!value) {
    out += "null";
    return;
  }
  out += R"({"px":)";
  json::append_double(out, value->px);
  out += R"(,"qty":)";
  json::append_double(out, value->qty);
  out += '}';
}

}  // namespace

void append_json(std::string& out, const tick& t) {
  out += R"({"v":1,"type":"tick","product":)";
  json::append_string(out, t.product);
  out += R"(,"seq":)";
  json::append_int(out, t.seq);
  out += R"(,"ts_event":)";
  json::append_int(out, t.ts_event);
  out += R"(,"ts_ingest":)";
  json::append_int(out, t.ts_ingest);
  out += R"(,"fields":{"last_trade":)";
  append_price_qty(out, t.last_trade);
  out += R"(,"best_bid":)";
  append_price_qty(out, t.best_bid);
  out += R"(,"best_ask":)";
  append_price_qty(out, t.best_ask);
  out += "}}";
}

}  // namespace cratchet
