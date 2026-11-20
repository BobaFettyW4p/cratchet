// Integration tests against a real NATS server with JetStream. Set CRATCHET_NATS_URL
// (e.g. nats://localhost:4222) to run them; otherwise they are skipped.

#include "cratchet/nats_publisher.hpp"

#include <gtest/gtest.h>
#include <nats/nats.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

#include "cratchet/log.hpp"

namespace {

using namespace std::chrono_literals;

std::optional<std::string> nats_url() {
  const char* url = std::getenv("CRATCHET_NATS_URL");
  if (url == nullptr || *url == '\0') {
    return std::nullopt;
  }
  return std::string(url);
}

// A stream name and subject prefix no other test run uses.
cratchet::nats_config unique_config(const std::string& url, const std::string& tag) {
  const std::string id = std::to_string(::getpid()) + "_" + tag;
  cratchet::nats_config config;
  config.urls = url;
  config.stream_name = "cratchet_test_" + id;
  config.subject_prefix = "cratchet.test." + id;
  return config;
}

// Reads stream state with a separate raw connection, and deletes the stream afterwards.
class stream_probe {
 public:
  explicit stream_probe(const std::string& url) {
    EXPECT_EQ(natsConnection_ConnectTo(&connection_, url.c_str()), NATS_OK);
    EXPECT_EQ(natsConnection_JetStream(&js_, connection_, nullptr), NATS_OK);
  }
  ~stream_probe() {
    jsCtx_Destroy(js_);
    natsConnection_Destroy(connection_);
  }
  stream_probe(const stream_probe&) = delete;
  stream_probe& operator=(const stream_probe&) = delete;
  stream_probe(stream_probe&&) = delete;
  stream_probe& operator=(stream_probe&&) = delete;

  // Message count, or -1 if the stream does not exist.
  std::int64_t messages(const std::string& stream) {
    jsStreamInfo* info = nullptr;
    if (js_GetStreamInfo(&info, js_, stream.c_str(), nullptr, nullptr) != NATS_OK) {
      return -1;
    }
    const auto count = static_cast<std::int64_t>(info->State.Msgs);
    const std::string subjects = info->Config->SubjectsLen > 0 ? info->Config->Subjects[0] : "";
    last_subjects_ = subjects;
    jsStreamInfo_Destroy(info);
    return count;
  }

  [[nodiscard]] const std::string& last_subjects() const { return last_subjects_; }

  void remove(const std::string& stream) { js_DeleteStream(js_, stream.c_str(), nullptr, nullptr); }

 private:
  natsConnection* connection_ = nullptr;
  jsCtx* js_ = nullptr;
  std::string last_subjects_;
};

TEST(nats_publisher, creates_the_stream_and_publishes) {
  const auto url = nats_url();
  if (!url) {
    GTEST_SKIP() << "CRATCHET_NATS_URL not set";
  }
  cratchet::log::set_min_level(cratchet::log::level::error);
  const auto config = unique_config(*url, "publish");
  stream_probe probe(*url);
  probe.remove(config.stream_name);

  {
    auto publisher = cratchet::nats_publisher::connect(config, {.connect_timeout = 5s});
    ASSERT_TRUE(publisher) << publisher.error();
    EXPECT_TRUE(publisher.value()->connected());
    for (int i = 0; i < 25; ++i) {
      const std::string subject = config.subject_prefix + "." + std::to_string(i % 4);
      EXPECT_TRUE(publisher.value()->publish(subject, R"({"n":)" + std::to_string(i) + "}"));
    }
    ASSERT_TRUE(publisher.value()->flush(5s));
    const auto stats = publisher.value()->stats();
    EXPECT_EQ(stats.submitted, 25U);
    EXPECT_EQ(stats.acked, 25U);
    EXPECT_EQ(stats.failed, 0U);
  }

  EXPECT_EQ(probe.messages(config.stream_name), 25);
  EXPECT_EQ(probe.last_subjects(), config.subject_prefix + ".*");
  probe.remove(config.stream_name);
}

// Like LedgerFlux: if the stream already exists it is used as is, even with delete_existing;
// deletion only happens on the path where stream_info failed.
TEST(nats_publisher, reuses_an_existing_stream) {
  const auto url = nats_url();
  if (!url) {
    GTEST_SKIP() << "CRATCHET_NATS_URL not set";
  }
  cratchet::log::set_min_level(cratchet::log::level::error);
  auto config = unique_config(*url, "reuse");
  stream_probe probe(*url);
  probe.remove(config.stream_name);

  for (int run = 0; run < 2; ++run) {
    config.delete_existing = run == 1;
    auto publisher = cratchet::nats_publisher::connect(config, {.connect_timeout = 5s});
    ASSERT_TRUE(publisher) << publisher.error();
    EXPECT_TRUE(publisher.value()->publish(config.subject_prefix + ".0", "{}"));
    ASSERT_TRUE(publisher.value()->flush(5s));
  }
  EXPECT_EQ(probe.messages(config.stream_name), 2);
  probe.remove(config.stream_name);
}

TEST(nats_publisher, counts_messages_no_stream_accepts) {
  const auto url = nats_url();
  if (!url) {
    GTEST_SKIP() << "CRATCHET_NATS_URL not set";
  }
  cratchet::log::set_min_level(cratchet::log::level::error);
  const auto config = unique_config(*url, "orphan");
  stream_probe probe(*url);
  {
    auto publisher = cratchet::nats_publisher::connect(config, {.connect_timeout = 5s});
    ASSERT_TRUE(publisher) << publisher.error();
    (void)publisher.value()->publish("cratchet.nowhere." + config.stream_name, "{}");
    (void)publisher.value()->flush(5s);
    EXPECT_EQ(publisher.value()->stats().failed, 1U);
    EXPECT_EQ(publisher.value()->stats().acked, 0U);
  }
  probe.remove(config.stream_name);
}

TEST(nats_publisher, gives_up_after_the_connect_timeout) {
  cratchet::log::set_min_level(cratchet::log::level::error);
  cratchet::nats_config config;
  config.urls = "nats://127.0.0.1:1";  // nothing listens on port 1
  const auto started = std::chrono::steady_clock::now();
  auto publisher =
      cratchet::nats_publisher::connect(config, {.connect_timeout = 1s, .attempt_timeout = 200ms});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  ASSERT_FALSE(publisher);
  EXPECT_NE(publisher.error().find("failed to connect to NATS at nats://127.0.0.1:1"),
            std::string::npos)
      << publisher.error();
  EXPECT_GE(elapsed, 1s);
  EXPECT_LT(elapsed, 4s);
}

}  // namespace
