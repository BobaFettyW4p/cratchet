#pragma once

#include <simdjson.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cratchet/coinbase/messages.hpp"
#include "cratchet/result.hpp"

namespace cratchet::coinbase {

enum class parse_error_code : std::uint8_t {
  invalid_json,   // not well-formed JSON
  not_an_object,  // top-level value is not an object
  missing_type,   // no string "type" field
  missing_field,  // a required field is absent
  wrong_type,     // a field has the wrong JSON type
  bad_number,     // a decimal string or integer did not parse, or is not finite
  bad_timestamp,  // a "time" field is not RFC 3339
  bad_level,      // a malformed bids/asks/changes entry
};

std::string_view to_string(parse_error_code code) noexcept;

struct parse_error {
  parse_error_code code{};
  std::string_view field;  // the offending field, if any; valid until the next parse()
};

// Parses ws-feed frames into typed messages with simdjson On-Demand. One parser per thread.
//
// Only the fields a message type needs are decoded, in one pass over the object, in whatever
// order they appear. Fields that are skipped are not validated, so a frame with malformed JSON
// in an ignored field can still parse.
//
// Returned messages borrow from the parser: string_views and spans are valid until the next
// call to parse(). Buffers are reused, so steady-state parsing does not allocate.
class parser {
 public:
  result<message, parse_error> parse(std::string_view frame);

 private:
  simdjson::ondemand::parser json_;
  std::string buffer_;  // frame copy with the trailing padding simdjson requires
  std::vector<price_level> bids_;
  std::vector<price_level> asks_;
  std::vector<level_change> changes_;
};

}  // namespace cratchet::coinbase
