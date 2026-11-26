#include "cratchet/order_book.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>

namespace cratchet {

namespace {

// Largest tick count a double represents exactly.
constexpr double max_exact_ticks = 9007199254740992.0;  // 2^53
constexpr double grid_tolerance_ticks = 1e-6;

bool bid_before(const book_level& level, std::int64_t ticks) { return level.price_ticks > ticks; }
bool ask_before(const book_level& level, std::int64_t ticks) { return level.price_ticks < ticks; }

// First level not ordered before `ticks`: the level itself if present, else the insert position.
std::vector<book_level>::iterator find_pos(std::vector<book_level>& levels, bool is_bid,
                                           std::int64_t ticks) {
  return std::lower_bound(levels.begin(), levels.end(), ticks, is_bid ? bid_before : ask_before);
}

std::string describe(const char* side, std::size_t index, const book_level& level,
                     const char* what) {
  return std::string(side) + "[" + std::to_string(index) +
         "] price_ticks=" + std::to_string(level.price_ticks) +
         " size=" + std::to_string(level.size) + ": " + what;
}

result<std::monostate, std::string> check_side(const std::vector<book_level>& levels, bool is_bid) {
  const char* name = is_bid ? "bids" : "asks";
  for (std::size_t i = 0; i < levels.size(); ++i) {
    if (!(levels[i].size > 0.0)) {
      return result<std::monostate, std::string>::fail(
          describe(name, i, levels[i], "size is not > 0"));
    }
    if (i > 0) {
      const bool ordered = is_bid ? levels[i - 1].price_ticks > levels[i].price_ticks
                                  : levels[i - 1].price_ticks < levels[i].price_ticks;
      if (!ordered) {
        return result<std::monostate, std::string>::fail(
            describe(name, i, levels[i], "not strictly sorted (or duplicate price)"));
      }
    }
  }
  return result<std::monostate, std::string>::ok({});
}

}  // namespace

order_book::order_book(double tick_size) : tick_size_(tick_size) {
  if (!std::isfinite(tick_size) || tick_size <= 0.0) {
    throw std::invalid_argument("order_book: tick_size must be finite and > 0");
  }
}

std::optional<std::int64_t> order_book::to_ticks(double price) const {
  const double scaled = price / tick_size_;
  if (!std::isfinite(scaled) || scaled < 0.0 || scaled > max_exact_ticks) {
    return std::nullopt;
  }
  const auto ticks = std::llround(scaled);
  if (std::fabs(scaled - static_cast<double>(ticks)) > grid_tolerance_ticks) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(ticks);
}

void order_book::set_level(coinbase::side side, std::int64_t ticks, double size) {
  const bool is_bid = side == coinbase::side::buy;
  auto& levels = is_bid ? bids_ : asks_;
  const auto pos = find_pos(levels, is_bid, ticks);
  const bool present = pos != levels.end() && pos->price_ticks == ticks;
  if (size > 0.0) {
    if (present) {
      pos->size = size;
    } else {
      levels.insert(pos, book_level{ticks, size});
    }
  } else if (present) {
    levels.erase(pos);
  } else {
    ++absent_removals_;
  }
}

void order_book::apply(const coinbase::snapshot& snap) {
  const auto load = [this](std::span<const coinbase::price_level> in, bool is_bid,
                           std::vector<book_level>& out) {
    struct entry {
      book_level level;
      std::size_t seq;
    };
    std::vector<entry> entries;
    entries.reserve(in.size());
    for (const auto& item : in) {
      const auto ticks = to_ticks(item.price);
      if (!ticks || !std::isfinite(item.size) || item.size < 0.0) {
        ++rejected_changes_;
        continue;
      }
      if (item.size > 0.0) {
        entries.push_back({{*ticks, item.size}, entries.size()});
      }
    }
    // Sort by price (side order), then input order, so the last duplicate of a price wins.
    std::sort(entries.begin(), entries.end(), [is_bid](const entry& a, const entry& b) {
      if (a.level.price_ticks != b.level.price_ticks) {
        return is_bid ? a.level.price_ticks > b.level.price_ticks
                      : a.level.price_ticks < b.level.price_ticks;
      }
      return a.seq < b.seq;
    });
    out.clear();
    out.reserve(entries.size());
    for (const auto& e : entries) {
      if (!out.empty() && out.back().price_ticks == e.level.price_ticks) {
        out.back() = e.level;
      } else {
        out.push_back(e.level);
      }
    }
  };
  load(snap.bids, true, bids_);
  load(snap.asks, false, asks_);
}

void order_book::apply(const coinbase::l2update& update) {
  for (const auto& change : update.changes) {
    const auto ticks = to_ticks(change.price);
    if (!ticks || !std::isfinite(change.size) || change.size < 0.0) {
      ++rejected_changes_;
      continue;
    }
    set_level(change.side, *ticks, change.size);
  }
}

std::optional<book_level> order_book::best_bid() const {
  return bids_.empty() ? std::nullopt : std::optional<book_level>(bids_.front());
}

std::optional<book_level> order_book::best_ask() const {
  return asks_.empty() ? std::nullopt : std::optional<book_level>(asks_.front());
}

std::vector<book_level> order_book::depth(coinbase::side side, std::size_t n) const {
  const auto& levels = side == coinbase::side::buy ? bids_ : asks_;
  const auto count = std::min(n, levels.size());
  return {levels.begin(), levels.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::size_t order_book::level_count(coinbase::side side) const noexcept {
  return (side == coinbase::side::buy ? bids_ : asks_).size();
}

bool order_book::is_crossed() const {
  return !bids_.empty() && !asks_.empty() && bids_.front().price_ticks >= asks_.front().price_ticks;
}

result<std::monostate, std::string> order_book::check_structure() const {
  if (auto bids = check_side(bids_, true); !bids) {
    return bids;
  }
  return check_side(asks_, false);
}

result<std::monostate, std::string> order_book::check_invariants() const {
  if (auto structure = check_structure(); !structure) {
    return structure;
  }
  if (is_crossed()) {
    return result<std::monostate, std::string>::fail(
        "crossed: best_bid_ticks=" + std::to_string(bids_.front().price_ticks) +
        " >= best_ask_ticks=" + std::to_string(asks_.front().price_ticks));
  }
  return result<std::monostate, std::string>::ok({});
}

}  // namespace cratchet
