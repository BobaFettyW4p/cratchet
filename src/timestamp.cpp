#include "cratchet/timestamp.hpp"

#include <chrono>
#include <cstddef>
#include <limits>

namespace cratchet {
namespace {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Reads exactly `count` decimal digits starting at `pos`. Requires pos <= text.size().
bool read_digits(std::string_view text, std::size_t& pos, std::size_t count, int& out) noexcept {
  if (count > text.size() - pos) {
    return false;
  }
  int value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char c = text[pos + i];
    if (!is_digit(c)) {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  pos += count;
  out = value;
  return true;
}

bool expect(std::string_view text, std::size_t& pos, char c) noexcept {
  if (pos >= text.size() || text[pos] != c) {
    return false;
  }
  ++pos;
  return true;
}

}  // namespace

std::optional<std::int64_t> parse_rfc3339_ns(std::string_view text) noexcept {
  std::size_t pos = 0;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!read_digits(text, pos, 4, year) || !expect(text, pos, '-') ||
      !read_digits(text, pos, 2, month) || !expect(text, pos, '-') ||
      !read_digits(text, pos, 2, day) || !expect(text, pos, 'T') ||
      !read_digits(text, pos, 2, hour) || !expect(text, pos, ':') ||
      !read_digits(text, pos, 2, minute) || !expect(text, pos, ':') ||
      !read_digits(text, pos, 2, second)) {
    return std::nullopt;
  }
  // Leap seconds (":60") are rejected, as Python's datetime does.
  if (hour > 23 || minute > 59 || second > 59) {
    return std::nullopt;
  }

  std::int64_t fraction_ns = 0;
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    int digits = 0;
    while (pos < text.size() && is_digit(text[pos])) {
      if (digits == 9) {
        return std::nullopt;  // sub-nanosecond precision would be silently dropped
      }
      fraction_ns = fraction_ns * 10 + (text[pos] - '0');
      ++digits;
      ++pos;
    }
    if (digits == 0) {
      return std::nullopt;
    }
    for (; digits < 9; ++digits) {
      fraction_ns *= 10;
    }
  }

  int offset_seconds = 0;
  if (pos < text.size() && text[pos] == 'Z') {
    ++pos;
  } else if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
    const int sign = text[pos] == '-' ? -1 : 1;
    ++pos;
    int offset_hours = 0;
    int offset_minutes = 0;
    if (!read_digits(text, pos, 2, offset_hours) || !expect(text, pos, ':') ||
        !read_digits(text, pos, 2, offset_minutes) || offset_hours > 23 || offset_minutes > 59) {
      return std::nullopt;
    }
    offset_seconds = sign * (offset_hours * 3600 + offset_minutes * 60);
  } else {
    return std::nullopt;
  }
  if (pos != text.size()) {
    return std::nullopt;
  }

  const std::chrono::year_month_day date{std::chrono::year{year},
                                         std::chrono::month{static_cast<unsigned>(month)},
                                         std::chrono::day{static_cast<unsigned>(day)}};
  if (!date.ok()) {
    return std::nullopt;
  }
  const std::int64_t days = std::chrono::sys_days{date}.time_since_epoch().count();
  const std::int64_t seconds = days * 86'400 + std::int64_t{hour} * 3'600 +
                               std::int64_t{minute} * 60 + second - offset_seconds;

  constexpr std::int64_t ns_per_second = 1'000'000'000;
  constexpr std::int64_t max_seconds = std::numeric_limits<std::int64_t>::max() / ns_per_second;
  if (seconds >= max_seconds || seconds <= -max_seconds) {
    return std::nullopt;
  }
  return seconds * ns_per_second + fraction_ns;
}

}  // namespace cratchet
