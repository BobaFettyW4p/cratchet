#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Minimal JSON writers whose output matches what LedgerFlux's pydantic models emit, so that
// Cratchet's messages can be diffed byte for byte against the Python services.
namespace cratchet::json {

// Appends `value` as pydantic's model_dump_json() does (Rust ryu "pretty" formatting):
// shortest round-trip digits, with a ".0" on integral values ("43000.0"), plain notation for
// decimal exponents -5 < e <= 16 ("0.00001", "1000000000000000.0") and scientific notation
// without padding or "+" otherwise ("1e-7", "1.5e16"). Non-finite values are written as null,
// pydantic's default.
void append_double(std::string& out, double value);

void append_int(std::string& out, std::int64_t value);

// Appends a quoted string, escaped as serde_json (and therefore pydantic) escapes it.
void append_string(std::string& out, std::string_view value);

}  // namespace cratchet::json
