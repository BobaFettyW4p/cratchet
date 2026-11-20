#include "cratchet/shard.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string hex(const cratchet::sha256_digest& digest) {
  std::string out;
  for (const auto byte : digest) {
    std::array<char, 3> buf{};
    std::snprintf(buf.data(), buf.size(), "%02x", byte);
    out += buf.data();
  }
  return out;
}

TEST(shard, sha256_known_vectors) {
  EXPECT_EQ(hex(cratchet::sha256("")),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hex(cratchet::sha256("BTC-USD")),
            "2fd1a09ed73e366561d195e4ae8fa215eca2085e508ecca8411af1c61dccc815");
}

// Reference vectors from docs/contract.md §3.
TEST(shard, matches_ledgerflux_for_four_shards) {
  EXPECT_EQ(cratchet::shard_for("BTC-USD", 4), 1U);
  EXPECT_EQ(cratchet::shard_for("ETH-USD", 4), 0U);
  EXPECT_EQ(cratchet::shard_for("ADA-USD", 4), 0U);
  EXPECT_EQ(cratchet::shard_for("SOL-USD", 4), 3U);
}

// num_shards that are not powers of two exercise the full big-endian reduction.
// Expected values from int(sha256(p).hexdigest(), 16) % n in Python.
TEST(shard, matches_ledgerflux_for_other_shard_counts) {
  struct vector {
    std::string_view product;
    std::uint32_t n;
    std::uint32_t shard;
  };
  for (const auto& [product, n, shard] : {
           vector{"BTC-USD", 1, 0},
           vector{"BTC-USD", 3, 1},
           vector{"BTC-USD", 7, 6},
           vector{"BTC-USD", 10, 7},
           vector{"BTC-USD", 1000, 157},
           vector{"ETH-USD", 3, 2},
           vector{"ETH-USD", 7, 0},
           vector{"ETH-USD", 1000, 532},
           vector{"ADA-USD", 7, 3},
           vector{"ADA-USD", 1000, 496},
           vector{"SOL-USD", 7, 1},
           vector{"SOL-USD", 1000, 539},
       }) {
    EXPECT_EQ(cratchet::shard_for(product, n), shard) << product << " n=" << n;
  }
}

TEST(shard, zero_shards_is_rejected) {
  EXPECT_THROW((void)cratchet::shard_for("BTC-USD", 0), std::invalid_argument);
}

}  // namespace
