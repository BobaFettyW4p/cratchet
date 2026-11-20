#include "cratchet/config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using cratchet::ingestor_config;
using cratchet::nats_config;

cratchet::env_lookup fake_env(std::map<std::string, std::string> vars) {
  return [vars = std::move(vars)](const std::string& name) -> std::optional<std::string> {
    if (auto it = vars.find(name); it != vars.end()) {
      return it->second;
    }
    return std::nullopt;
  };
}

ingestor_config parse_ok(const std::string& json) {
  auto config = cratchet::parse_ingestor_config(json);
  EXPECT_TRUE(config) << (config ? "" : config.error());
  return config ? config.value() : ingestor_config{};
}

std::string parse_error(const std::string& json) {
  auto config = cratchet::parse_ingestor_config(json);
  EXPECT_FALSE(config) << json;
  return config ? "" : config.error();
}

nats_config resolve_ok(const ingestor_config& service, const std::optional<std::string>& file,
                       std::map<std::string, std::string> env = {}) {
  auto config = cratchet::resolve_nats_config(service, file, fake_env(std::move(env)));
  EXPECT_TRUE(config) << (config ? "" : config.error());
  return config ? config.value() : nats_config{};
}

// k8s/services/configs/ingestor.json at LedgerFlux 004a8c7.
constexpr const char* ledgerflux_ingestor_json = R"({
  "products": ["BTC-USD", "ETH-USD", "ADA-USD"],
  "channels": ["ticker", "heartbeat"],
  "ws_uri": "wss://ws-feed.exchange.coinbase.com",
  "num_shards": 4,
  "stream_name": "market_ticks",
  "subject_prefix": "market.ticks"
})";

// k8s/services/configs/nats.json at LedgerFlux 004a8c7.
constexpr const char* ledgerflux_nats_json = R"({
  "urls": "nats://nats:4222",
  "stream_name": "market_ticks",
  "subject_prefix": "market.ticks",
  "retention_minutes": 30,
  "max_age_seconds": 1800,
  "delete_existing": false
})";

TEST(config, ledgerflux_ingestor_configmap) {
  const auto config = parse_ok(ledgerflux_ingestor_json);
  EXPECT_EQ(config.products, (std::vector<std::string>{"BTC-USD", "ETH-USD", "ADA-USD"}));
  EXPECT_EQ(config.channels, (std::vector<std::string>{"ticker", "heartbeat"}));
  EXPECT_EQ(config.ws_uri, "wss://ws-feed.exchange.coinbase.com");
  EXPECT_EQ(config.num_shards, 4U);
  EXPECT_EQ(config.stream_name, "market_ticks");
  EXPECT_EQ(config.subject_prefix, "market.ticks");
  EXPECT_EQ(config.health_port, 8080);
}

TEST(config, ingestor_defaults_and_normalization) {
  const auto config = parse_ok(R"({"products": [" btc-usd ", "eth-Usd"], "health_port": 9000})");
  EXPECT_EQ(config.products, (std::vector<std::string>{"BTC-USD", "ETH-USD"}));
  EXPECT_EQ(config.channels, (std::vector<std::string>{"ticker", "heartbeat"}));
  EXPECT_EQ(config.ws_uri, "wss://ws-feed.exchange.coinbase.com");
  EXPECT_EQ(config.num_shards, 4U);
  EXPECT_EQ(config.stream_name, "market_ticks");
  EXPECT_EQ(config.subject_prefix, "market.ticks");
  EXPECT_EQ(config.health_port, 9000);
}

TEST(config, ingestor_errors) {
  EXPECT_NE(parse_error("{nope").find("invalid JSON"), std::string::npos);
  EXPECT_NE(parse_error("[]").find("not a JSON object"), std::string::npos);
  EXPECT_NE(parse_error("{}").find("products"), std::string::npos);
  EXPECT_NE(parse_error(R"({"products": []})").find("products"), std::string::npos);
  EXPECT_NE(parse_error(R"({"products": ["  "]})").find("products"), std::string::npos);
  EXPECT_NE(parse_error(R"({"products": "BTC-USD"})").find("array of strings"), std::string::npos);
  EXPECT_NE(parse_error(R"({"products": ["A"], "num_shards": 0})").find("num_shards"),
            std::string::npos);
  EXPECT_NE(parse_error(R"({"products": ["A"], "num_shards": "4"})").find("integer"),
            std::string::npos);
  EXPECT_NE(parse_error(R"({"products": ["A"], "health_port": 70000})").find("health_port"),
            std::string::npos);
  EXPECT_NE(parse_error(R"({"products": ["A"], "ws_uri": 5})").find("ws_uri"), std::string::npos);
}

TEST(config, nats_ledgerflux_configmap) {
  const auto config = resolve_ok(parse_ok(ledgerflux_ingestor_json), ledgerflux_nats_json);
  EXPECT_EQ(config.urls, "nats://nats:4222");
  EXPECT_EQ(config.stream_name, "market_ticks");
  EXPECT_EQ(config.subject_prefix, "market.ticks");
  EXPECT_EQ(config.retention_minutes, 30);
  EXPECT_EQ(config.max_age_seconds, 1800);
  EXPECT_FALSE(config.delete_existing);
}

TEST(config, nats_defaults_without_a_file) {
  const auto config = resolve_ok(parse_ok(R"({"products": ["A"]})"), std::nullopt);
  EXPECT_EQ(config.urls, "nats://localhost:4222");
  EXPECT_EQ(config.retention_minutes, 30);
  EXPECT_EQ(config.max_age_seconds, 1800);
  EXPECT_FALSE(config.delete_existing);
}

