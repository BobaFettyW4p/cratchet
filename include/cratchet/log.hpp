#pragma once

#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

// Minimal line logger to stderr: "<UTC timestamp> <level> <message>". Thread-safe per line.
namespace cratchet::log {

enum class level : std::uint8_t { debug, info, warn, error };

void set_min_level(level min) noexcept;
void write(level lvl, std::string_view message);

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
  write(level::info, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
  write(level::warn, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
  write(level::error, std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
  write(level::debug, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace cratchet::log
