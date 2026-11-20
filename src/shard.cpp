#include "cratchet/shard.hpp"

#include <openssl/evp.h>

#include <stdexcept>

namespace cratchet {

sha256_digest sha256(std::string_view data) {
  sha256_digest digest{};
  unsigned int length = 0;
  if (EVP_Digest(data.data(), data.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 ||
      length != digest.size()) {
    throw std::runtime_error("EVP_Digest(SHA-256) failed");
  }
  return digest;
}

std::uint32_t shard_for(std::string_view product, std::uint32_t num_shards) {
  if (num_shards == 0) {
    throw std::invalid_argument("num_shards must be positive");
  }
  // Horner's rule mod n: exact for any n, no 256-bit arithmetic needed.
  std::uint64_t remainder = 0;
  for (const std::uint8_t byte : sha256(product)) {
    remainder = (remainder * 256 + byte) % num_shards;
  }
  return static_cast<std::uint32_t>(remainder);
}

}  // namespace cratchet
