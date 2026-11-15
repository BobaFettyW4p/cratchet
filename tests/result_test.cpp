#include "cratchet/result.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>

namespace {

using int_result = cratchet::result<int, std::string>;

TEST(result, holds_a_value) {
  auto r = int_result::ok(42);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(static_cast<bool>(r));
  EXPECT_EQ(r.value(), 42);
}

TEST(result, holds_an_error) {
  auto r = int_result::fail("boom");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), "boom");
}

TEST(result, wrong_alternative_throws) {
  EXPECT_THROW((void)int_result::fail("x").value(), std::bad_variant_access);
  EXPECT_THROW((void)int_result::ok(1).error(), std::bad_variant_access);
}

TEST(result, value_or_falls_back_on_error) {
  EXPECT_EQ(int_result::ok(7).value_or(0), 7);
  EXPECT_EQ(int_result::fail("x").value_or(-1), -1);
}

TEST(result, value_and_error_may_share_a_type) {
  using same = cratchet::result<int, int>;
  EXPECT_TRUE(same::ok(1).has_value());
  EXPECT_FALSE(same::fail(1).has_value());
}

}  // namespace
