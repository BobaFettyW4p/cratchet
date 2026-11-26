// libFuzzer target for the Coinbase message parser (PLAN.md Stage 3).
//
// Feeds arbitrary bytes to coinbase::parser. On success it reads every field of the returned
// message, including each bids/asks/changes entry, so ASan can flag out-of-bounds or dangling
// string_view/span reads. Tickers are also run through the normalizer.
//
// parser::parse() copies the frame into its own padded buffer, so no padding is needed here.
// The parser and normalizer are reused across calls, as in the real pipeline.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "cratchet/coinbase/parser.hpp"
#include "cratchet/normalizer.hpp"

namespace {

using namespace cratchet;

// Every value read is folded into this, so the reads cannot be optimized away.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::uint64_t sink = 0;

std::uint64_t touch(std::string_view text) {
  std::uint64_t acc = text.size();
  for (const char c : text) {
    acc += static_cast<unsigned char>(c);
  }
  return acc;
}

std::uint64_t touch(double value) { return std::bit_cast<std::uint64_t>(value); }

std::uint64_t touch(std::int64_t value) { return static_cast<std::uint64_t>(value); }

template <typename T>
std::uint64_t touch(const std::optional<T>& value) {
  return value ? 1 + touch(*value) : 0;
}

std::uint64_t touch(const coinbase::ticker& msg) {
  return touch(msg.product_id) + touch(msg.sequence) + touch(msg.time_ns) + touch(msg.price) +
         touch(msg.last_size) + touch(msg.best_bid) + touch(msg.best_bid_size) +
         touch(msg.best_ask) + touch(msg.best_ask_size);
}

std::uint64_t touch(const coinbase::heartbeat& msg) {
  return touch(msg.product_id) + touch(msg.sequence) + touch(msg.last_trade_id) +
         touch(msg.time_ns);
}

std::uint64_t touch(const coinbase::subscriptions& /*msg*/) { return 0; }

std::uint64_t touch(const coinbase::error_message& msg) {
  return touch(msg.message) + touch(msg.reason);
}

std::uint64_t touch(const coinbase::snapshot& msg) {
  std::uint64_t acc = touch(msg.product_id) + touch(msg.time_ns);
  for (const auto& level : msg.bids) {
    acc += touch(level.price) + touch(level.size);
  }
  for (const auto& level : msg.asks) {
    acc += touch(level.price) + touch(level.size);
  }
  return acc;
}

std::uint64_t touch(const coinbase::l2update& msg) {
  std::uint64_t acc = touch(msg.product_id) + touch(msg.time_ns);
  for (const auto& change : msg.changes) {
    acc += static_cast<std::uint64_t>(change.side) + touch(change.price) + touch(change.size);
  }
  return acc;
}

std::uint64_t touch(const coinbase::unknown& msg) { return touch(msg.type); }

}  // namespace

// The entry point name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  static coinbase::parser parser;
  static normalizer norm(8);

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): libFuzzer hands over bytes.
  const std::string_view frame(reinterpret_cast<const char*>(data), size);
  const auto parsed = parser.parse(frame);
  if (!parsed) {
    sink = sink + touch(parsed.error().field) + static_cast<std::uint64_t>(parsed.error().code);
    return 0;
  }

  const auto& msg = parsed.value();
  sink = sink + std::visit([](const auto& alt) { return touch(alt); }, msg);

  if (const auto* tick_msg = std::get_if<coinbase::ticker>(&msg)) {
    const auto normalized = norm.normalize(*tick_msg, tick_msg->time_ns);
    sink = sink + (normalized ? touch(normalized.value().json) : 0);
  }
  return 0;
}
