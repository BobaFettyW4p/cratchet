#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace cratchet {

using sha256_digest = std::array<std::uint8_t, 32>;

sha256_digest sha256(std::string_view data);

// The shard for `product`, identical to LedgerFlux's shard_product(): the SHA-256 digest of the
// product's UTF-8 bytes, read as a big-endian 256-bit integer, modulo `num_shards`
// (docs/contract.md §3). Throws std::invalid_argument if num_shards is 0.
std::uint32_t shard_for(std::string_view product, std::uint32_t num_shards);

}  // namespace cratchet
