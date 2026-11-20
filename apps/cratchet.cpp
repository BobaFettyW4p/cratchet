#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "cratchet/capture.hpp"
#include "cratchet/coinbase/subscribe.hpp"
#include "cratchet/config.hpp"
#include "cratchet/feed_client.hpp"
#include "cratchet/log.hpp"
#include "cratchet/nats_publisher.hpp"
#include "cratchet/pipeline.hpp"
#include "cratchet/version.hpp"

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;

constexpr std::string_view usage_text = R"(usage:
  cratchet run [options]
      Connect to the feed, normalize tickers and publish them.
        --config PATH       ingestor config (default /app/services/ingestor/config.json)
        --nats-config PATH  NATS config (default /app/services/common/nats.config.json)
        --dry-run           print ticks to stdout (<subject><TAB><json>); no NATS connection
        --record PATH       append every raw frame to PATH (capture format)
        --duration SECONDS  exit after this long (default: run until SIGINT/SIGTERM)
        --verbose           debug logging
  cratchet normalize [--config PATH] [--publish [--nats-config PATH]] <capture.jsonl>
      Replay a recorded feed through the parser and normalizer. Prints one line per tick
      that would be published, <subject><TAB><json>; a summary goes to stderr. With
      --publish, also publishes each tick to NATS JetStream.
  cratchet --version
)";

struct usage_error {
  std::string message;
};

std::optional<std::int64_t> parse_seconds(std::string_view text) {
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size() || value <= 0) {
    return std::nullopt;
  }
  return value;
}

// Pops the value of an option like "--config PATH".
std::string take_value(const std::vector<std::string_view>& args, std::size_t& i) {
  if (i + 1 >= args.size()) {
    throw usage_error{std::string(args[i]) + " needs a value"};
  }
  return std::string(args[++i]);
}

cratchet::ingestor_config load_config_or_throw(const std::string& path) {
  auto config = cratchet::load_ingestor_config(path);
  if (!config) {
    throw std::runtime_error(config.error());
  }
  return std::move(config).value();
}

std::unique_ptr<cratchet::nats_publisher> connect_publisher_or_throw(
    const std::string& nats_config_path, const cratchet::ingestor_config& config) {
  auto nats = cratchet::load_nats_config(nats_config_path, config, cratchet::process_env());
  if (!nats) {
    throw std::runtime_error(nats.error());
  }
  cratchet::log::info("NATS {} stream={} subjects={}.*", nats.value().urls,
                      nats.value().stream_name, nats.value().subject_prefix);
  auto publisher = cratchet::nats_publisher::connect(nats.value());
  if (!publisher) {
    throw std::runtime_error(publisher.error());
  }
  return std::move(publisher).value();
}

std::string format_publisher_stats(const cratchet::nats_publisher& publisher) {
  const auto stats = publisher.stats();
  return std::format("nats_submitted={} nats_acked={} nats_failed={} nats_reconnects={}",
                     stats.submitted, stats.acked, stats.failed, stats.reconnects);
}

// Waits for outstanding acks before exit; returns false if some never came.
bool drain_publisher(cratchet::nats_publisher& publisher) {
  if (!publisher.flush(std::chrono::seconds(5))) {
    cratchet::log::error("timed out waiting for NATS acknowledgements");
    return false;
  }
  return true;
}

int normalize(const std::vector<std::string_view>& args) {
  std::optional<std::string> config_path;
  std::optional<std::string> capture_path;
  std::string nats_config_path(cratchet::default_nats_config_path);
  bool publish = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--config") {
      config_path = take_value(args, i);
    } else if (args[i] == "--publish") {
      publish = true;
    } else if (args[i] == "--nats-config") {
      nats_config_path = take_value(args, i);
    } else if (!args[i].starts_with("--") && !capture_path) {
      capture_path = std::string(args[i]);
    } else {
      throw usage_error{"unexpected argument " + std::string(args[i])};
    }
  }
  if (!capture_path) {
    throw usage_error{"normalize needs a capture file"};
  }
  // Without a config, use LedgerFlux's defaults (4 shards, market.ticks); products are only
  // used for subscribing, so any placeholder will do.
  cratchet::ingestor_config config;
  config.products = {"*"};
  if (config_path) {
    config = load_config_or_throw(*config_path);
  }

  cratchet::log::set_min_level(publish ? cratchet::log::level::warn : cratchet::log::level::error);
  std::unique_ptr<cratchet::nats_publisher> publisher;
  if (publish) {
    publisher = connect_publisher_or_throw(nats_config_path, config);
  }
  cratchet::capture_reader capture(*capture_path);
  cratchet::pipeline pipeline(config,
                              [&](std::string_view subject, const cratchet::normalized_tick& tick) {
                                std::cout << subject << '\t' << tick.json << '\n';
                                if (publisher) {
                                  publisher->publish(subject, tick.json);
                                }
                              });
  while (const auto frame = capture.next()) {
    pipeline.on_frame(frame->msg, frame->recv_ns);
  }
  std::string summary = cratchet::format_stats(pipeline.stats());
  bool ok = static_cast<bool>(std::cout);
  if (publisher) {
    ok = drain_publisher(*publisher) && ok;
    summary += " " + format_publisher_stats(*publisher);
    ok = ok && publisher->stats().failed == 0;
  }
  std::cerr << summary << '\n';
  return ok ? 0 : 1;
}

