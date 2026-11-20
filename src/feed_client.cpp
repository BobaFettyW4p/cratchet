#include "cratchet/feed_client.hpp"

#include <openssl/ssl.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "cratchet/backoff.hpp"
#include "cratchet/version.hpp"

namespace cratchet {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

std::optional<ws_endpoint> parse_ws_uri(std::string_view uri) {
  ws_endpoint endpoint;
  if (uri.starts_with("wss://")) {
    endpoint.tls = true;
    uri.remove_prefix(6);
  } else if (uri.starts_with("ws://")) {
    endpoint.tls = false;
    uri.remove_prefix(5);
  } else {
    return std::nullopt;
  }
  const auto path_start = uri.find_first_of("/?");
  const std::string_view authority = uri.substr(0, path_start);
  endpoint.target =
      path_start == std::string_view::npos ? "/" : std::string(uri.substr(path_start));
  if (endpoint.target.front() == '?') {
    endpoint.target.insert(0, "/");
  }

  const auto colon = authority.rfind(':');
  if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
    endpoint.host = authority.substr(0, colon);
    endpoint.port = authority.substr(colon + 1);
    unsigned port = 0;
    const auto* const last = endpoint.port.data() + endpoint.port.size();
    const auto [ptr, ec] = std::from_chars(endpoint.port.data(), last, port);
    if (endpoint.port.empty() || ec != std::errc{} || ptr != last || port == 0 || port > 65535) {
      return std::nullopt;
    }
  } else {
    endpoint.host = authority;
    endpoint.port = endpoint.tls ? "443" : "80";
  }
  if (endpoint.host.empty() || endpoint.host.find('@') != std::string::npos) {
    return std::nullopt;
  }
  return endpoint;
}

namespace {

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// vcpkg's static OpenSSL looks for CA certificates under its own install prefix, so also try the
// usual system bundle locations. SSL_CERT_FILE / SSL_CERT_DIR are honoured by
// set_default_verify_paths().
void load_ca_certificates(ssl::context& context, const std::string& ca_file) {
  if (!ca_file.empty()) {
    context.load_verify_file(ca_file);
    return;
  }
  context.set_default_verify_paths();
  for (const char* bundle : {"/etc/ssl/certs/ca-certificates.crt",
                             "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
    std::error_code ec;
    if (std::filesystem::exists(bundle, ec)) {
      context.load_verify_file(bundle);
      return;
    }
  }
}

}  // namespace

class feed_client::impl {
 public:
  impl(asio::io_context& io, feed_client_options options, feed_client_events events)
      : io_(io),
        options_(std::move(options)),
        events_(std::move(events)),
        tls_(ssl::context::tls_client),
        retry_timer_(io),
        backoff_(options_.backoff_initial, options_.backoff_max) {}

  void start() {
    const auto endpoint = parse_ws_uri(options_.uri);
    if (!endpoint) {
      throw std::invalid_argument("not a ws:// or wss:// URI: " + options_.uri);
    }
    endpoint_ = *endpoint;
    if (endpoint_.tls) {
      tls_.set_verify_mode(ssl::verify_peer);
      load_ca_certificates(tls_, options_.ca_file);
    }
    asio::co_spawn(io_, run(), asio::detached);
  }

  void stop() {
    asio::post(io_, [this] {
      stopping_ = true;
      retry_timer_.cancel();
      if (close_) {
        close_();
      }
    });
  }

  [[nodiscard]] std::uint64_t connections() const noexcept {
    return connections_.load(std::memory_order_relaxed);
  }

