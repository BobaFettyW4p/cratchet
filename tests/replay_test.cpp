// Replays the committed Coinbase capture through the parser and normalizer, and compares every
// tick with what LedgerFlux's Python transform produced for the same frame
// (tests/data/README.md; docs/contract.md §8).

#include <gtest/gtest.h>
#include <simdjson.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cratchet/coinbase/parser.hpp"
#include "cratchet/normalizer.hpp"
#include "cratchet/shard.hpp"

namespace {

std::vector<std::string> read_lines(const std::string& path) {
  std::ifstream in(path);
  EXPECT_TRUE(in) << "cannot open " << path;
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  return lines;
}

// Capture lines are {"recv_ns": ..., "msg": "<raw frame>"}.
std::string frame_of(simdjson::ondemand::parser& json, const std::string& line) {
  simdjson::padded_string padded(line);
  std::string_view msg;
  auto doc = json.iterate(padded);
  EXPECT_EQ(doc["msg"].get_string().get(msg), simdjson::SUCCESS) << line;
  return std::string(msg);
}

std::int64_t ts_event_of(simdjson::ondemand::parser& json, const std::string& tick_json) {
  simdjson::padded_string padded(tick_json);
  std::int64_t ts = 0;
  auto doc = json.iterate(padded);
  EXPECT_EQ(doc["ts_event"].get_int64().get(ts), simdjson::SUCCESS) << tick_json;
  return ts;
}

// Replaces the ts_event digits in a Tick JSON string.
std::string with_ts_event(std::string tick_json, std::int64_t ts_event) {
  constexpr std::string_view key = R"("ts_event":)";
  const auto start = tick_json.find(key) + key.size();
  const auto end = tick_json.find(',', start);
  tick_json.replace(start, end - start, std::to_string(ts_event));
  return tick_json;
}

TEST(replay, capture_matches_ledgerflux_transform) {
  const std::string dir = CRATCHET_TEST_DATA_DIR;
  const auto capture = read_lines(dir + "/coinbase_sample.jsonl");
  const auto golden = read_lines(dir + "/coinbase_sample.ticks.jsonl");
  ASSERT_FALSE(capture.empty());

  simdjson::ondemand::parser json;
  cratchet::coinbase::parser parser;
  cratchet::normalizer normalizer(4);
  std::map<std::string, int> counts;
  std::size_t tick_index = 0;

  for (const auto& line : capture) {
    const std::string frame = frame_of(json, line);
    auto parsed = parser.parse(frame);
    ASSERT_TRUE(parsed) << to_string(parsed.error().code) << " '" << parsed.error().field << "' in "
                        << frame;

    std::visit(
        [&](const auto& msg) {
          using T = std::decay_t<decltype(msg)>;
          if constexpr (std::is_same_v<T, cratchet::coinbase::ticker>) {
            ++counts["ticker"];
            auto out = normalizer.normalize(msg, 0);
            ASSERT_TRUE(out) << to_string(out.error()) << " in " << frame;
            ASSERT_LT(tick_index, golden.size());
            const std::string& expected = golden[tick_index++];

            // Python derives ts_event through a float and can be off by a few hundred ns;
            // the contract allows |delta| < 1 us. Everything else must match byte for byte.
            const auto ours = out.value().value.ts_event;
            const auto theirs = ts_event_of(json, expected);
            EXPECT_LT(std::llabs(ours - theirs), 1000) << frame;
            EXPECT_EQ(out.value().json, with_ts_event(expected, ours)) << frame;
            EXPECT_EQ(out.value().shard, cratchet::shard_for(msg.product_id, 4));
            EXPECT_FALSE(out.value().out_of_order) << frame;
          } else if constexpr (std::is_same_v<T, cratchet::coinbase::heartbeat>) {
            ++counts["heartbeat"];
          } else if constexpr (std::is_same_v<T, cratchet::coinbase::l2update>) {
            ++counts["l2update"];
            EXPECT_FALSE(msg.changes.empty());
          } else if constexpr (std::is_same_v<T, cratchet::coinbase::subscriptions>) {
            ++counts["subscriptions"];
          } else {
            ADD_FAILURE() << "unexpected message: " << frame;
          }
        },
        parsed.value());
  }

  EXPECT_EQ(tick_index, golden.size());
  EXPECT_EQ(counts["ticker"], 604);
  EXPECT_EQ(counts["heartbeat"], 60);
  EXPECT_EQ(counts["l2update"], 41);
  EXPECT_EQ(counts["subscriptions"], 1);
}

}  // namespace
