#include "cratchet/shard.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
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

// Inputs of 55, 56, 63, 64 and 65 bytes straddle the SHA-256 padding boundaries (the length field
// no longer fits in the last block at 56, and 64 fills a block exactly).
// Expected values from hashlib.sha256(b'a' * n).hexdigest() in Python.
TEST(shard, sha256_padding_boundaries) {
  struct vector {
    std::size_t length;
    std::string_view digest;
  };
  for (const auto& [length, digest] : {
           vector{55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
           vector{56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
           vector{63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
           vector{64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
           vector{65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
       }) {
    EXPECT_EQ(hex(cratchet::sha256(std::string(length, 'a'))), digest) << "length=" << length;
  }
}

// Expected values from hashlib.sha256(s.encode('utf-8')).hexdigest() in Python.
TEST(shard, sha256_non_ascii_utf8) {
  struct vector {
    std::string_view input;
    std::string_view digest;
  };
  for (const auto& [input, digest] : {
           vector{"é", "4a99557e4033c3539de2eb65472017cad5f9557f7a0625a09f1c3f6e2ba69c4c"},
           vector{"BTC-€", "5f4ecfec9644b2cf14750808c81dc3306cfb29a972d37fd34e5a25cbce60d1d4"},
           vector{"日本-USD", "fbfa7323223c3fb1a590e8048c736defdc78cbb2f16003b6a3ff427c48af293c"},
       }) {
    EXPECT_EQ(hex(cratchet::sha256(input)), digest) << input;
  }
}

// Expected values from hashlib.sha256(b'BTC\0USD').hexdigest() and
// hashlib.sha256(b'BTC').hexdigest().
TEST(shard, sha256_embedded_nul_is_hashed) {
  const std::string_view with_nul("BTC\0USD", 7);
  EXPECT_EQ(hex(cratchet::sha256(with_nul)),
            "ce4ae5bf6fa0c1433954c4c21fa6570fba7732d7f96c3f344dd220d566848b89");
  EXPECT_EQ(hex(cratchet::sha256("BTC")),
            "da8562e7abc01a6f0d49a25d144ce6a9d7752a079c5d950ad5a93fd6d623f7fd");
}

// Expected values from int(hashlib.sha256(s.encode()).hexdigest(), 16) % n in Python.
TEST(shard, shard_for_non_ascii_and_embedded_nul) {
  struct vector {
    std::string_view product;
    std::uint32_t n;
    std::uint32_t shard;
  };
  for (const auto& [product, n, shard] : {
           vector{"é", 7, 6},
           vector{"日本-USD", 7, 0},
           vector{"BTC-€", 7, 0},
           vector{std::string_view("BTC\0USD", 7), 7, 4},
           vector{std::string_view("BTC\0USD", 7), 1000, 665},
           vector{"BTC", 7, 2},
           vector{"BTC", 1000, 13},
       }) {
    EXPECT_EQ(cratchet::shard_for(product, n), shard) << product << " n=" << n;
  }
}

// Shard counts at and above 2^31 exercise the full 32-bit remainder arithmetic.
// Expected values from int(hashlib.sha256(p).hexdigest(), 16) % n in Python, with n = 2**31 and
// n = 2**32 - 1 (UINT32_MAX).
TEST(shard, shard_for_very_large_shard_counts) {
  struct vector {
    std::string_view product;
    std::uint32_t n;
    std::uint32_t shard;
  };
  constexpr std::uint32_t k_two_pow_31 = 1U << 31;
  constexpr std::uint32_t k_max = std::numeric_limits<std::uint32_t>::max();
  for (const auto& [product, n, shard] : {
           vector{"BTC-USD", k_two_pow_31, 499959829},
           vector{"ETH-USD", k_two_pow_31, 1068126556},
           vector{"btc-usd", k_two_pow_31, 1911245986},
           vector{"é", k_two_pow_31, 732339276},
           vector{"BTC-USD", k_max, 3012140512},
           vector{"ETH-USD", k_max, 1004414207},
           vector{"btc-usd", k_max, 248185277},
           vector{"é", k_max, 2324853468},
       }) {
    EXPECT_EQ(cratchet::shard_for(product, n), shard) << product << " n=" << n;
  }
}

// Property check: no product may map outside [0, num_shards).
TEST(shard, shard_for_is_always_below_num_shards) {
  constexpr std::array<std::uint32_t, 8> counts{
      1, 2, 3, 4, 7, 1000, 1U << 31, std::numeric_limits<std::uint32_t>::max()};
  for (int i = 0; i < 5000; ++i) {
    const std::string product = "PRODUCT" + std::to_string(i) + "-USD";
    for (const auto n : counts) {
      EXPECT_LT(cratchet::shard_for(product, n), n) << product << " n=" << n;
    }
  }
}

// Expected values from int(hashlib.sha256(p).hexdigest(), 16) % n in Python.
TEST(shard, product_ids_are_case_sensitive) {
  struct vector {
    std::string_view product;
    std::uint32_t n;
    std::uint32_t shard;
  };
  for (const auto& [product, n, shard] : {
           vector{"BTC-USD", 7, 6},
           vector{"btc-usd", 7, 4},
           vector{"Btc-Usd", 7, 1},
           vector{"ETH-USD", 7, 0},
           vector{"eth-usd", 7, 2},
           vector{"btc-usd", 4, 2},
           vector{"Btc-Usd", 4, 2},
           vector{"eth-usd", 4, 2},
       }) {
    EXPECT_EQ(cratchet::shard_for(product, n), shard) << product << " n=" << n;
  }
  // Expected digests from hashlib.sha256(b'btc-usd').hexdigest() and b'BTC-USD'.
  EXPECT_NE(cratchet::sha256("BTC-USD"), cratchet::sha256("btc-usd"));
  EXPECT_EQ(hex(cratchet::sha256("btc-usd")),
            "5a355e13eee0a3649ecff7390f3d37265512d9eb8dc02764c2e983f271eb4ca2");
}

TEST(shard, zero_shards_is_rejected) {
  EXPECT_THROW((void)cratchet::shard_for("BTC-USD", 0), std::invalid_argument);
}

}  // namespace