 private:
  asio::awaitable<void> run() {
    while (!stopping_) {
      received_ = false;
      std::string reason = "connection closed";
      try {
        if (endpoint_.tls) {
          co_await session<websocket::stream<ssl::stream<beast::tcp_stream>>>();
        } else {
          co_await session<websocket::stream<beast::tcp_stream>>();
        }
      } catch (const boost::system::system_error& e) {
        reason = e.code() == beast::error::timeout ? "timed out (no data or handshake)"
                                                   : e.code().message();
      } catch (const std::exception& e) {
        reason = e.what();
      }
      close_ = nullptr;
      if (stopping_) {
        break;
      }
      // A connection that delivered data was healthy; start the backoff over.
      if (received_) {
        backoff_.reset();
      }
      const auto delay = backoff_.next();
      if (events_.on_disconnected) {
        events_.on_disconnected(reason, delay);
      }
      retry_timer_.expires_after(delay);
      boost::system::error_code ignored;
      co_await retry_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
  }

  // One connection's lifetime. The stream lives in this coroutine's frame; close_ refers to it
  // only while the coroutine is running.
  template <typename Stream>
  asio::awaitable<void> session() {
    constexpr bool tls = !std::is_same_v<Stream, websocket::stream<beast::tcp_stream>>;
    auto stream = [this] {
      if constexpr (tls) {
        return Stream(io_, tls_);
      } else {
        return Stream(io_);
      }
    }();
    auto& tcp_layer = beast::get_lowest_layer(stream);
    tcp::resolver resolver(io_);
    // stop() while connecting: cancel the lookup and close the socket, which aborts whatever is
    // pending; check_stopping() catches a stop that lands between steps.
    close_ = [&tcp_layer, &resolver] {
      resolver.cancel();
      tcp_layer.close();
    };
    const auto check_stopping = [this] {
      if (stopping_) {
        throw std::runtime_error("stopped");
      }
    };

    tcp_layer.expires_after(options_.connect_timeout);
    const auto addresses =
        co_await resolver.async_resolve(endpoint_.host, endpoint_.port, asio::use_awaitable);
    check_stopping();
    co_await tcp_layer.async_connect(addresses, asio::use_awaitable);
    check_stopping();

    if constexpr (tls) {
      auto& tls_stream = stream.next_layer();
      // SNI, and check the certificate is for the host we asked for.
      if (SSL_set_tlsext_host_name(tls_stream.native_handle(), endpoint_.host.c_str()) != 1) {
        throw std::runtime_error("cannot set TLS SNI host name");
      }
      tls_stream.set_verify_callback(ssl::host_name_verification(endpoint_.host));
      co_await tls_stream.async_handshake(ssl::stream_base::client, asio::use_awaitable);
      check_stopping();
    }

    // Beast's own timers take over from here: handshake/close timeouts, and closing the
    // connection if nothing at all arrives for stale_after.
    tcp_layer.expires_never();
    websocket::stream_base::timeout timeouts{};
    timeouts.handshake_timeout = options_.connect_timeout;
    timeouts.idle_timeout = options_.stale_after;
    timeouts.keep_alive_pings = false;
    stream.set_option(timeouts);
    stream.set_option(websocket::stream_base::decorator([](websocket::request_type& request) {
      request.set(beast::http::field::user_agent, "cratchet/" + std::string(version()));
    }));
    stream.read_message_max(options_.max_message_bytes);

    const bool default_port = endpoint_.port == (endpoint_.tls ? "443" : "80");
    const std::string host_header =
        default_port ? endpoint_.host : endpoint_.host + ":" + endpoint_.port;
    co_await stream.async_handshake(host_header, endpoint_.target, asio::use_awaitable);
    check_stopping();
    co_await stream.async_write(asio::buffer(options_.subscribe_message), asio::use_awaitable);

    close_ = [&stream] {
      stream.async_close(websocket::close_code::normal, [](const boost::system::error_code&) {});
    };
    connections_.fetch_add(1, std::memory_order_relaxed);
    if (events_.on_connected) {
      events_.on_connected();
    }

    beast::flat_buffer buffer;
    for (;;) {
      buffer.clear();
      co_await stream.async_read(buffer, asio::use_awaitable);
      const std::int64_t recv_ns = now_ns();
      received_ = true;
      const auto data = buffer.cdata();
      if (events_.on_frame) {
        events_.on_frame(std::string_view(static_cast<const char*>(data.data()), data.size()),
                         recv_ns);
      }
    }
  }

  asio::io_context& io_;
  feed_client_options options_;
  feed_client_events events_;
  ws_endpoint endpoint_;
  ssl::context tls_;
  asio::steady_timer retry_timer_;
  backoff backoff_;
  std::function<void()> close_;  // closes the current connection, if any
  bool stopping_ = false;
  bool received_ = false;  // the current connection has delivered at least one frame
  std::atomic<std::uint64_t> connections_{0};
};

feed_client::feed_client(asio::io_context& io, feed_client_options options,
                         feed_client_events events)
    : impl_(std::make_unique<impl>(io, std::move(options), std::move(events))) {}

feed_client::~feed_client() = default;

void feed_client::start() { impl_->start(); }

void feed_client::stop() { impl_->stop(); }

std::uint64_t feed_client::connections() const noexcept { return impl_->connections(); }

}  // namespace cratchet
