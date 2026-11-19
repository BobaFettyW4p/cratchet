#pragma once

#include <string_view>

namespace cratchet {

// Semantic version of the library, taken from the CMake project version.
std::string_view version() noexcept;

}  // namespace cratchet
