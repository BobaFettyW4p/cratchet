// feed_client against an in-process WebSocket server on 127.0.0.1 (plain ws://; TLS is
// exercised against the live feed, not here).

#include "cratchet/feed_client.hpp"

#include <gtest/gtest.h>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;
using ws_stream = websocket::stream<beast::tcp_stream>;

// Accepts WebSocket connections, records each client's subscribe message, then hands the
// connection to `behaviour(stream, connection_index)`.
class test_server {
 public:
  using behaviour_fn = std::function<asio::awaitable<void>(ws_stream&, int)>;

  test_server(asio::io_context& io, behaviour_fn behaviour)
      : acceptor_(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)),
        behaviour_(std::move(behaviour)) {
    asio::co_spawn(io, accept_loop(), asio::detached);
  }

  [[nodiscard]] std::string uri() const {
    return "ws://127.0.0.1:" + std::to_string(acceptor_.local_endpoint().port()) + "/";
  }
  void stop() { acceptor_.close(); }
  [[nodiscard]] const std::vector<std::string>& subscribes() const { return subscribes_; }

  // Reads until the client goes away.
  static asio::awaitable<void> drain(ws_stream& stream) {
    beast::flat_buffer buffer;
    try {
      for (;;) {
        co_await stream.async_read(buffer, asio::use_awaitable);
        buffer.clear();
      }
    } catch (const boost::system::system_error&) {
    }
  }

  static asio::awaitable<void> send(ws_stream& stream, const std::string& text) {
    co_await stream.async_write(asio::buffer(text), asio::use_awaitable);
  }

 private:
  asio::awaitable<void> accept_loop() {
    int index = 0;
    try {
      for (;;) {
        tcp::socket socket = co_await acceptor_.async_accept(asio::use_awaitable);
        asio::co_spawn(acceptor_.get_executor(), handle(std::move(socket), index++),
                       asio::detached);
      }
    } catch (const boost::system::system_error&) {
    }
  }

  asio::awaitable<void> handle(tcp::socket socket, int index) {
    try {
      ws_stream stream(std::move(socket));
      co_await stream.async_accept(asio::use_awaitable);
      beast::flat_buffer buffer;
      co_await stream.async_read(buffer, asio::use_awaitable);
      subscribes_.push_back(beast::buffers_to_string(buffer.data()));
      co_await behaviour_(stream, index);
    } catch (const boost::system::system_error&) {
    }
  }

  tcp::acceptor acceptor_;
  behaviour_fn behaviour_;
  std::vector<std::string> subscribes_;
};

cratchet::feed_client_options options_for(const std::string& uri) {
  cratchet::feed_client_options options;
  options.uri = uri;
  options.subscribe_message = R"({"type":"subscribe"})";
  options.connect_timeout = 2s;
  options.stale_after = 2s;
  options.backoff_initial = 10ms;
  options.backoff_max = 40ms;
  return options;
}

TEST(feed_client, subscribes_and_delivers_frames) {
  asio::io_context io;
  test_server server(io, [](ws_stream& stream, int) -> asio::awaitable<void> {
    co_await test_server::send(stream, "one");
    co_await test_server::send(stream, "two");
    co_await test_server::send(stream, "three");
    co_await test_server::drain(stream);
  });

  std::vector<std::string> frames;
  int connected = 0;
  cratchet::feed_client* client_ptr = nullptr;
  cratchet::feed_client client(io, options_for(server.uri()),
                               {.on_frame =
                                    [&](std::string_view frame, std::int64_t recv_ns) {
                                      EXPECT_GT(recv_ns, 0);
                                      frames.emplace_back(frame);
                                      if (frames.size() == 3) {
                                        client_ptr->stop();
                                        server.stop();
                                      }
                                    },
                                .on_connected = [&] { ++connected; },
                                .on_disconnected = {}});
  client_ptr = &client;
  client.start();
  io.run_for(5s);

  EXPECT_TRUE(io.stopped()) << "client did not finish after stop()";
  EXPECT_EQ(frames, (std::vector<std::string>{"one", "two", "three"}));
  EXPECT_EQ(connected, 1);
  EXPECT_EQ(client.connections(), 1U);
  EXPECT_EQ(server.subscribes(), (std::vector<std::string>{R"({"type":"subscribe"})"}));
}

