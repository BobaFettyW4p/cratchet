#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace boost::asio {
class io_context;
}  // namespace boost::asio

namespace cratchet {

// A parsed ws:// or wss:// URI.
struct ws_endpoint {
  bool tls = true;
  std::string host;
  std::string port;    // "443" / "80" when not given
  std::string target;  // path and query, "/" when not given

  bool operator==(const ws_endpoint&) const = default;
};

// Parses "wss://host[:port][/path]" or "ws://...". nullopt if malformed.
std::optional<ws_endpoint> parse_ws_uri(std::string_view uri);

struct feed_client_options {
  std::string uri;                // e.g. wss://ws-feed.exchange.coinbase.com
  std::string subscribe_message;  // sent after every (re)connect
  std::chrono::milliseconds connect_timeout{std::chrono::seconds(10)};  // DNS+TCP+TLS+WS
  // No data for this long closes the connection and reconnects. Coinbase heartbeats arrive
  // every second per product, so a few seconds of silence means the feed is stale.
  std::chrono::milliseconds stale_after{std::chrono::seconds(5)};
  std::chrono::milliseconds backoff_initial{std::chrono::seconds(1)};
  std::chrono::milliseconds backoff_max{std::chrono::seconds(30)};
  std::size_t max_message_bytes = std::size_t{64} << 20U;  // full-depth snapshots are ~1 MB
  std::string ca_file;  // PEM bundle; empty = SSL_CERT_FILE or the system bundle
};

struct feed_client_events {
  // Every text frame, with the wall-clock receive time. `frame` is valid only during the call.
  std::function<void(std::string_view frame, std::int64_t recv_ns)> on_frame;
  // The connection is up and the subscribe request has been sent.
  std::function<void()> on_connected;
  // The connection ended (or never came up); `retry_in` is the backoff before the next attempt.
  std::function<void(std::string_view reason, std::chrono::milliseconds retry_in)> on_disconnected;
};

// Keeps one WebSocket subscription to the feed alive: connects, subscribes, delivers frames,
// and on any failure (connect error, server close, stale feed) reconnects with exponential
// backoff. Runs on the given io_context; all callbacks are invoked from its thread(s).
// The client must outlive the io_context's run().
class feed_client {
 public:
  feed_client(boost::asio::io_context& io, feed_client_options options, feed_client_events events);
  ~feed_client();
  feed_client(const feed_client&) = delete;
  feed_client& operator=(const feed_client&) = delete;
  feed_client(feed_client&&) = delete;
  feed_client& operator=(feed_client&&) = delete;

  // Begins connecting. Throws std::invalid_argument if the URI is not ws:// or wss://.
  void start();

  // Closes the connection and stops reconnecting; safe to call from any thread. Once the close
  // finishes, the client has no pending work, so io_context::run() can return.
  void stop();

  // Successful connections so far; reconnects = connections() - 1.
  [[nodiscard]] std::uint64_t connections() const noexcept;

 private:
  class impl;
  std::unique_ptr<impl> impl_;
};

}  // namespace cratchet
