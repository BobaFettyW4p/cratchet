#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cratchet {

// Parses an RFC 3339 timestamp such as "2026-11-18T01:02:03.123456Z" to integer nanoseconds
// since the Unix epoch. Accepts 0-9 fractional digits and a "Z" or "+HH:MM"/"-HH:MM" offset.
// The conversion is exact (no floating point), unlike LedgerFlux's
// datetime.timestamp() * 1e9; see docs/contract.md §4. Returns nullopt on malformed input or
// if the result does not fit in int64 nanoseconds (years outside roughly 1678-2261).
std::optional<std::int64_t> parse_rfc3339_ns(std::string_view text) noexcept;

}  // namespace cratchet
