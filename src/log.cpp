#include "cratchet/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace cratchet::log {

namespace {

std::atomic<level>& min_level() {
  static std::atomic<level> value{level::info};
  return value;
}

std::mutex& write_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::string_view name(level lvl) {
  switch (lvl) {
    case level::debug:
      return "debug";
    case level::info:
      return "info";
    case level::warn:
      return "warn";
    case level::error:
      return "error";
  }
  return "?";
}

}  // namespace

void set_min_level(level min) noexcept { min_level().store(min, std::memory_order_relaxed); }

void write(level lvl, std::string_view message) {
  if (lvl < min_level().load(std::memory_order_relaxed)) {
    return;
  }
  const auto now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
  const std::string line = std::format("{:%FT%T}Z {} {}\n", now, name(lvl), message);
  const std::lock_guard lock(write_mutex());
  std::fwrite(line.data(), 1, line.size(), stderr);
}

}  // namespace cratchet::log
