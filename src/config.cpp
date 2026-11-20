#include "cratchet/config.hpp"

#include <simdjson.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace cratchet {

namespace {

namespace dom = simdjson::dom;

std::string_view trim(std::string_view text) {
  const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

std::string upper(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  });
  return out;
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return std::move(contents).str();
}

// The parsed top-level object, or an error message.
result<dom::object, std::string> parse_object(dom::parser& parser, std::string_view json,
                                              std::string_view what) {
  dom::element root;
  if (const auto code = parser.parse(json.data(), json.size()).get(root);
      code != simdjson::SUCCESS) {
    return result<dom::object, std::string>::fail(std::string(what) + ": invalid JSON (" +
                                                  simdjson::error_message(code) + ")");
  }
  dom::object object;
  if (root.get_object().get(object) != simdjson::SUCCESS) {
    return result<dom::object, std::string>::fail(std::string(what) + ": not a JSON object");
  }
  return result<dom::object, std::string>::ok(object);
}

std::string type_error(std::string_view key, std::string_view expected) {
  return "\"" + std::string(key) + "\" must be " + std::string(expected);
}

// Reads an optional string field into `out`; returns an error message on a type mismatch.
std::optional<std::string> read_string(const dom::object& object, std::string_view key,
                                       std::string& out) {
  dom::element value;
  if (object.at_key(key).get(value) != simdjson::SUCCESS) {
    return std::nullopt;  // absent: keep the default
  }
  std::string_view text;
  if (value.get_string().get(text) != simdjson::SUCCESS) {
    return type_error(key, "a string");
  }
  out = text;
  return std::nullopt;
}

std::optional<std::string> read_string_list(const dom::object& object, std::string_view key,
                                            std::vector<std::string>& out, bool to_upper) {
  dom::element value;
  if (object.at_key(key).get(value) != simdjson::SUCCESS) {
    return std::nullopt;
  }
  dom::array array;
  if (value.get_array().get(array) != simdjson::SUCCESS) {
    return type_error(key, "an array of strings");
  }
  out.clear();
  for (const dom::element element : array) {
    std::string_view text;
    if (element.get_string().get(text) != simdjson::SUCCESS) {
      return type_error(key, "an array of strings");
    }
    const auto trimmed = trim(text);
    out.push_back(to_upper ? upper(trimmed) : std::string(trimmed));
  }
  return std::nullopt;
}

std::optional<std::string> read_int(const dom::object& object, std::string_view key,
                                    std::optional<std::int64_t>& out) {
  dom::element value;
  if (object.at_key(key).get(value) != simdjson::SUCCESS) {
    return std::nullopt;
  }
  std::int64_t number = 0;
  if (value.get_int64().get(number) != simdjson::SUCCESS) {
    return type_error(key, "an integer");
  }
  out = number;
  return std::nullopt;
}

// Python's int() on an environment string: optional surrounding whitespace and sign.
std::optional<std::int64_t> parse_env_int(std::string_view text) {
  text = trim(text);
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
  }
  std::int64_t value = 0;
  const char* const last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), last, value);
  if (text.empty() || ec != std::errc{} || ptr != last) {
    return std::nullopt;
  }
  return value;
}

// LedgerFlux's _parse_bool for strings.
bool parse_env_bool(std::string_view text) {
  static const std::set<std::string, std::less<>> truthy{"1", "true", "yes", "y", "on"};
  std::string lowered(trim(text));
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return truthy.contains(lowered);
}

}  // namespace

env_lookup process_env() {
  return [](const std::string& name) -> std::optional<std::string> {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read at startup, before any threads exist
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
      return std::nullopt;
    }
    return std::string(value);
  };
}

result<ingestor_config, std::string> parse_ingestor_config(std::string_view json) {
  using config_result = result<ingestor_config, std::string>;
  dom::parser parser;
  auto parsed = parse_object(parser, json, "ingestor config");
  if (!parsed) {
    return config_result::fail(parsed.error());
  }
  const dom::object& object = parsed.value();

  ingestor_config config;
  std::optional<std::int64_t> num_shards;
  std::optional<std::int64_t> health_port;
  for (auto error : {
           read_string_list(object, "products", config.products, true),
           read_string_list(object, "channels", config.channels, false),
           read_string(object, "ws_uri", config.ws_uri),
           read_int(object, "num_shards", num_shards),
           read_string(object, "stream_name", config.stream_name),
           read_string(object, "subject_prefix", config.subject_prefix),
           read_int(object, "health_port", health_port),
       }) {
    if (error) {
      return config_result::fail("ingestor config: " + *error);
    }
  }

  if (config.products.empty() || std::any_of(config.products.begin(), config.products.end(),
                                             [](const std::string& p) { return p.empty(); })) {
    return config_result::fail("ingestor config: \"products\" must list at least one product");
  }
  if (num_shards) {
    if (*num_shards <= 0 || *num_shards > std::numeric_limits<std::uint32_t>::max()) {
      return config_result::fail("ingestor config: \"num_shards\" must be positive");
    }
    config.num_shards = static_cast<std::uint32_t>(*num_shards);
  }
  if (health_port) {
    if (*health_port <= 0 || *health_port > std::numeric_limits<std::uint16_t>::max()) {
      return config_result::fail("ingestor config: \"health_port\" must be 1-65535");
    }
    config.health_port = static_cast<std::uint16_t>(*health_port);
  }
  return config_result::ok(std::move(config));
}

