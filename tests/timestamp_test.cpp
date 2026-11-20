#include "cratchet/timestamp.hpp"

#include <gtest/gtest.h>

#include <optional>

namespace {

using cratchet::parse_rfc3339_ns;

// Expected values computed exactly in Python with calendar.timegm (integer arithmetic).
TEST(timestamp, parses_coinbase_microsecond_timestamps_exactly) {
  EXPECT_EQ(parse_rfc3339_ns("2026-11-19T01:53:27.000000Z"), 1795053207000000000);
  EXPECT_EQ(parse_rfc3339_ns("2026-11-19T01:53:26.618406Z"), 1795053206618406000);
  EXPECT_EQ(parse_rfc3339_ns("2024-02-29T23:59:59.999999Z"), 1709251199999999000);
}

TEST(timestamp, epoch_and_fraction_lengths) {
  EXPECT_EQ(parse_rfc3339_ns("1970-01-01T00:00:00Z"), 0);
  EXPECT_EQ(parse_rfc3339_ns("1970-01-01T00:00:00.5Z"), 500'000'000);
  EXPECT_EQ(parse_rfc3339_ns("1970-01-01T00:00:00.123456789Z"), 123'456'789);
  EXPECT_EQ(parse_rfc3339_ns("1969-12-31T23:59:59.000000001Z"), -999'999'999);
}

TEST(timestamp, applies_utc_offsets) {
  EXPECT_EQ(parse_rfc3339_ns("1970-01-01T01:00:00+01:00"), 0);
  EXPECT_EQ(parse_rfc3339_ns("1969-12-31T19:30:00-04:30"), 0);
  EXPECT_EQ(parse_rfc3339_ns("1970-01-01T00:00:00+00:00"), 0);
}

TEST(timestamp, rejects_malformed_input) {
  for (const char* bad : {
           "", "2026-11-19",
           "2026-11-19T01:53:27",              // no zone
           "2026-11-19 01:53:27Z",             // space separator
           "2026-11-19T01:53:27.Z",            // empty fraction
           "2026-11-19T01:53:27.1234567890Z",  // more than 9 digits
           "2026-11-19T01:53:27ZZ",            // trailing junk
           "2026-13-01T00:00:00Z",             // month
           "2026-02-29T00:00:00Z",             // not a leap year
           "2026-11-19T24:00:00Z",             // hour
           "2026-11-19T23:60:00Z",             // minute
           "2026-11-19T23:59:60Z",             // leap second
           "2026-11-19T01:53:27+0100",         // offset without colon
           "2026-11-19T01:53:27+24:00",        // offset hour
           "2026-1x-19T01:53:27Z",
           "9999-12-31T23:59:59Z",  // overflows int64 nanoseconds
       }) {
    EXPECT_EQ(parse_rfc3339_ns(bad), std::nullopt) << bad;
  }
}

}  // namespace
