#pragma once

#include <cstddef>
#include <utility>
#include <variant>

namespace cratchet {

// Minimal stand-in for C++23 std::expected<T, E>: holds either a value or an error.
// Built on std::variant with index-based construction, so T and E may be the same type.
// Accessing the wrong alternative throws std::bad_variant_access.
template <typename T, typename E>
class result {
 public:
  using value_type = T;
  using error_type = E;

  static result ok(T value) { return result(std::in_place_index<0>, std::move(value)); }
  static result fail(E error) { return result(std::in_place_index<1>, std::move(error)); }

  [[nodiscard]] bool has_value() const noexcept { return data_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(data_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(data_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(data_)); }

  [[nodiscard]] E& error() & { return std::get<1>(data_); }
  [[nodiscard]] const E& error() const& { return std::get<1>(data_); }
  [[nodiscard]] E&& error() && { return std::get<1>(std::move(data_)); }

  template <typename U>
  [[nodiscard]] T value_or(U&& fallback) const& {
    return has_value() ? value() : static_cast<T>(std::forward<U>(fallback));
  }

 private:
  template <std::size_t I, typename U>
  result(std::in_place_index_t<I> index, U&& payload) : data_(index, std::forward<U>(payload)) {}

  std::variant<T, E> data_;
};

}  // namespace cratchet
