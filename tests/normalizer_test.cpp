#include "cratchet/normalizer.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace {

using cratchet::drop_reason;
using cratchet::coinbase::ticker;

ticker full_ticker() {
  ticker t;
  t.product_id = "ADA-USD";
  t.sequence = 17858688817;
  t.time_ns = 1795053203200696000;
  t.price = 0.15079;
  t.last_size = 21.8;
  t.best_bid = 0.15079;
  t.best_bid_size = 1068.9;
  t.best_ask = 0.1508;
  t.best_ask_size = 358.58911845;
  return t;
}

TEST(normalizer, make_tick_pairs_fields_like_ledgerflux) {
  auto msg = full_ticker();
  msg.last_size.reset();      // price without size: no last_trade
  msg.best_ask_size.reset();  // likewise for the ask
  const auto t = cratchet::make_tick(msg, 42);
  EXPECT_EQ(t.product, "ADA-USD");
  EXPECT_EQ(t.seq, 17858688817);
  EXPECT_EQ(t.ts_event, 1795053203200696000);
  EXPECT_EQ(t.ts_ingest, 42);
  EXPECT_FALSE(t.last_trade);
  EXPECT_EQ(t.best_bid, (std::optional<cratchet::price_qty>{{0.15079, 1068.9}}));
  EXPECT_FALSE(t.best_ask);
}

// Expected JSON is LedgerFlux's own output for this frame (tests/data/coinbase_sample.ticks.jsonl,
// line 1), with ts_event replaced by the exact value: Python's float path gives ...696064.
TEST(normalizer, serializes_like_pydantic) {
  cratchet::normalizer n(4);
  const auto out = n.normalize(full_ticker(), 0);
  ASSERT_TRUE(out);
  EXPECT_EQ(out.value().json,
            R"({"v":1,"type":"tick","product":"ADA-USD","seq":17858688817,)"
            R"("ts_event":1795053203200696000,"ts_ingest":0,"fields":{)"
            R"("last_trade":{"px":0.15079,"qty":21.8},"best_bid":{"px":0.15079,"qty":1068.9},)"
            R"("best_ask":{"px":0.1508,"qty":358.58911845}}})");
  EXPECT_EQ(out.value().shard, 0U);
  EXPECT_FALSE(out.value().out_of_order);
}

TEST(normalizer, absent_fields_serialize_as_null) {
  ticker msg;
  msg.product_id = "BTC-USD";
  msg.sequence = 1;
  msg.time_ns = 2;
  msg.best_bid = 1.0;
  msg.best_bid_size = 2.0;
  cratchet::normalizer n(4);
  const auto out = n.normalize(msg, 3);
  ASSERT_TRUE(out);
  // Byte-identical to pydantic's output for the same Tick.
  EXPECT_EQ(out.value().json,
            R"({"v":1,"type":"tick","product":"BTC-USD","seq":1,"ts_event":2,"ts_ingest":3,)"
            R"("fields":{"last_trade":null,"best_bid":{"px":1.0,"qty":2.0},"best_ask":null}})");
  EXPECT_EQ(out.value().shard, 1U);
}

TEST(normalizer, drops_ticks_that_fail_validation) {
  cratchet::normalizer n(4);
  auto expect_drop = [&](ticker msg, drop_reason reason) {
    const auto out = n.normalize(msg, 0);
    ASSERT_FALSE(out);
    EXPECT_EQ(out.error(), reason) << to_string(reason);
  };

  auto msg = full_ticker();
  msg.product_id = "";
  expect_drop(msg, drop_reason::empty_product);

  msg = full_ticker();
  msg.price = 0.0;
  expect_drop(msg, drop_reason::non_positive_trade_price);

  msg = full_ticker();
  msg.best_bid = -1.0;
  expect_drop(msg, drop_reason::non_positive_bid_price);

  msg = full_ticker();
  msg.best_ask = 0.0;
  expect_drop(msg, drop_reason::non_positive_ask_price);

  msg = full_ticker();
  msg.best_ask = msg.best_bid;  // locked
  expect_drop(msg, drop_reason::crossed_book);

  msg = full_ticker();
  msg.best_ask = 0.15;  // crossed
  expect_drop(msg, drop_reason::crossed_book);

  // A non-positive price in a half-present pair is not part of the tick, so it can't drop it.
  msg = full_ticker();
  msg.price = -1.0;
  msg.last_size.reset();
  EXPECT_TRUE(n.normalize(msg, 0));
}

TEST(normalizer, flags_out_of_order_sequences_without_dropping) {
  cratchet::normalizer n(4);
  auto msg = full_ticker();
  msg.sequence = 100;
  EXPECT_FALSE(n.normalize(msg, 0).value().out_of_order);

  msg.sequence = 150;  // gaps are normal on the ticker channel
  EXPECT_FALSE(n.normalize(msg, 0).value().out_of_order);

  msg.sequence = 120;
  const auto late = n.normalize(msg, 0);
  ASSERT_TRUE(late);
  EXPECT_TRUE(late.value().out_of_order);

  // The late tick becomes the new reference point, as in LedgerFlux.
  msg.sequence = 130;
  EXPECT_FALSE(n.normalize(msg, 0).value().out_of_order);

  // Ordering is per product.
  auto other = full_ticker();
  other.product_id = "BTC-USD";
  other.sequence = 1;
  EXPECT_FALSE(n.normalize(other, 0).value().out_of_order);

  // Dropped ticks don't move the reference point.
  msg.sequence = 10;
  msg.best_ask = 0.0;
  EXPECT_FALSE(n.normalize(msg, 0));
  msg = full_ticker();
  msg.sequence = 131;
  EXPECT_FALSE(n.normalize(msg, 0).value().out_of_order);
}

TEST(normalizer, zero_shards_is_rejected) {
  EXPECT_THROW(cratchet::normalizer(0), std::invalid_argument);
}

}  // namespace
