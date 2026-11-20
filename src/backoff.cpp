#include "cratchet/backoff.hpp"

#include <algorithm>
#include <stdexcept>

namespace cratchet {

backoff::backoff(duration initial, duration max, std::uint64_t seed)
    : initial_(initial), max_(max), ceiling_(initial), rng_(seed) {
  if (initial.count() <= 0 || max < initial) {
    throw std::invalid_argument("backoff: need 0 < initial <= max");
  }
}

backoff::duration backoff::next() {
  const duration ceiling = ceiling_;
  ceiling_ = std::min(max_, ceiling_ * 2);
  std::uniform_int_distribution<duration::rep> pick(ceiling.count() / 2, ceiling.count());
  return duration{pick(rng_)};
}

}  // namespace cratchet
