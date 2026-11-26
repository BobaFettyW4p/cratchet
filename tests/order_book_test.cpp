// Order book tests. Expected values are not computed by the code under test: they were printed by
// `python3 scripts/book_reference.py`, which replays the same inputs into a plain dict-based book
// keyed by integer tick counts (Decimal arithmetic). Sizes are chosen to be exact in binary where
// hand-built.

#include "cratchet/order_book.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "cratchet/capture.hpp"
#include "cratchet/coinbase/parser.hpp"

namespace {

using cratchet::book_level;
using cratchet::order_book;
using cratchet::coinbase::l2update;
using cratchet::coinbase::level_change;
using cratchet::coinbase::price_level;
using cratchet::coinbase::side;
using cratchet::coinbase::snapshot;
using levels = std::vector<book_level>;

constexpr side buy = side::buy;
constexpr side sell = side::sell;

void apply_snapshot(order_book& book, const std::vector<price_level>& bids,
                    const std::vector<price_level>& asks) {
  snapshot snap;
  snap.bids = bids;
  snap.asks = asks;
  book.apply(snap);
}

void apply_changes(order_book& book, const std::vector<level_change>& changes) {
  l2update update;
  update.changes = changes;
  book.apply(update);
}

void expect_levels(const levels& actual, const levels& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    EXPECT_EQ(actual[i].price_ticks, expected[i].price_ticks) << "index " << i;
    EXPECT_DOUBLE_EQ(actual[i].size, expected[i].size) << "index " << i;
  }
}

void expect_book(const order_book& book, const levels& bids, const levels& asks) {
  expect_levels(book.depth(buy, bids.size() + 5), bids);
  expect_levels(book.depth(sell, asks.size() + 5), asks);
  EXPECT_EQ(book.level_count(buy), bids.size());
  EXPECT_EQ(book.level_count(sell), asks.size());
  ASSERT_TRUE(book.check_structure()) << book.check_structure().error();
}

// Snapshot used by the hand-built cases (tick 0.01).
order_book seeded_book() {
  order_book book(0.01);
  apply_snapshot(book, {{100.00, 1}, {99.50, 2}, {99.00, 3}},
                 {{100.50, 1}, {101.00, 2}, {101.50, 3}});
  return book;
}

// The reference script's "add top/mid/bottom" step.
void add_levels(order_book& book) {
  apply_changes(book, {{buy, 100.20, 4},
                       {buy, 99.75, 5},
                       {buy, 98.00, 6},
                       {sell, 100.30, 1.5},
                       {sell, 100.75, 2.5},
                       {sell, 102.00, 3.5}});
}

