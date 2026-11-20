#include "cratchet/nats_publisher.hpp"

#include <nats/nats.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <thread>
#include <utility>

#include "cratchet/log.hpp"

namespace cratchet {

namespace {

std::string status_text(natsStatus status) {
  std::string text = natsStatus_GetText(status);
  natsStatus last = NATS_OK;
  const char* detail = nats_GetLastError(&last);
  if (detail != nullptr && *detail != '\0' && detail != text) {
    text += ": ";
    text += detail;
  }
  return text;
}

bool should_log(std::uint64_t count) { return count <= 10 || count % 1000 == 0; }

}  // namespace

class nats_publisher::impl {
 public:
  impl(nats_config config, options opts) : config_(std::move(config)), opts_(opts) {}

  ~impl() {
    closing_ = true;  // destroying the connection fires the disconnected callback
    if (js_ != nullptr) {
      jsCtx_Destroy(js_);
    }
    if (connection_ != nullptr) {
      natsConnection_Destroy(connection_);  // closes the connection
    }
    if (nats_options_ != nullptr) {
      natsOptions_Destroy(nats_options_);
    }
  }

  impl(const impl&) = delete;
  impl& operator=(const impl&) = delete;
  impl(impl&&) = delete;
  impl& operator=(impl&&) = delete;

  std::optional<std::string> connect() {
    if (const auto status = configure_options(); status != NATS_OK) {
      return "invalid NATS options: " + status_text(status);
    }

    // LedgerFlux's NATSStreamManager.connect: retry with 1, 2, 4, 8 s... until the timeout.
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + opts_.connect_timeout;
    std::string last_error;
    for (int attempt = 1;; ++attempt) {
      const auto status = natsConnection_Connect(&connection_, nats_options_);
      if (status == NATS_OK) {
        break;
      }
      connection_ = nullptr;
      last_error = status_text(status);
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return "failed to connect to NATS at " + config_.urls + " after " +
               std::to_string(opts_.connect_timeout.count()) + "s: " + last_error;
      }
      const auto wait = std::min<std::chrono::steady_clock::duration>(
          std::chrono::seconds(1LL << std::min(attempt - 1, 30)), remaining);
      log::warn("NATS connection failed (attempt {}): {}; retrying in {} ms", attempt, last_error,
                std::chrono::duration_cast<std::chrono::milliseconds>(wait).count());
      std::this_thread::sleep_for(wait);
    }
    connected_ = true;
    log::info("connected to NATS at {}", config_.urls);

    jsOptions js_options;
    jsOptions_Init(&js_options);
    js_options.PublishAsync.MaxPending = opts_.max_pending_acks;
    js_options.PublishAsync.StallWait = opts_.stall_wait.count();
    js_options.PublishAsync.AckHandler = &impl::on_ack;
    js_options.PublishAsync.AckHandlerClosure = this;
    if (const auto status = natsConnection_JetStream(&js_, connection_, &js_options);
        status != NATS_OK) {
      return "cannot create JetStream context: " + status_text(status);
    }
    jsPubOptions_Init(&publish_options_);
    publish_options_.MaxWait = opts_.ack_timeout.count();
    return ensure_stream();
  }

  bool publish(std::string_view subject, std::string_view payload) {
    submitted_.fetch_add(1, std::memory_order_relaxed);
    if (payload.size() > static_cast<std::size_t>(INT_MAX)) {
      record_failure("payload too large");
      return false;
    }
    subject_.assign(subject);  // the C API needs a NUL-terminated subject
    const auto status = js_PublishAsync(js_, subject_.c_str(), payload.data(),
                                        static_cast<int>(payload.size()), &publish_options_);
    if (status != NATS_OK) {
      record_failure(status_text(status));
      return false;
    }
    return true;
  }

  bool flush(std::chrono::milliseconds timeout) {
    jsPubOptions options;
    jsPubOptions_Init(&options);
    options.MaxWait = std::max<std::int64_t>(1, timeout.count());
    return js_PublishAsyncComplete(js_, &options) == NATS_OK;
  }

  [[nodiscard]] bool connected() const noexcept { return connected_.load(); }

  [[nodiscard]] publisher_stats stats() const noexcept {
    return {.submitted = submitted_.load(),
            .acked = acked_.load(),
            .failed = failed_.load(),
            .disconnects = disconnects_.load(),
            .reconnects = reconnects_.load()};
  }

  [[nodiscard]] const nats_config& config() const noexcept { return config_; }

 private:
  natsStatus configure_options() {
    natsStatus status = natsOptions_Create(&nats_options_);
    if (status == NATS_OK) {
      status = natsOptions_SetURL(nats_options_, config_.urls.c_str());
    }
    if (status == NATS_OK) {
      status = natsOptions_SetTimeout(nats_options_, opts_.attempt_timeout.count());
    }
    if (status == NATS_OK) {
      // After the initial connect, ride out server restarts: reconnect forever, buffering
      // publishes in the meantime.
      status = natsOptions_SetMaxReconnect(nats_options_, -1);
    }
    if (status == NATS_OK) {
      status = natsOptions_SetDisconnectedCB(nats_options_, &impl::on_disconnected, this);
    }
    if (status == NATS_OK) {
      status = natsOptions_SetReconnectedCB(nats_options_, &impl::on_reconnected, this);
    }
    return status;
  }

