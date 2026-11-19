#include <gtest/gtest.h>

#include "cratchet/version.hpp"

TEST(hello, version_is_reported) {
  EXPECT_FALSE(cratchet::version().empty());
  EXPECT_EQ(cratchet::version(), "0.1.0");
}
