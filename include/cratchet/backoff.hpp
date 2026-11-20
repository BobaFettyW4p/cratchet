#pragma once

#include <chrono>
#include <cstdint>
#include <random>

namespace cratchet {

// Exponential reconnect backoff with jitter: the n-th delay is drawn uniformly from
// [d/2, d] where d = min(max, initial * 2^n). The jitter keeps many instances from
// reconnecting in lockstep after a shared outage.
class backoff {
 public:
  using duration = std::chrono::milliseconds;

  backoff(duration initial, duration max, std::uint64_t seed = std::random_device{}());

  // The delay before the next attempt; each call doubles the ceiling up to `max`.
  duration next();

  // Call after a healthy connection so the next failure starts from `initial` again.
  void reset() noexcept { ceiling_ = initial_; }

 private:
  duration initial_;
  duration max_;
  duration ceiling_;
  std::mt19937_64 rng_;
};

}  // namespace cratchet