TEST(config, nats_env_overrides_file) {
  const auto config = resolve_ok(parse_ok(R"({"products": ["A"]})"), ledgerflux_nats_json,
                                 {{"NATS_URLS", "nats://a:1,nats://b:2"},
                                  {"JS_RETENTION_MINUTES", " 5 "},
                                  {"NATS_DELETE_EXISTING", "Yes"}});
  EXPECT_EQ(config.urls, "nats://a:1,nats://b:2");
  EXPECT_EQ(config.retention_minutes, 5);
  // The file's max_age_seconds still applies; only a missing value derives from retention.
  EXPECT_EQ(config.max_age_seconds, 1800);
  EXPECT_TRUE(config.delete_existing);
}

TEST(config, nats_max_age_derives_from_retention) {
  const auto config = resolve_ok(parse_ok(R"({"products": ["A"]})"), R"({"retention_minutes": 2})",
                                 {{"JS_RETENTION_MINUTES", "3"}});
  EXPECT_EQ(config.retention_minutes, 3);
  EXPECT_EQ(config.max_age_seconds, 180);
}

// The ingestor passes its own stream_name and subject_prefix to load_nats_config, so they win
// over the env vars and the shared file, unless they are empty.
TEST(config, nats_ingestor_stream_settings_take_precedence) {
  auto service =
      parse_ok(R"({"products": ["A"], "stream_name": "svc", "subject_prefix": "svc.p"})");
  const std::map<std::string, std::string> env{{"JS_STREAM_NAME", "env"},
                                               {"JS_SUBJECT_PREFIX", "env.p"}};
  auto config = resolve_ok(service, R"({"stream_name": "file", "subject_prefix": "file.p"})", env);
  EXPECT_EQ(config.stream_name, "svc");
  EXPECT_EQ(config.subject_prefix, "svc.p");

  service.stream_name.clear();
  service.subject_prefix.clear();
  config = resolve_ok(service, R"({"stream_name": "file", "subject_prefix": "file.p"})", env);
  EXPECT_EQ(config.stream_name, "env");
  EXPECT_EQ(config.subject_prefix, "env.p");

  config = resolve_ok(service, R"({"stream_name": "file", "subject_prefix": "file.p"})");
  EXPECT_EQ(config.stream_name, "file");
  EXPECT_EQ(config.subject_prefix, "file.p");

  config = resolve_ok(service, "{}");
  EXPECT_EQ(config.stream_name, "market_ticks");
  EXPECT_EQ(config.subject_prefix, "market.ticks");
}

TEST(config, nats_delete_existing_accepts_ledgerflux_spellings) {
  const auto service = parse_ok(R"({"products": ["A"]})");
  EXPECT_TRUE(resolve_ok(service, R"({"delete_existing": true})").delete_existing);
  EXPECT_TRUE(resolve_ok(service, R"({"delete_existing": "on"})").delete_existing);
  EXPECT_TRUE(resolve_ok(service, R"({"delete_existing": 1})").delete_existing);
  EXPECT_FALSE(resolve_ok(service, R"({"delete_existing": "nope"})").delete_existing);
  EXPECT_FALSE(resolve_ok(service, R"({"delete_existing": true})", {{"NATS_DELETE_EXISTING", "0"}})
                   .delete_existing);
}

TEST(config, nats_errors) {
  const auto service = parse_ok(R"({"products": ["A"]})");
  auto error = [&](const std::optional<std::string>& file,
                   std::map<std::string, std::string> env = {}) {
    auto config = cratchet::resolve_nats_config(service, file, fake_env(std::move(env)));
    EXPECT_FALSE(config);
    return config ? std::string() : config.error();
  };
  EXPECT_NE(error("{oops").find("invalid JSON"), std::string::npos);
  EXPECT_NE(error(R"({"retention_minutes": 0})").find("JS_RETENTION_MINUTES"), std::string::npos);
  EXPECT_NE(error(std::nullopt, {{"JS_RETENTION_MINUTES", "ten"}}).find("not an integer"),
            std::string::npos);
  EXPECT_NE(error(std::nullopt, {{"JS_MAX_AGE_SECONDS", "-1"}}).find("JS_MAX_AGE_SECONDS"),
            std::string::npos);
  EXPECT_NE(error(R"({"urls": 4222})").find("urls"), std::string::npos);

  auto dotted = parse_ok(R"({"products": ["A"], "stream_name": "market.ticks"})");
  auto config = cratchet::resolve_nats_config(dotted, std::nullopt, fake_env({}));
  ASSERT_FALSE(config);
  EXPECT_NE(config.error().find("cannot contain '.'"), std::string::npos);
}

TEST(config, load_from_files) {
  const auto dir = std::filesystem::temp_directory_path() / "cratchet_config_test";
  std::filesystem::create_directories(dir);
  const auto ingestor_path = (dir / "config.json").string();
  std::ofstream(ingestor_path) << ledgerflux_ingestor_json;

  auto service = cratchet::load_ingestor_config(ingestor_path);
  ASSERT_TRUE(service) << service.error();
  EXPECT_EQ(service.value().products.size(), 3U);

  // A missing nats config falls back to defaults, as in LedgerFlux.
  auto nats =
      cratchet::load_nats_config((dir / "missing.json").string(), service.value(), fake_env({}));
  ASSERT_TRUE(nats) << nats.error();
  EXPECT_EQ(nats.value().urls, "nats://localhost:4222");

  EXPECT_FALSE(cratchet::load_ingestor_config((dir / "missing.json").string()));
  std::filesystem::remove_all(dir);
}

}  // namespace