  // LedgerFlux: use the stream if stream_info succeeds; otherwise optionally delete it, then
  // create it with subjects ["<prefix>.*"] and default limits.
  std::optional<std::string> ensure_stream() {
    jsStreamInfo* info = nullptr;
    jsErrCode js_error{};
    if (js_GetStreamInfo(&info, js_, config_.stream_name.c_str(), nullptr, &js_error) == NATS_OK) {
      jsStreamInfo_Destroy(info);
      log::info("stream already exists: {}", config_.stream_name);
      return std::nullopt;
    }
    if (config_.delete_existing) {
      if (js_DeleteStream(js_, config_.stream_name.c_str(), nullptr, &js_error) == NATS_OK) {
        log::info("deleted existing stream: {}", config_.stream_name);
      }
    }

    const std::string subjects = config_.subject_prefix + ".*";
    std::array<const char*, 1> subject_list{subjects.c_str()};
    jsStreamConfig stream_config;
    jsStreamConfig_Init(&stream_config);
    stream_config.Name = config_.stream_name.c_str();
    stream_config.Subjects = subject_list.data();
    stream_config.SubjectsLen = 1;
    info = nullptr;
    const auto status = js_AddStream(&info, js_, &stream_config, nullptr, &js_error);
    if (status == NATS_OK) {
      jsStreamInfo_Destroy(info);
      log::info("created stream {} with subjects {}", config_.stream_name, subjects);
      return std::nullopt;
    }
    const std::string text = status_text(status);
    if (text.find("already in use") != std::string::npos) {
      log::info("stream already exists: {}", config_.stream_name);
      return std::nullopt;
    }
    return "failed to create stream '" + config_.stream_name + "': " + text;
  }

  void record_failure(std::string_view why) {
    const auto count = failed_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (should_log(count)) {
      log::warn("publish failed (#{}): {}", count, why);
    }
  }

  static void on_ack(jsCtx* /*js*/, natsMsg* msg, jsPubAck* ack, jsPubAckErr* ack_error,
                     void* closure) {
    auto* self = static_cast<impl*>(closure);
    if (ack != nullptr) {
      self->acked_.fetch_add(1, std::memory_order_relaxed);
    } else {
      std::string why = ack_error != nullptr ? natsStatus_GetText(ack_error->Err) : "no ack";
      if (ack_error != nullptr && ack_error->ErrText != nullptr) {
        why += std::string(": ") + ack_error->ErrText;
      }
      self->record_failure(why);
    }
    natsMsg_Destroy(msg);
  }

  static void on_disconnected(natsConnection* /*connection*/, void* closure) {
    auto* self = static_cast<impl*>(closure);
    self->connected_ = false;
    if (self->closing_) {
      return;
    }
    self->disconnects_.fetch_add(1, std::memory_order_relaxed);
    log::warn("disconnected from NATS; reconnecting");
  }

  static void on_reconnected(natsConnection* /*connection*/, void* closure) {
    auto* self = static_cast<impl*>(closure);
    self->connected_ = true;
    self->reconnects_.fetch_add(1, std::memory_order_relaxed);
    log::info("reconnected to NATS");
  }

  nats_config config_;
  options opts_;
  natsOptions* nats_options_ = nullptr;
  natsConnection* connection_ = nullptr;
  jsCtx* js_ = nullptr;
  jsPubOptions publish_options_{};
  std::string subject_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> closing_{false};
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> acked_{0};
  std::atomic<std::uint64_t> failed_{0};
  std::atomic<std::uint64_t> disconnects_{0};
  std::atomic<std::uint64_t> reconnects_{0};
};

nats_publisher::nats_publisher(std::unique_ptr<impl> state) : impl_(std::move(state)) {}

nats_publisher::~nats_publisher() = default;

result<std::unique_ptr<nats_publisher>, std::string> nats_publisher::connect(
    const nats_config& config, options opts) {
  using connect_result = result<std::unique_ptr<nats_publisher>, std::string>;
  auto state = std::make_unique<impl>(config, opts);
  if (auto error = state->connect()) {
    return connect_result::fail(std::move(*error));
  }
  return connect_result::ok(std::unique_ptr<nats_publisher>(new nats_publisher(std::move(state))));
}

bool nats_publisher::publish(std::string_view subject, std::string_view payload) {
  return impl_->publish(subject, payload);
}

bool nats_publisher::flush(std::chrono::milliseconds timeout) { return impl_->flush(timeout); }

bool nats_publisher::connected() const noexcept { return impl_->connected(); }

publisher_stats nats_publisher::stats() const noexcept { return impl_->stats(); }

const nats_config& nats_publisher::config() const noexcept { return impl_->config(); }

void nats_publisher::shutdown_library() { nats_CloseAndWait(2000); }

}  // namespace cratchet