int run(const std::vector<std::string_view>& args) {
  std::string config_path(cratchet::default_ingestor_config_path);
  std::string nats_config_path(cratchet::default_nats_config_path);
  bool dry_run = false;
  std::optional<std::string> record_path;
  std::optional<std::int64_t> duration_seconds;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--config") {
      config_path = take_value(args, i);
    } else if (args[i] == "--nats-config") {
      nats_config_path = take_value(args, i);
    } else if (args[i] == "--dry-run") {
      dry_run = true;
    } else if (args[i] == "--record") {
      record_path = take_value(args, i);
    } else if (args[i] == "--duration") {
      duration_seconds = parse_seconds(take_value(args, i));
      if (!duration_seconds) {
        throw usage_error{"--duration needs a positive number of seconds"};
      }
    } else if (args[i] == "--verbose") {
      cratchet::log::set_min_level(cratchet::log::level::debug);
    } else {
      throw usage_error{"unexpected argument " + std::string(args[i])};
    }
  }

  const auto config = load_config_or_throw(config_path);
  cratchet::log::info("cratchet {} products={} channels={} shards={} uri={}", cratchet::version(),
                      config.products.size(), config.channels.size(), config.num_shards,
                      config.ws_uri);

  std::optional<cratchet::capture_writer> recorder;
  if (record_path) {
    recorder.emplace(*record_path);
    cratchet::log::info("recording raw frames to {}", *record_path);
  }

  // Like the Python ingestor, connect to NATS (and set up the stream) before the feed.
  std::unique_ptr<cratchet::nats_publisher> publisher;
  if (!dry_run) {
    publisher = connect_publisher_or_throw(nats_config_path, config);
  }

  cratchet::pipeline pipeline(
      config,
      [&](std::string_view subject, const cratchet::normalized_tick& tick) {
        if (publisher) {
          publisher->publish(subject, tick.json);
        } else {
          std::cout << subject << '\t' << tick.json << '\n';
        }
      },
      recorder ? &*recorder : nullptr);

  // Heartbeats arrive every second per product; without them, a quiet product could look stale.
  const bool heartbeats = std::find(config.channels.begin(), config.channels.end(), "heartbeat") !=
                          config.channels.end();
  if (!heartbeats) {
    cratchet::log::warn("heartbeat channel not subscribed; stale-feed detection relaxed to 60s");
  }

  asio::io_context io;
  cratchet::feed_client_options options;
  options.uri = config.ws_uri;
  options.subscribe_message =
      cratchet::coinbase::subscribe_message(config.products, config.channels);
  options.stale_after = heartbeats ? 5s : 60s;

  int exit_code = 0;
  bool stopping = false;
  asio::signal_set signals(io, SIGINT, SIGTERM);
  asio::steady_timer stats_timer(io);
  asio::steady_timer duration_timer(io);
  std::optional<cratchet::feed_client> client;

  const auto shutdown = [&](std::string_view why) {
    if (stopping) {
      return;
    }
    stopping = true;
    cratchet::log::info("shutting down: {}", why);
    client->stop();
    signals.cancel();
    stats_timer.cancel();
    duration_timer.cancel();
  };

  client.emplace(io, options,
                 cratchet::feed_client_events{
                     .on_frame =
                         [&](std::string_view frame, std::int64_t recv_ns) {
                           if (stopping) {
                             return;
                           }
                           try {
                             pipeline.on_frame(frame, recv_ns);
                           } catch (const std::exception& e) {
                             cratchet::log::error("{}", e.what());
                             exit_code = 1;
                             shutdown("pipeline failure");
                           }
                         },
                     .on_connected =
                         [&] {
                           cratchet::log::info("connected to {} (connection #{})", config.ws_uri,
                                               client->connections());
                         },
                     .on_disconnected =
                         [&](std::string_view reason, std::chrono::milliseconds retry_in) {
                           cratchet::log::warn("disconnected: {}; reconnecting in {} ms", reason,
                                               retry_in.count());
                         }});

  signals.async_wait([&](const boost::system::error_code& ec, int signal) {
    if (!ec) {
      shutdown(signal == SIGINT ? "SIGINT" : "SIGTERM");
    }
  });
  std::function<void()> schedule_stats = [&] {
    stats_timer.expires_after(30s);
    stats_timer.async_wait([&](const boost::system::error_code& ec) {
      if (!ec) {
        cratchet::log::info("stats {}{}", cratchet::format_stats(pipeline.stats()),
                            publisher ? " " + format_publisher_stats(*publisher) : "");
        schedule_stats();
      }
    });
  };
  schedule_stats();
  if (duration_seconds) {
    duration_timer.expires_after(std::chrono::seconds(*duration_seconds));
    duration_timer.async_wait([&](const boost::system::error_code& ec) {
      if (!ec) {
        shutdown("--duration elapsed");
      }
    });
  }

  client->start();
  io.run();

  std::cout.flush();
  if (publisher && !drain_publisher(*publisher)) {
    exit_code = 1;
  }
  if (recorder) {
    recorder->flush();
    cratchet::log::info("recorded {} frames", recorder->frames_written());
  }
  cratchet::log::info("final stats {} connections={}{}", cratchet::format_stats(pipeline.stats()),
                      client->connections(),
                      publisher ? " " + format_publisher_stats(*publisher) : "");
  return exit_code;
}

int dispatch(const std::vector<std::string_view>& args) {
  try {
    if (args.size() == 1 && args[0] == "--version") {
      std::cout << "cratchet " << cratchet::version() << '\n';
      return 0;
    }
    if (!args.empty() && args[0] == "normalize") {
      return normalize({args.begin() + 1, args.end()});
    }
    if (!args.empty() && args[0] == "run") {
      return run({args.begin() + 1, args.end()});
    }
    std::cerr << usage_text;
    return 2;
  } catch (const usage_error& e) {
    std::cerr << "cratchet: " << e.message << "\n\n" << usage_text;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "cratchet: " << e.what() << '\n';
    return 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const int code = dispatch(std::vector<std::string_view>(argv + 1, argv + argc));
  cratchet::nats_publisher::shutdown_library();
  return code;
}
