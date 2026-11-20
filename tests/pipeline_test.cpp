#include "cratchet/pipeline.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "cratchet/log.hpp"

namespace {

cratchet::ingestor_config test_config() {
  cratchet::ingestor_config config;
  config.products = {"BTC-USD"};
  config.subject_prefix = "test.ticks";
  return config;
}

constexpr std::string_view ticker =
    R"({"type":"ticker","sequence":5,"product_id":"BTC-USD","price":"10","last_size":"1",)"
    R"("best_bid":"9","best_bid_size":"2","best_ask":"11","best_ask_size":"3",)"
    R"("time":"1970-01-01T00:00:01Z"})";

TEST(pipeline, publishes_tickers_with_their_subject) {
  cratchet::log::set_min_level(cratchet::log::level::error);
  std::vector<std::pair<std::string, std::string>> published;
  cratchet::pipeline p(
      test_config(),
      [&](std::string_view subject, const cratchet::normalized_tick& tick) {
        published.emplace_back(subject, tick.json);
      },
      nullptr, [] { return std::int64_t{77}; });

  p.on_frame(R"({"type":"subscriptions","channels":[]})", 1);
  p.on_frame(ticker, 2);
  p.on_frame(R"({"type":"heartbeat","product_id":"BTC-USD","sequence":6,"last_trade_id":1,)"
             R"("time":"1970-01-01T00:00:02Z"})",
             3);
  p.on_frame(R"({"type":"l2update","product_id":"BTC-USD","time":"1970-01-01T00:00:02Z",)"
             R"("changes":[["buy","9","1"]]})",
             4);
  p.on_frame(R"({"type":"error","message":"bad","reason":"x"})", 5);
  p.on_frame("{not json", 6);

  ASSERT_EQ(published.size(), 1U);
  EXPECT_EQ(published[0].first, "test.ticks.1");  // BTC-USD -> shard 1 of 4
  EXPECT_EQ(published[0].second,
            R"({"v":1,"type":"tick","product":"BTC-USD","seq":5,"ts_event":1000000000,)"
            R"("ts_ingest":77,"fields":{"last_trade":{"px":10.0,"qty":1.0},)"
            R"("best_bid":{"px":9.0,"qty":2.0},"best_ask":{"px":11.0,"qty":3.0}}})");

  const auto& stats = p.stats();
  EXPECT_EQ(stats.frames, 6U);
  EXPECT_EQ(stats.tickers, 1U);
  EXPECT_EQ(stats.published, 1U);
  EXPECT_EQ(stats.heartbeats, 1U);
  EXPECT_EQ(stats.l2updates, 1U);
  EXPECT_EQ(stats.coinbase_errors, 1U);
  EXPECT_EQ(stats.other, 1U);
  EXPECT_EQ(stats.parse_errors, 1U);
  EXPECT_EQ(stats.parse_errors_by_code.at("invalid_json"), 1U);
  EXPECT_NE(cratchet::format_stats(stats).find("parse_error.invalid_json=1"), std::string::npos);
}

TEST(pipeline, counts_drops_and_out_of_order) {
  cratchet::log::set_min_level(cratchet::log::level::error);
  int published = 0;
  cratchet::pipeline p(test_config(),
                       [&](std::string_view, const cratchet::normalized_tick&) { ++published; });

  std::string crossed(ticker);
  crossed.replace(crossed.find(R"("best_ask":"11")"), 15, R"("best_ask":"8")");
  p.on_frame(crossed, 1);
  p.on_frame(ticker, 2);
  std::string earlier(ticker);
  earlier.replace(earlier.find(R"("sequence":5)"), 12, R"("sequence":4)");
  p.on_frame(earlier, 3);

  EXPECT_EQ(published, 2);
  EXPECT_EQ(p.stats().dropped, 1U);
  EXPECT_EQ(p.stats().drops_by_reason.at("crossed_book"), 1U);
  EXPECT_EQ(p.stats().out_of_order, 1U);
}

TEST(pipeline, records_every_frame_raw) {
  cratchet::log::set_min_level(cratchet::log::level::error);
  const auto path = std::filesystem::temp_directory_path() / "cratchet_pipeline_record.jsonl";
  std::filesystem::remove(path);
  {
    cratchet::capture_writer recorder(path.string());
    cratchet::pipeline p(test_config(), {}, &recorder);
    p.on_frame(ticker, 11);
    p.on_frame("garbage", 12);  // recorded even though it does not parse
  }
  cratchet::capture_reader reader(path.string());
  std::vector<std::pair<std::int64_t, std::string>> frames;
  while (const auto frame = reader.next()) {
    frames.emplace_back(frame->recv_ns, frame->msg);
  }
  EXPECT_EQ(frames, (std::vector<std::pair<std::int64_t, std::string>>{{11, std::string(ticker)},
                                                                       {12, "garbage"}}));
  std::filesystem::remove(path);
}

}  // namespace