TEST(order_book, rejects_bad_tick_size) {
  EXPECT_THROW(order_book(0.0), std::invalid_argument);
  EXPECT_THROW(order_book(-0.01), std::invalid_argument);
  EXPECT_THROW(order_book(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST(order_book, snapshot_sets_both_sides) {
  const auto book = seeded_book();
  expect_book(book, {{10000, 1}, {9950, 2}, {9900, 3}}, {{10050, 1}, {10100, 2}, {10150, 3}});
  EXPECT_EQ(book.best_bid(), (book_level{10000, 1}));
  EXPECT_EQ(book.best_ask(), (book_level{10050, 1}));
  EXPECT_FALSE(book.is_crossed());
  EXPECT_TRUE(book.check_invariants());
  expect_levels(book.depth(buy, 2), {{10000, 1}, {9950, 2}});
  EXPECT_TRUE(book.depth(sell, 0).empty());
  EXPECT_DOUBLE_EQ(book.price_of(10050), 100.50);
}

TEST(order_book, snapshot_input_order_does_not_matter) {
  order_book book(0.01);
  apply_snapshot(book, {{99.00, 3}, {100.00, 1}, {99.50, 2}},
                 {{101.50, 3}, {100.50, 1}, {101.00, 2}});
  expect_book(book, {{10000, 1}, {9950, 2}, {9900, 3}}, {{10050, 1}, {10100, 2}, {10150, 3}});
}

TEST(order_book, add_resize_remove_at_top_middle_bottom) {
  auto book = seeded_book();

  add_levels(book);
  expect_book(book, {{10020, 4}, {10000, 1}, {9975, 5}, {9950, 2}, {9900, 3}, {9800, 6}},
              {{10030, 1.5}, {10050, 1}, {10075, 2.5}, {10100, 2}, {10150, 3}, {10200, 3.5}});

  apply_changes(book, {{buy, 100.20, 7},
                       {buy, 99.50, 8},
                       {buy, 98.00, 9},
                       {sell, 100.30, 1.25},
                       {sell, 101.00, 2.25},
                       {sell, 102.00, 3.25}});
  expect_book(book, {{10020, 7}, {10000, 1}, {9975, 5}, {9950, 8}, {9900, 3}, {9800, 9}},
              {{10030, 1.25}, {10050, 1}, {10075, 2.5}, {10100, 2.25}, {10150, 3}, {10200, 3.25}});

  apply_changes(book, {{buy, 100.20, 0},
                       {buy, 99.75, 0},
                       {buy, 98.00, 0},
                       {sell, 100.30, 0},
                       {sell, 100.75, 0},
                       {sell, 102.00, 0}});
  expect_book(book, {{10000, 1}, {9950, 8}, {9900, 3}}, {{10050, 1}, {10100, 2.25}, {10150, 3}});
  EXPECT_EQ(book.absent_removals(), 0U);
  EXPECT_EQ(book.rejected_changes(), 0U);
  EXPECT_TRUE(book.check_invariants());
}

TEST(order_book, removal_of_absent_level_is_counted_noop) {
  auto book = seeded_book();
  apply_changes(book, {{buy, 50.00, 0}, {sell, 200.00, 0}});
  expect_book(book, {{10000, 1}, {9950, 2}, {9900, 3}}, {{10050, 1}, {10100, 2}, {10150, 3}});
  EXPECT_EQ(book.absent_removals(), 2U);
  EXPECT_EQ(book.rejected_changes(), 0U);
}

TEST(order_book, duplicate_prices_in_one_update_apply_in_order) {
  auto book = seeded_book();
  // bid 99.00: 5 then 6 -> 6; ask 101.50: 0 then 4 -> 4; bid 98.50: 1 then 0 -> absent again.
  apply_changes(book, {{buy, 99.00, 5},
                       {buy, 99.00, 6},
                       {sell, 101.50, 0},
                       {sell, 101.50, 4},
                       {buy, 98.50, 1},
                       {buy, 98.50, 0}});
  expect_book(book, {{10000, 1}, {9950, 2}, {9900, 6}}, {{10050, 1}, {10100, 2}, {10150, 4}});
  EXPECT_EQ(book.absent_removals(), 0U);
}

TEST(order_book, snapshot_with_duplicate_price_keeps_last) {
  order_book book(0.01);
  apply_snapshot(book, {{50.00, 1}}, {{60.00, 2}, {60.00, 3}});
  expect_book(book, {{5000, 1}}, {{6000, 3}});
}

TEST(order_book, empty_snapshot_empties_the_book) {
  auto book = seeded_book();
  apply_snapshot(book, {}, {});
  expect_book(book, {}, {});
  EXPECT_FALSE(book.best_bid());
  EXPECT_FALSE(book.best_ask());
  EXPECT_FALSE(book.is_crossed());
  EXPECT_TRUE(book.check_invariants());
}

TEST(order_book, new_snapshot_replaces_state_after_updates) {
  auto book = seeded_book();
  add_levels(book);
  apply_changes(book, {{buy, 101.00, 1}});  // crossed
  ASSERT_TRUE(book.is_crossed());
  apply_snapshot(book, {{50.00, 1}}, {{60.00, 2}});
  expect_book(book, {{5000, 1}}, {{6000, 2}});
  EXPECT_FALSE(book.is_crossed());
  EXPECT_TRUE(book.check_invariants());
}

TEST(order_book, crossed_result_is_reported_not_thrown) {
  auto book = seeded_book();
  EXPECT_NO_THROW(apply_changes(book, {{buy, 101.00, 1}}));
  expect_book(book, {{10100, 1}, {10000, 1}, {9950, 2}, {9900, 3}},
              {{10050, 1}, {10100, 2}, {10150, 3}});
  EXPECT_TRUE(book.is_crossed());
  EXPECT_TRUE(book.check_structure());
  const auto verdict = book.check_invariants();
  ASSERT_FALSE(verdict);
  EXPECT_NE(verdict.error().find("crossed"), std::string::npos) << verdict.error();

  // Locked (bid == ask) counts as crossed. Removing the offending bid uncrosses it.
  apply_changes(book, {{buy, 101.00, 0}, {buy, 100.50, 1}});
  EXPECT_TRUE(book.is_crossed());
  apply_changes(book, {{buy, 100.50, 0}});
  EXPECT_FALSE(book.is_crossed());
  EXPECT_TRUE(book.check_invariants());
}

TEST(order_book, off_grid_prices_are_rejected_and_counted) {
  auto book = seeded_book();
  apply_changes(book, {{buy, 100.005, 1}, {sell, 100.0051, 1}, {buy, 97.00, 1}});
  expect_book(book, {{10000, 1}, {9950, 2}, {9900, 3}, {9700, 1}},
              {{10050, 1}, {10100, 2}, {10150, 3}});
  EXPECT_EQ(book.rejected_changes(), 2U);

  // A rejected removal must not touch an existing level or count as an absent removal.
  apply_changes(book, {{buy, 100.004, 0}});
  EXPECT_EQ(book.level_count(buy), 4U);
  EXPECT_EQ(book.rejected_changes(), 3U);
  EXPECT_EQ(book.absent_removals(), 0U);
}

TEST(order_book, invalid_values_are_rejected) {
  order_book book(0.01);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  apply_changes(book, {{buy, nan, 1}, {buy, -1.0, 1}, {buy, 1.0, -1}, {sell, 1.0, nan}});
  expect_book(book, {}, {});
  EXPECT_EQ(book.rejected_changes(), 4U);
}

TEST(order_book, snapshot_off_grid_entries_are_rejected_and_zero_sizes_skipped) {
  order_book book(0.01);
  apply_snapshot(book, {{100.00, 1}, {99.999, 2}, {99.00, 0}}, {{100.50, 1}});
  expect_book(book, {{10000, 1}}, {{10050, 1}});
  EXPECT_EQ(book.rejected_changes(), 1U);
}

TEST(order_book, grid_conversion_survives_division_error) {
  // 62691.57 / 0.01 is 6269157.000000001 in double arithmetic; it must land on tick 6269157.
  order_book book(0.01);
  apply_changes(book, {{buy, 62691.57, 1}, {buy, 0.07, 2}, {buy, 1774.43, 3}});
  expect_book(book, {{6269157, 1}, {177443, 3}, {7, 2}}, {});
  EXPECT_EQ(book.rejected_changes(), 0U);
}

TEST(order_book, check_invariants_accepts_empty_book) {
  const order_book book(0.01);
  EXPECT_TRUE(book.check_invariants());
}

// Replays tests/data/coinbase_sample.jsonl. That capture has no snapshot frames (they were
// removed, tests/data/README.md) and keeps only every 20th l2update, so every book starts empty
// and is missing most of its levels. Consequences, all visible below and in the reference output:
//  - most removals hit levels the book never saw (absent counts);
//  - BTC-USD and ETH-USD books end up crossed after some messages (stale asks/bids survive).
// Both are properties of the capture, not book bugs, so the test pins them instead of hiding them.
struct checkpoint {
  int ordinal;  // 1-based among the capture's l2update messages
  const char* product;
  book_level top_bid;
  book_level top_ask;
  std::size_t bid_count;
  std::size_t ask_count;
  std::uint64_t absent;
  bool crossed;
};

TEST(order_book, capture_replay_keeps_invariants) {
  // Expected values: `python3 scripts/book_reference.py` (dict books, Decimal ticks).
  const std::vector<checkpoint> checkpoints = {
      {1, "BTC-USD", {6269447, 0.31258369}, {6269561, 0.00638022}, 22, 16, 52, false},
      {10, "ETH-USD", {177450, 1.41}, {177455, 0.5}, 25, 16, 26, false},
      {20, "ADA-USD", {15076, 762.372969}, {15077, 1068.9}, 15, 16, 3, false},
      {30, "BTC-USD", {6269447, 0.31258369}, {6265085, 0.0161}, 129, 77, 222, true},
      {41, "ADA-USD", {15076, 762.372969}, {15077, 1068.9}, 22, 19, 4, false},
  };
  const std::map<std::string, double> tick_sizes = {
      {"BTC-USD", 0.01}, {"ETH-USD", 0.01}, {"ADA-USD", 0.00001}};

  std::map<std::string, order_book> books;
  for (const auto& [product, tick] : tick_sizes) {
    books.emplace(product, order_book(tick));
  }
  std::map<std::string, int> crossed_after;
  int ordinal = 0;
  std::size_t next_checkpoint = 0;

  cratchet::capture_reader capture(std::string(CRATCHET_TEST_DATA_DIR) + "/coinbase_sample.jsonl");
  cratchet::coinbase::parser parser;
  while (const auto captured = capture.next()) {
    auto parsed = parser.parse(captured->msg);
    ASSERT_TRUE(parsed) << captured->msg;
    std::string product;
    if (const auto* snap = std::get_if<snapshot>(&parsed.value())) {
      product = snap->product_id;
      books.at(product).apply(*snap);
    } else if (const auto* update = std::get_if<l2update>(&parsed.value())) {
      product = update->product_id;
      books.at(product).apply(*update);
      ++ordinal;
    } else {
      continue;
    }

    const auto& book = books.at(product);
    ASSERT_TRUE(book.check_structure()) << product << ": " << book.check_structure().error();
    EXPECT_EQ(book.is_crossed(), !book.check_invariants());
    if (book.is_crossed()) {
      ++crossed_after[product];
    }

    if (next_checkpoint < checkpoints.size() && checkpoints[next_checkpoint].ordinal == ordinal) {
      const auto& expected = checkpoints[next_checkpoint++];
      ASSERT_EQ(product, expected.product) << "ordinal " << ordinal;
      EXPECT_EQ(book.best_bid()->price_ticks, expected.top_bid.price_ticks) << ordinal;
      EXPECT_DOUBLE_EQ(book.best_bid()->size, expected.top_bid.size) << ordinal;
      EXPECT_EQ(book.best_ask()->price_ticks, expected.top_ask.price_ticks) << ordinal;
      EXPECT_DOUBLE_EQ(book.best_ask()->size, expected.top_ask.size) << ordinal;
      EXPECT_EQ(book.level_count(buy), expected.bid_count) << ordinal;
      EXPECT_EQ(book.level_count(sell), expected.ask_count) << ordinal;
      EXPECT_EQ(book.absent_removals(), expected.absent) << ordinal;
      EXPECT_EQ(book.is_crossed(), expected.crossed) << ordinal;
    }
  }

  EXPECT_EQ(ordinal, 41);
  EXPECT_EQ(next_checkpoint, checkpoints.size());
  // Messages after which each book was crossed, and final state, from the reference script.
  EXPECT_EQ(crossed_after["BTC-USD"], 14);
  EXPECT_EQ(crossed_after["ETH-USD"], 8);
  EXPECT_EQ(crossed_after["ADA-USD"], 0);
  EXPECT_EQ(books.at("BTC-USD").level_count(buy), 181U);
  EXPECT_EQ(books.at("BTC-USD").level_count(sell), 113U);
  EXPECT_EQ(books.at("BTC-USD").absent_removals(), 330U);
  EXPECT_EQ(books.at("ETH-USD").level_count(buy), 102U);
  EXPECT_EQ(books.at("ETH-USD").level_count(sell), 65U);
  EXPECT_EQ(books.at("ETH-USD").absent_removals(), 81U);
  EXPECT_EQ(books.at("ADA-USD").level_count(buy), 22U);
  EXPECT_EQ(books.at("ADA-USD").level_count(sell), 19U);
  EXPECT_EQ(books.at("ADA-USD").absent_removals(), 4U);
  for (const auto& [product, book] : books) {
    EXPECT_EQ(book.rejected_changes(), 0U) << product << ": off-grid prices at the chosen tick";
  }
}

}  // namespace
