#include "cratchet/json.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <string_view>

namespace {

std::string format(double value) {
  std::string out;
  cratchet::json::append_double(out, value);
  return out;
}

// Each expected string is what pydantic 2's model_dump_json() printed for the same float.
TEST(json, doubles_match_pydantic) {
  EXPECT_EQ(format(43000.0), "43000.0");
  EXPECT_EQ(format(0.15079), "0.15079");
  EXPECT_EQ(format(2.5), "2.5");
  EXPECT_EQ(format(0.0), "0.0");
  EXPECT_EQ(format(-0.0), "-0.0");
  EXPECT_EQ(format(0.0001), "0.0001");
  EXPECT_EQ(format(1e-5), "0.00001");
  EXPECT_EQ(format(0.000012345), "0.000012345");
  EXPECT_EQ(format(9.999e-5), "0.00009999");
  EXPECT_EQ(format(1e-6), "1e-6");
  EXPECT_EQ(format(1e-7), "1e-7");
  EXPECT_EQ(format(1e-8), "1e-8");
  EXPECT_EQ(format(1.23456e-7), "1.23456e-7");
  EXPECT_EQ(format(1e15), "1000000000000000.0");
  EXPECT_EQ(format(1234567890123456.0), "1234567890123456.0");
  EXPECT_EQ(format(1e16), "1e16");
  EXPECT_EQ(format(1.5e16), "1.5e16");
  EXPECT_EQ(format(12345678901234567.0), "1.2345678901234568e16");
  EXPECT_EQ(format(1e21), "1e21");
  EXPECT_EQ(format(5e-324), "5e-324");
  EXPECT_EQ(format(1.7976931348623157e308), "1.7976931348623157e308");
  EXPECT_EQ(format(-1774.63), "-1774.63");
}

TEST(json, non_finite_doubles_are_null) {
  EXPECT_EQ(format(std::numeric_limits<double>::infinity()), "null");
  EXPECT_EQ(format(std::numeric_limits<double>::quiet_NaN()), "null");
}

TEST(json, ints) {
  std::string out;
  cratchet::json::append_int(out, 1795053206618406000);
  out += ',';
  cratchet::json::append_int(out, -5);
  EXPECT_EQ(out, "1795053206618406000,-5");
}

TEST(json, strings_are_escaped_like_serde_json) {
  std::string out;
  cratchet::json::append_string(out, std::string_view("a\"b\\c\n\t\x01\x1f\x7f", 10));
  EXPECT_EQ(out, R"("a\"b\\c\n\t\u0001\u001f)"
                 "\x7f\"");
}

}  // namespace