result<ingestor_config, std::string> load_ingestor_config(const std::string& path) {
  const auto contents = read_file(path);
  if (!contents) {
    return result<ingestor_config, std::string>::fail("cannot read ingestor config " + path);
  }
  auto config = parse_ingestor_config(*contents);
  if (!config) {
    return result<ingestor_config, std::string>::fail(path + ": " + config.error());
  }
  return config;
}

result<nats_config, std::string> resolve_nats_config(const ingestor_config& service,
                                                     const std::optional<std::string>& file_json,
                                                     const env_lookup& env) {
  using config_result = result<nats_config, std::string>;

  // Values from the file, if present.
  std::optional<std::string> file_urls;
  std::optional<std::string> file_stream;
  std::optional<std::string> file_prefix;
  std::optional<std::int64_t> file_retention;
  std::optional<std::int64_t> file_max_age;
  std::optional<bool> file_delete;
  dom::parser parser;
  if (file_json) {
    auto parsed = parse_object(parser, *file_json, "nats config");
    if (!parsed) {
      return config_result::fail(parsed.error());
    }
    const dom::object& object = parsed.value();
    std::string text;
    auto read_optional_string = [&](std::string_view key,
                                    std::optional<std::string>& out) -> std::optional<std::string> {
      dom::element value;
      if (object.at_key(key).get(value) != simdjson::SUCCESS) {
        return std::nullopt;
      }
      if (auto error = read_string(object, key, text)) {
        return error;
      }
      out = text;
      return std::nullopt;
    };
    for (auto error :
         {read_optional_string("urls", file_urls), read_optional_string("stream_name", file_stream),
          read_optional_string("subject_prefix", file_prefix),
          read_int(object, "retention_minutes", file_retention),
          read_int(object, "max_age_seconds", file_max_age)}) {
      if (error) {
        return config_result::fail("nats config: " + *error);
      }
    }
    dom::element value;
    if (object.at_key("delete_existing").get(value) == simdjson::SUCCESS) {
      bool flag = false;
      std::string_view flag_text;
      std::int64_t flag_number = 0;
      if (value.get_bool().get(flag) == simdjson::SUCCESS) {
        file_delete = flag;
      } else if (value.get_string().get(flag_text) == simdjson::SUCCESS) {
        file_delete = parse_env_bool(flag_text);
      } else if (value.get_int64().get(flag_number) == simdjson::SUCCESS) {
        file_delete = flag_number != 0;
      } else {
        return config_result::fail("nats config: " + type_error("delete_existing", "a boolean"));
      }
    }
  }

  nats_config config;
  config.urls = env("NATS_URLS").value_or(file_urls.value_or(config.urls));

  if (const auto text = env("JS_RETENTION_MINUTES")) {
    const auto value = parse_env_int(*text);
    if (!value) {
      return config_result::fail("JS_RETENTION_MINUTES is not an integer: " + *text);
    }
    config.retention_minutes = *value;
  } else if (file_retention) {
    config.retention_minutes = *file_retention;
  }
  if (config.retention_minutes <= 0) {
    return config_result::fail("JS_RETENTION_MINUTES must be positive");
  }

  config.max_age_seconds = config.retention_minutes * 60;
  if (const auto text = env("JS_MAX_AGE_SECONDS")) {
    const auto value = parse_env_int(*text);
    if (!value) {
      return config_result::fail("JS_MAX_AGE_SECONDS is not an integer: " + *text);
    }
    config.max_age_seconds = *value;
  } else if (file_max_age) {
    config.max_age_seconds = *file_max_age;
  }
  if (config.max_age_seconds <= 0) {
    return config_result::fail("JS_MAX_AGE_SECONDS must be positive");
  }

  // Python: explicit or env or file or default, where empty strings are falsy.
  auto first_non_empty = [](std::initializer_list<std::optional<std::string>> candidates,
                            const std::string& fallback) {
    for (const auto& candidate : candidates) {
      if (candidate && !candidate->empty()) {
        return *candidate;
      }
    }
    return fallback;
  };
  config.stream_name =
      first_non_empty({service.stream_name, env("JS_STREAM_NAME"), file_stream}, "market_ticks");
  if (config.stream_name.find('.') != std::string::npos) {
    return config_result::fail("invalid JetStream name '" + config.stream_name +
                               "': stream names cannot contain '.'");
  }
  config.subject_prefix = first_non_empty(
      {service.subject_prefix, env("JS_SUBJECT_PREFIX"), file_prefix}, "market.ticks");

  if (const auto text = env("NATS_DELETE_EXISTING")) {
    config.delete_existing = parse_env_bool(*text);
  } else {
    config.delete_existing = file_delete.value_or(false);
  }
  return config_result::ok(std::move(config));
}

result<nats_config, std::string> load_nats_config(const std::string& path,
                                                  const ingestor_config& service,
                                                  const env_lookup& env) {
  // A missing file is fine (defaults apply); an unreadable or invalid one is not.
  std::error_code ec;
  const bool exists = std::filesystem::exists(path, ec);
  std::optional<std::string> contents;
  if (exists) {
    contents = read_file(path);
    if (!contents) {
      return result<nats_config, std::string>::fail("cannot read nats config " + path);
    }
  }
  auto config = resolve_nats_config(service, contents, env);
  if (!config) {
    return result<nats_config, std::string>::fail(path + ": " + config.error());
  }
  return config;
}

}  // namespace cratchet
