#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "cratchet/coinbase/messages.hpp"
#include "cratchet/result.hpp"

namespace cratchet {

// One price level. The price is an integer tick count (price / tick_size), never a double.
struct book_level {
  std::int64_t price_ticks{};
  double size{};

  friend bool operator==(const book_level&, const book_level&) = default;
};

// Limit order book for one product, built from level2 snapshots and incremental updates.
//
// Layout: one flat, sorted std::vector per side (bids descending, asks ascending), so the best
// level is at index 0 and lookups are binary searches over contiguous memory. Inserting or
// erasing in the middle shifts the tail; for the few dozen levels that move per update this beats
// node-based containers.
//
// Prices: every incoming double price is converted with llround(price / tick_size). A price that
// is not within a small tolerance (1e-6 ticks) of a whole tick count, or is non-finite, negative
// or too large for exact conversion, is rejected and counted (rejected_changes()), never rounded.
// Sizes stay double. A negative or non-finite size is rejected the same way.
//
// Invariants (check_invariants()):
//   1. Each side is strictly sorted (bids descending, asks ascending), so no duplicate price.
//   2. Every stored size is > 0.
//   3. best_bid < best_ask whenever both sides are non-empty.
//
// 1 and 2 are maintained by apply() and cannot be broken by any input. 3 cannot be: a crossed
// book cannot be detected from a missed update, only observed (docs/contract.md section 9), and
// the level2 messages carry no sequence number. So apply() never throws or aborts on a crossed
// result; it applies the change and is_crossed() / check_invariants() report it.
class order_book {
 public:
  // Throws std::invalid_argument unless tick_size is finite and > 0.
  explicit order_book(double tick_size);

  // Replaces the whole book. Entries with size 0 are skipped; a price listed twice keeps the last
  // size; off-grid or invalid entries are rejected and counted. Counters are not reset.
  void apply(const coinbase::snapshot& snap);

  // Applies each change in order: size > 0 sets the level, size == 0 erases it. Removing an
  // absent level is a no-op counted in absent_removals().
  void apply(const coinbase::l2update& update);

  [[nodiscard]] std::optional<book_level> best_bid() const;
  [[nodiscard]] std::optional<book_level> best_ask() const;

  // The top `n` levels of a side, best first (fewer if the side has fewer).
  [[nodiscard]] std::vector<book_level> depth(coinbase::side side, std::size_t n) const;
  [[nodiscard]] std::size_t level_count(coinbase::side side) const noexcept;

  // True if both sides are non-empty and best_bid >= best_ask.
  [[nodiscard]] bool is_crossed() const;

  // Checks invariants 1 and 2 only.
  [[nodiscard]] result<std::monostate, std::string> check_structure() const;
  // Checks invariants 1 to 3; the error is a human-readable reason.
  [[nodiscard]] result<std::monostate, std::string> check_invariants() const;

  [[nodiscard]] double tick_size() const noexcept { return tick_size_; }
  [[nodiscard]] double price_of(std::int64_t ticks) const noexcept {
    return static_cast<double>(ticks) * tick_size_;
  }

  // Changes (or snapshot entries) dropped because the price was off the tick grid or a value was
  // invalid.
  [[nodiscard]] std::uint64_t rejected_changes() const noexcept { return rejected_changes_; }
  // Removals of a level that was not in the book.
  [[nodiscard]] std::uint64_t absent_removals() const noexcept { return absent_removals_; }

 private:
  [[nodiscard]] std::optional<std::int64_t> to_ticks(double price) const;
  void set_level(coinbase::side side, std::int64_t ticks, double size);

  double tick_size_;
  std::vector<book_level> bids_;  // descending by price_ticks
  std::vector<book_level> asks_;  // ascending by price_ticks
  std::uint64_t rejected_changes_{0};
  std::uint64_t absent_removals_{0};
};

}  // namespace cratchet
