#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "cratchet/config.hpp"
#include "cratchet/result.hpp"

namespace cratchet {

struct publisher_stats {
  std::uint64_t submitted = 0;  // handed to the client library
  std::uint64_t acked = 0;      // stored by JetStream
  std::uint64_t failed = 0;     // rejected on submit, negatively acked, or ack timed out
  std::uint64_t disconnects = 0;
  std::uint64_t reconnects = 0;
};

// Publishes ticks to NATS JetStream the way LedgerFlux's NATSStreamManager does
// (docs/contract.md §6): connect with exponential backoff, then use the stream if it exists,
// otherwise optionally delete and create it with subjects ["<subject_prefix>.*"].
//
// Publishing is asynchronous: publish() queues the message and returns; acknowledgements are
// processed on the NATS library's threads. The connection reconnects by itself after a drop,
// buffering what is published meanwhile.
class nats_publisher {
 public:
  struct options {
    std::chrono::seconds connect_timeout{60};  // LedgerFlux: 60 s of 1, 2, 4, 8 s backoff
    std::chrono::milliseconds attempt_timeout{5000};
    std::int64_t max_pending_acks = 4096;  // publish() waits (up to stall_wait) past this
    std::chrono::milliseconds stall_wait{200};
    std::chrono::milliseconds ack_timeout{5000};  // an unacknowledged message counts as failed
  };

  static result<std::unique_ptr<nats_publisher>, std::string> connect(const nats_config& config,
                                                                      options opts);
  static result<std::unique_ptr<nats_publisher>, std::string> connect(const nats_config& config) {
    return connect(config, options{});
  }

  ~nats_publisher();
  nats_publisher(const nats_publisher&) = delete;
  nats_publisher& operator=(const nats_publisher&) = delete;
  nats_publisher(nats_publisher&&) = delete;
  nats_publisher& operator=(nats_publisher&&) = delete;

  // Queues one message. False (and counted as failed) if it could not be queued.
  bool publish(std::string_view subject, std::string_view payload);

  // Waits until every queued message is acknowledged; false on timeout.
  bool flush(std::chrono::milliseconds timeout);

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] publisher_stats stats() const noexcept;
  [[nodiscard]] const nats_config& config() const noexcept;

  // Releases the NATS library's global state. Call once at exit, after every publisher is gone.
  static void shutdown_library();

  class impl;

 private:
  explicit nats_publisher(std::unique_ptr<impl> state);
  std::unique_ptr<impl> impl_;
};

}  // namespace cratchet