TEST(feed_client, reconnects_and_resubscribes_after_server_close) {
  asio::io_context io;
  test_server server(io, [](ws_stream& stream, int index) -> asio::awaitable<void> {
    co_await test_server::send(stream, "frame-" + std::to_string(index));
    if (index == 0) {
      co_await stream.async_close(websocket::close_code::going_away, asio::use_awaitable);
    } else {
      co_await test_server::drain(stream);
    }
  });

  std::vector<std::string> frames;
  std::vector<std::chrono::milliseconds> retries;
  cratchet::feed_client* client_ptr = nullptr;
  cratchet::feed_client client(io, options_for(server.uri()),
                               {.on_frame =
                                    [&](std::string_view frame, std::int64_t) {
                                      frames.emplace_back(frame);
                                      if (frames.size() == 2) {
                                        client_ptr->stop();
                                        server.stop();
                                      }
                                    },
                                .on_connected = {},
                                .on_disconnected =
                                    [&](std::string_view, std::chrono::milliseconds retry_in) {
                                      retries.push_back(retry_in);
                                    }});
  client_ptr = &client;
  client.start();
  io.run_for(5s);

  EXPECT_TRUE(io.stopped());
  EXPECT_EQ(frames, (std::vector<std::string>{"frame-0", "frame-1"}));
  EXPECT_EQ(server.subscribes().size(), 2U);
  ASSERT_EQ(retries.size(), 1U);
  EXPECT_LE(retries[0], 10ms);  // the first connection delivered data, so backoff restarted
  EXPECT_EQ(client.connections(), 2U);
}

TEST(feed_client, reconnects_when_the_feed_goes_quiet) {
  asio::io_context io;
  test_server server(io, [](ws_stream& stream, int index) -> asio::awaitable<void> {
    if (index > 0) {
      co_await test_server::send(stream, "fresh");
    }
    co_await test_server::drain(stream);  // connection 0 stays silent
  });

  auto options = options_for(server.uri());
  options.stale_after = 200ms;
  std::vector<std::string> reasons;
  std::vector<std::string> frames;
  const auto started = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration first_disconnect{};
  cratchet::feed_client* client_ptr = nullptr;
  cratchet::feed_client client(io, options,
                               {.on_frame =
                                    [&](std::string_view frame, std::int64_t) {
                                      frames.emplace_back(frame);
                                      client_ptr->stop();
                                      server.stop();
                                    },
                                .on_connected = {},
                                .on_disconnected =
                                    [&](std::string_view reason, std::chrono::milliseconds) {
                                      if (reasons.empty()) {
                                        first_disconnect =
                                            std::chrono::steady_clock::now() - started;
                                      }
                                      reasons.emplace_back(reason);
                                    }});
  client_ptr = &client;
  client.start();
  io.run_for(5s);

  EXPECT_TRUE(io.stopped());
  ASSERT_EQ(reasons.size(), 1U);
  EXPECT_NE(reasons[0].find("timed out"), std::string::npos) << reasons[0];
  EXPECT_GE(first_disconnect, 200ms);
  EXPECT_LT(first_disconnect, 2s);
  EXPECT_EQ(frames, (std::vector<std::string>{"fresh"}));
}

TEST(feed_client, keeps_retrying_with_growing_backoff_when_nothing_listens) {
  asio::io_context io;
  // Grab a free port, then close it so connections are refused.
  std::string uri;
  {
    tcp::acceptor probe(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    uri = "ws://127.0.0.1:" + std::to_string(probe.local_endpoint().port());
  }

  std::vector<std::chrono::milliseconds> retries;
  cratchet::feed_client* client_ptr = nullptr;
  cratchet::feed_client client(
      io, options_for(uri),
      {.on_frame = {},
       .on_connected = {},
       .on_disconnected = [&](std::string_view, std::chrono::milliseconds retry_in) {
         retries.push_back(retry_in);
         if (retries.size() == 4) {
           client_ptr->stop();
         }
       }});
  client_ptr = &client;
  client.start();
  io.run_for(5s);

  EXPECT_TRUE(io.stopped());
  ASSERT_EQ(retries.size(), 4U);
  // Ceilings 10, 20, 40, 40 ms (capped); each delay is in [ceiling/2, ceiling].
  EXPECT_LE(retries[0], 10ms);
  EXPECT_GE(retries[1], 10ms);
  EXPECT_GE(retries[2], 20ms);
  EXPECT_LE(retries[3], 40ms);
  EXPECT_EQ(client.connections(), 0U);
}

TEST(feed_client, rejects_non_websocket_uris) {
  asio::io_context io;
  cratchet::feed_client client(io, options_for("https://example.com"), {});
  EXPECT_THROW(client.start(), std::invalid_argument);
}

}  // namespace
