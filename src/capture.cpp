#include "cratchet/capture.hpp"

#include <algorithm>
#include <stdexcept>

namespace cratchet {

capture_reader::capture_reader(const std::string& path) : in_(path), path_(path) {
  if (!in_) {
    throw std::runtime_error("cannot open capture " + path);
  }
}

std::optional<capture_frame> capture_reader::next() {
  if (!std::getline(in_, line_)) {
    return std::nullopt;
  }
  ++line_number_;

  buffer_.resize(line_.size() + simdjson::SIMDJSON_PADDING);
  std::copy(line_.begin(), line_.end(), buffer_.begin());
  const simdjson::padded_string_view padded(buffer_.data(), line_.size(), buffer_.size());

  capture_frame frame;
  simdjson::ondemand::document doc;
  simdjson::ondemand::object object;
  if (json_.iterate(padded).get(doc) != simdjson::SUCCESS ||
      doc.get_object().get(object) != simdjson::SUCCESS ||
      object.find_field_unordered("recv_ns").get_int64().get(frame.recv_ns) != simdjson::SUCCESS ||
      object.find_field_unordered("msg").get_string().get(frame.msg) != simdjson::SUCCESS) {
    throw std::runtime_error(path_ + ":" + std::to_string(line_number_) +
                             R"x(: not a capture line ({"recv_ns": int, "msg": string}))x");
  }
  return frame;
}

}  // namespace cratchet
