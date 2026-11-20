#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "cratchet/capture.hpp"
#include "cratchet/coinbase/parser.hpp"
#include "cratchet/normalizer.hpp"
#include "cratchet/version.hpp"

namespace {

// LedgerFlux defaults (docs/contract.md §6). Configurable once config loading lands.
constexpr std::uint32_t num_shards = 4;
constexpr std::string_view subject_prefix = "market.ticks";

void usage(std::ostream& out) {
  out << "usage: cratchet normalize <capture.jsonl>\n"
         "       cratchet --version\n"
         "\n"
         "normalize  Run a recorded feed through the parser and normalizer. Prints one line\n"
         "           per tick that would be published: <subject><TAB><Tick JSON>.\n"
         "           A summary goes to stderr.\n";
}

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int normalize(const std::string& path) {
  cratchet::capture_reader capture(path);
  cratchet::coinbase::parser parser;
  cratchet::normalizer normalizer(num_shards);

  std::size_t frames = 0;
  std::size_t published = 0;
  std::size_t out_of_order = 0;
  std::map<std::string, std::size_t, std::less<>> parse_errors;
  std::map<std::string, std::size_t, std::less<>> drops;

  while (const auto frame = capture.next()) {
    ++frames;
    auto parsed = parser.parse(frame->msg);
    if (!parsed) {
      ++parse_errors[std::string(to_string(parsed.error().code))];
      continue;
    }
    const auto* msg = std::get_if<cratchet::coinbase::ticker>(&parsed.value());
    if (msg == nullptr) {
      continue;
    }
    // ts_ingest is taken after parsing, as LedgerFlux defines it (docs/contract.md §4).
    auto out = normalizer.normalize(*msg, now_ns());
    if (!out) {
      ++drops[std::string(to_string(out.error()))];
      continue;
    }
    ++published;
    if (out.value().out_of_order) {
      ++out_of_order;
    }
    std::cout << subject_prefix << '.' << out.value().shard << '\t' << out.value().json << '\n';
  }

  std::cerr << "frames=" << frames << " published=" << published
            << " out_of_order=" << out_of_order;
  for (const auto& [code, count] : parse_errors) {
    std::cerr << " parse_error." << code << '=' << count;
  }
  for (const auto& [reason, count] : drops) {
    std::cerr << " dropped." << reason << '=' << count;
  }
  std::cerr << '\n';
  return std::cout ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  try {
    if (args.size() == 1 && args[0] == "--version") {
      std::cout << "cratchet " << cratchet::version() << '\n';
      return 0;
    }
    if (args.size() == 2 && args[0] == "normalize") {
      return normalize(std::string(args[1]));
    }
    usage(std::cerr);
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "cratchet: " << e.what() << '\n';
    return 1;
  }
}
