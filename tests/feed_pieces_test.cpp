#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "cratchet/backoff.hpp"
#include "cratchet/capture.hpp"
#include "cratchet/coinbase/subscribe.hpp"
#include "cratchet/feed_client.hpp"

namespace {

using namespace std::chrono_literals;

TEST(backoff, doubles_with_jitter_up_to_the_cap) {
  cratchet::backoff b(100ms, 1000ms, 42);
  for (const auto ceiling : {100ms, 200ms, 400ms, 800ms, 1000ms, 1000ms}) {
    const auto delay = b.next();
    EXPECT_GE(delay, ceiling / 2);
    EXPECT_LE(delay, ceiling);
  }
  b.reset();
  EXPECT_LE(b.next(), 100ms);
}

TEST(backoff, rejects_bad_bounds) {
  EXPECT_THROW(cratchet::backoff(0ms, 1s), std::invalid_argument);
  EXPECT_THROW(cratchet::backoff(2s, 1s), std::invalid_argument);
}

TEST(subscribe, matches_ledgerflux_request_shape) {
  EXPECT_EQ(cratchet::coinbase::subscribe_message({"BTC-USD", "ETH-USD"}, {"ticker", "heartbeat"}),
            R"({"type":"subscribe","product_ids":["BTC-USD","ETH-USD"],)"
            R"("channels":["ticker","heartbeat"]})");
}

TEST(ws_uri, parses_feed_uris) {
  using cratchet::ws_endpoint;
  EXPECT_EQ(cratchet::parse_ws_uri("wss://ws-feed.exchange.coinbase.com"),
            (ws_endpoint{true, "ws-feed.exchange.coinbase.com", "443", "/"}));
  EXPECT_EQ(cratchet::parse_ws_uri("ws://127.0.0.1:9001/feed?x=1"),
            (ws_endpoint{false, "127.0.0.1", "9001", "/feed?x=1"}));
  EXPECT_EQ(cratchet::parse_ws_uri("ws://host?a=b"), (ws_endpoint{false, "host", "80", "/?a=b"}));

  for (const char* bad : {"https://x", "wss://", "wss://:443", "wss://h:0", "wss://h:99999",
                          "wss://h:abc", "wss://user@h"}) {
    EXPECT_FALSE(cratchet::parse_ws_uri(bad)) << bad;
  }
}

TEST(capture_writer, round_trips_through_the_reader) {
  const auto path = std::filesystem::temp_directory_path() / "cratchet_capture_writer_test.jsonl";
  std::filesystem::remove(path);
  {
    cratchet::capture_writer writer(path.string());
    writer.write(1, R"({"type":"ticker","price":"1.5"})");
    writer.write(2, "line\nbreak \"quoted\" \\ tab\t");
    EXPECT_EQ(writer.frames_written(), 2U);
  }
  std::ifstream raw(path);
  std::string first;
  std::getline(raw, first);
  // Same layout as scripts/capture_coinbase.py (json.dumps defaults).
  EXPECT_EQ(first, R"({"recv_ns": 1, "msg": "{\"type\":\"ticker\",\"price\":\"1.5\"}"})");

  cratchet::capture_reader reader(path.string());
  std::vector<std::pair<std::int64_t, std::string>> frames;
  while (const auto frame = reader.next()) {
    frames.emplace_back(frame->recv_ns, frame->msg);
  }
  EXPECT_EQ(frames, (std::vector<std::pair<std::int64_t, std::string>>{
                        {1, R"({"type":"ticker","price":"1.5"})"},
                        {2, "line\nbreak \"quoted\" \\ tab\t"}}));
  std::filesystem::remove(path);
}

}  // namespace
