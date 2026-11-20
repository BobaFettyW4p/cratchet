#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

#include "cratchet/coinbase/parser.hpp"

namespace {

using namespace cratchet::coinbase;

// A real frame from the 2026-11-19 capture.
constexpr std::string_view ticker_frame =
    R"({"type":"ticker","sequence":17858688817,"product_id":"ADA-USD","price":"0.15079",)"
    R"("open_24h":"0.15278","volume_24h":"55453899.08438020","low_24h":"0.148",)"
    R"("high_24h":"0.15523","volume_30d":"1310066805.23086565","best_bid":"0.15079",)"
    R"("best_bid_size":"1068.90000000","best_ask":"0.15080","best_ask_size":"358.58911845",)"
    R"("side":"sell","time":"2026-11-19T01:53:23.200696Z","trade_id":172124132,"last_size":"21.8"})";

template <typename T>
T parse_as(parser& p, std::string_view frame) {
  auto parsed = p.parse(frame);
  if (!parsed) {
    ADD_FAILURE() << "parse failed: " << to_string(parsed.error().code) << " field '"
                  << parsed.error().field << "'";
    return T{};
  }
  if (!std::holds_alternative<T>(parsed.value())) {
    ADD_FAILURE() << "wrong message type, variant index " << parsed.value().index();
    return T{};
  }
  return std::get<T>(parsed.value());
}

parse_error parse_failure(parser& p, std::string_view frame) {
  auto parsed = p.parse(frame);
  if (parsed) {
    ADD_FAILURE() << "expected a parse error for: " << frame;
    return {};
  }
  return parsed.error();
}

TEST(coinbase_parser, ticker) {
  parser p;
  const auto msg = parse_as<ticker>(p, ticker_frame);
  EXPECT_EQ(msg.product_id, "ADA-USD");
  EXPECT_EQ(msg.sequence, 17858688817);
  EXPECT_EQ(msg.time_ns, 1795053203200696000);
  EXPECT_EQ(msg.price, 0.15079);
  EXPECT_EQ(msg.last_size, 21.8);
  EXPECT_EQ(msg.best_bid, 0.15079);
  EXPECT_EQ(msg.best_bid_size, 1068.9);
  EXPECT_EQ(msg.best_ask, 0.1508);
  EXPECT_EQ(msg.best_ask_size, 358.58911845);
}

TEST(coinbase_parser, ticker_fields_in_any_order_and_optional_prices) {
  parser p;
  const auto msg = parse_as<ticker>(
      p, R"({"time":"1970-01-01T00:00:01Z","best_bid":"1.5","product_id":"BTC-USD",)"
         R"("sequence":7,"type":"ticker"})");
  EXPECT_EQ(msg.product_id, "BTC-USD");
  EXPECT_EQ(msg.sequence, 7);
  EXPECT_EQ(msg.time_ns, 1'000'000'000);
  EXPECT_EQ(msg.best_bid, 1.5);
  EXPECT_FALSE(msg.best_bid_size);
  EXPECT_FALSE(msg.price);
  EXPECT_FALSE(msg.best_ask);
}

TEST(coinbase_parser, ticker_errors) {
  parser p;
  constexpr std::string_view base = R"("type":"ticker","product_id":"BTC-USD")";
  auto frame = [&](std::string_view extra) {
    return "{" + std::string(base) + std::string(extra) + "}";
  };

  auto err = parse_failure(p, frame(R"(,"time":"1970-01-01T00:00:00Z")"));
  EXPECT_EQ(err.code, parse_error_code::missing_field);
  EXPECT_EQ(err.field, "sequence");

  err = parse_failure(p, frame(R"(,"sequence":1)"));
  EXPECT_EQ(err.code, parse_error_code::missing_field);
  EXPECT_EQ(err.field, "time");

  err = parse_failure(p, frame(R"(,"sequence":"1","time":"1970-01-01T00:00:00Z")"));
  EXPECT_EQ(err.code, parse_error_code::wrong_type);
  EXPECT_EQ(err.field, "sequence");

  err = parse_failure(p, frame(R"(,"sequence":1,"time":"yesterday")"));
  EXPECT_EQ(err.code, parse_error_code::bad_timestamp);

  for (const char* bad_price : {R"("")", R"("1.5x")", R"("nan")", R"("inf")", R"("1e999")"}) {
    err = parse_failure(
        p,
        frame(std::string(R"(,"sequence":1,"time":"1970-01-01T00:00:00Z","price":)") + bad_price));
    EXPECT_EQ(err.code, parse_error_code::bad_number) << bad_price;
    EXPECT_EQ(err.field, "price");
  }

  err = parse_failure(p, frame(R"(,"sequence":1,"time":"1970-01-01T00:00:00Z","price":1.5)"));
  EXPECT_EQ(err.code, parse_error_code::wrong_type);
}

TEST(coinbase_parser, heartbeat) {
  parser p;
  const auto msg = parse_as<heartbeat>(
      p, R"({"type":"heartbeat","last_trade_id":856451841,"product_id":"ETH-USD",)"
         R"("sequence":105356086194,"time":"2026-11-19T01:53:27.000000Z"})");
  EXPECT_EQ(msg.product_id, "ETH-USD");
  EXPECT_EQ(msg.sequence, 105356086194);
  EXPECT_EQ(msg.last_trade_id, 856451841);
  EXPECT_EQ(msg.time_ns, 1795053207000000000);
}

TEST(coinbase_parser, snapshot) {
  parser p;
  const auto msg = parse_as<snapshot>(
      p,
      R"({"type":"snapshot","product_id":"BTC-USD","bids":[["62692.47","0.18769802"],)"
      R"(["62692.46","1.5"]],"asks":[["62692.48","0.01"]],"time":"2026-11-19T01:53:26.618406Z"})");
  EXPECT_EQ(msg.product_id, "BTC-USD");
  EXPECT_EQ(msg.time_ns, 1795053206618406000);
  ASSERT_EQ(msg.bids.size(), 2U);
  EXPECT_EQ(msg.bids[0].price, 62692.47);
  EXPECT_EQ(msg.bids[0].size, 0.18769802);
  EXPECT_EQ(msg.bids[1].price, 62692.46);
  ASSERT_EQ(msg.asks.size(), 1U);
  EXPECT_EQ(msg.asks[0].size, 0.01);

  const auto bare =
      parse_as<snapshot>(p, R"({"type":"snapshot","product_id":"X","bids":[],"asks":[]})");
  EXPECT_FALSE(bare.time_ns);
  EXPECT_TRUE(bare.bids.empty());

  auto err = parse_failure(p, R"({"type":"snapshot","product_id":"X","bids":[["1"]],"asks":[]})");
  EXPECT_EQ(err.code, parse_error_code::bad_level);
  EXPECT_EQ(err.field, "bids");
}

TEST(coinbase_parser, l2update) {
  parser p;
  const auto msg = parse_as<l2update>(
      p, R"({"type":"l2update","product_id":"BTC-USD","changes":[["buy","54855.92","0.01432656"],)"
         R"(["sell","62744.67","0.00000000"]],"time":"2026-11-19T01:53:26.618406Z"})");
  EXPECT_EQ(msg.product_id, "BTC-USD");
  EXPECT_EQ(msg.time_ns, 1795053206618406000);
  ASSERT_EQ(msg.changes.size(), 2U);
  EXPECT_EQ(msg.changes[0].side, side::buy);
  EXPECT_EQ(msg.changes[0].price, 54855.92);
  EXPECT_EQ(msg.changes[0].size, 0.01432656);
  EXPECT_EQ(msg.changes[1].side, side::sell);
  EXPECT_EQ(msg.changes[1].size, 0.0);

  auto err = parse_failure(
      p,
      R"({"type":"l2update","product_id":"X","time":"1970-01-01T00:00:00Z","changes":[["hold","1","1"]]})");
  EXPECT_EQ(err.code, parse_error_code::bad_level);
}

TEST(coinbase_parser, error_subscriptions_and_unknown) {
  parser p;
  const auto err_msg = parse_as<error_message>(
      p, R"({"type":"error","message":"Failed to subscribe","reason":"level2 requires auth"})");
  EXPECT_EQ(err_msg.message, "Failed to subscribe");
  EXPECT_EQ(err_msg.reason, "level2 requires auth");

  EXPECT_EQ(parse_as<error_message>(p, R"({"type":"error","message":"x"})").reason, "");

  (void)parse_as<subscriptions>(p, R"({"type":"subscriptions","channels":[]})");
  EXPECT_EQ(parse_as<unknown>(p, R"({"type":"status","products":[1,2,3]})").type, "status");
}

TEST(coinbase_parser, envelope_errors) {
  parser p;
  EXPECT_EQ(parse_failure(p, "").code, parse_error_code::invalid_json);
  EXPECT_EQ(parse_failure(p, "{not json").code, parse_error_code::invalid_json);
  EXPECT_EQ(parse_failure(p, "[1,2]").code, parse_error_code::not_an_object);
  EXPECT_EQ(parse_failure(p, R"({"product_id":"BTC-USD"})").code, parse_error_code::missing_type);
  EXPECT_EQ(parse_failure(p, R"({"type":5})").code, parse_error_code::missing_type);
}

TEST(coinbase_parser, parser_is_reusable_after_errors) {
  parser p;
  (void)parse_failure(p, "{broken");
  EXPECT_EQ(parse_as<ticker>(p, ticker_frame).sequence, 17858688817);
  (void)parse_failure(p, R"({"type":"ticker"})");
  EXPECT_EQ(parse_as<ticker>(p, ticker_frame).product_id, "ADA-USD");
}

}  // namespace
