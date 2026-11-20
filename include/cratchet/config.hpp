#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cratchet/result.hpp"

// Service configuration, read from the same two files LedgerFlux's ingestor reads, with the same
// defaults and precedence, so the existing Kubernetes ConfigMaps work unchanged
// (docs/contract.md §6).
namespace cratchet {

// Where the LedgerFlux ingestor Deployment mounts its ConfigMaps.
inline constexpr std::string_view default_ingestor_config_path =
    "/app/services/ingestor/config.json";
inline constexpr std::string_view default_nats_config_path =
    "/app/services/common/nats.config.json";

// The ingestor's config.json (LedgerFlux services/ingestor/app.py, CoinbaseIngester.__init__).
struct ingestor_config {
  std::vector<std::string> products;  // trimmed and upper-cased
  std::vector<std::string> channels{"ticker", "heartbeat"};
  std::string ws_uri{"wss://ws-feed.exchange.coinbase.com"};
  std::uint32_t num_shards = 4;
  std::string stream_name{"market_ticks"};
  std::string subject_prefix{"market.ticks"};
  std::uint16_t health_port = 8080;
};

// The shared nats.config.json (LedgerFlux services/common/config.py, load_nats_config).
struct nats_config {
  std::string urls{"nats://localhost:4222"};
  std::string stream_name{"market_ticks"};
  std::string subject_prefix{"market.ticks"};
  std::int64_t retention_minutes = 30;
  std::int64_t max_age_seconds = 1800;
  bool delete_existing = false;
};

// Looks up an environment variable; nullopt if unset. Injectable for tests.
using env_lookup = std::function<std::optional<std::string>(const std::string& name)>;
env_lookup process_env();

// Parses ingestor config JSON. Missing keys take LedgerFlux's defaults. Unlike Python, which
// coerces with str()/int(), values must have the right JSON type. Also rejects what would
// fail later at runtime: no products, num_shards of 0, or an out-of-range health_port.
result<ingestor_config, std::string> parse_ingestor_config(std::string_view json);

// Reads and parses the file. Like LedgerFlux, a missing ingestor config is an error.
result<ingestor_config, std::string> load_ingestor_config(const std::string& path);

// Resolves NATS settings as load_nats_config(stream_name=..., subject_prefix=...) does when the
// ingestor calls it:
//   urls, retention_minutes, max_age_seconds, delete_existing: env var > file > default;
//   stream_name, subject_prefix: the ingestor config's value > env var > file > default.
// The ingestor config's stream_name and subject_prefix always win unless they are empty,
// because the ingestor passes them explicitly. `file_json` is the nats.config.json text, or
// nullopt if the file does not exist (allowed; defaults apply).
result<nats_config, std::string> resolve_nats_config(const ingestor_config& service,
                                                     const std::optional<std::string>& file_json,
                                                     const env_lookup& env);

// Reads `path` if it exists, then resolve_nats_config().
result<nats_config, std::string> load_nats_config(const std::string& path,
                                                  const ingestor_config& service,
                                                  const env_lookup& env);

}  // namespace cratchet
