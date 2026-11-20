#include "cratchet/json.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <system_error>

namespace cratchet::json {

void append_double(std::string& out, double value) {
  if (!std::isfinite(value)) {
    out += "null";
    return;
  }

  // std::to_chars in scientific form gives the shortest round-trip digits, as ryu does:
  // "[-]d[.ddd]e(+|-)XX". Pull out the digits and the exponent, then lay them out the way
  // ryu's pretty formatter does.
  std::array<char, 32> sci{};
  const auto [sci_end, ec] =
      std::to_chars(sci.data(), sci.data() + sci.size(), value, std::chars_format::scientific);
  if (ec != std::errc{}) {
    out += "null";  // unreachable: 32 bytes holds any double
    return;
  }
  std::string_view text(sci.data(), static_cast<std::size_t>(sci_end - sci.data()));

  if (text.front() == '-') {
    out += '-';
    text.remove_prefix(1);
  }
  const std::size_t e_pos = text.find('e');
  const std::string_view mantissa = text.substr(0, e_pos);
  std::string_view exponent_text = text.substr(e_pos + 1);
  const bool negative_exponent = exponent_text.front() == '-';
  exponent_text.remove_prefix(1);  // drop the sign; std::from_chars rejects '+'
  int exponent = 0;
  std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent);
  if (negative_exponent) {
    exponent = -exponent;
  }

  std::array<char, 20> digit_buf{};
  std::size_t length = 0;
  for (const char c : mantissa) {
    if (c != '.') {
      digit_buf.at(length++) = c;
    }
  }
  const std::string_view digits(digit_buf.data(), length);

  // value = 0.<digits> * 10^point, i.e. the decimal point sits `point` places into the digits.
  const int point = exponent + 1;
  const int n = static_cast<int>(length);
  const auto upoint = static_cast<std::size_t>(point);

  if (point >= n && point <= 16) {
    out += digits;
    out.append(upoint - length, '0');
    out += ".0";
  } else if (point > 0 && point <= 16) {
    out += digits.substr(0, upoint);
    out += '.';
    out += digits.substr(upoint);
  } else if (point > -5 && point <= 0) {
    out += "0.";
    out.append(static_cast<std::size_t>(-point), '0');
    out += digits;
  } else {
    out += digits.front();
    if (length > 1) {
      out += '.';
      out += digits.substr(1);
    }
    out += 'e';
    append_int(out, exponent);
  }
}

void append_int(std::string& out, std::int64_t value) {
  std::array<char, 24> buf{};
  const auto [end, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  out.append(buf.data(), end);
}

void append_string(std::string& out, std::string_view value) {
  constexpr std::string_view hex = "0123456789abcdef";
  out += '"';
  for (const char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          const auto byte = static_cast<unsigned char>(c);
          out += "\\u00";
          out += hex[byte >> 4U];
          out += hex[byte & 0x0FU];
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

}  // namespace cratchet::json
