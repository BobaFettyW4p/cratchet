#include "cratchet/coinbase/subscribe.hpp"

#include "cratchet/json.hpp"

namespace cratchet::coinbase {

namespace {

void append_list(std::string& out, const std::vector<std::string>& items) {
  out += '[';
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) {
      out += ',';
    }
    json::append_string(out, items[i]);
  }
  out += ']';
}

}  // namespace

std::string subscribe_message(const std::vector<std::string>& products,
                              const std::vector<std::string>& channels) {
  std::string out = R"({"type":"subscribe","product_ids":)";
  append_list(out, products);
  out += R"(,"channels":)";
  append_list(out, channels);
  out += '}';
  return out;
}

}  // namespace cratchet::coinbase
