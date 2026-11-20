#pragma once

#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace cratchet {

// One recorded ws-feed frame.
struct capture_frame {
  std::int64_t recv_ns{};  // wall clock at receive, ns since the Unix epoch
  std::string_view msg;    // the raw frame text; valid until the next call to next()
};

// Reads feed captures: JSON Lines of {"recv_ns": <int>, "msg": "<raw frame>"}, the format
// scripts/capture_coinbase.py writes (docs/contract.md §9).
class capture_reader {
 public:
  // Throws std::runtime_error if the file cannot be opened.
  explicit capture_reader(const std::string& path);

  // The next frame, or nullopt at end of file. Throws std::runtime_error on a malformed line.
  std::optional<capture_frame> next();

  // 1-based number of the line most recently returned.
  [[nodiscard]] std::size_t line_number() const noexcept { return line_number_; }

 private:
  std::ifstream in_;
  std::string path_;
  std::string line_;
  std::string buffer_;  // line copy with simdjson padding
  simdjson::ondemand::parser json_;
  std::size_t line_number_ = 0;
};

// Writes feed captures in the same format as capture_reader reads and
// scripts/capture_coinbase.py writes: {"recv_ns": <int>, "msg": "<raw frame>"} per line.
// Buffered; call flush() to push complete lines to disk (also done on destruction).
class capture_writer {
 public:
  // Appends to `path`, creating it if needed. Throws std::runtime_error if it cannot be opened.
  explicit capture_writer(const std::string& path);

  // Throws std::runtime_error if the write fails (e.g. disk full).
  void write(std::int64_t recv_ns, std::string_view msg);
  void flush();

  [[nodiscard]] std::size_t frames_written() const noexcept { return frames_; }

 private:
  std::ofstream out_;
  std::string path_;
  std::string line_;
  std::size_t frames_ = 0;
};

}  // namespace cratchet
