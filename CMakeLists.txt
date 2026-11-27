cmake_minimum_required(VERSION 3.25)

project(cratchet
  VERSION 0.1.0
  DESCRIPTION "C++20 market data core: feed handler and order book engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CRATCHET_BUILD_TESTS "Build unit tests" ON)
option(CRATCHET_BUILD_BENCH "Build benchmarks" ON)
option(CRATCHET_BUILD_FUZZ "Build libFuzzer targets (needs Clang; see the fuzz preset)" OFF)
option(CRATCHET_WERROR "Treat compiler warnings as errors" OFF)
set(CRATCHET_SANITIZERS "" CACHE STRING
    "Comma-separated sanitizers to enable, e.g. address,undefined or thread")

# Deliberately no -march=native: dev runs on aarch64, CI and benchmarks on x86_64.

# Interface target carrying warnings and sanitizer flags for every cratchet target.
add_library(cratchet_options INTERFACE)
target_compile_options(cratchet_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(CRATCHET_WERROR)
  target_compile_options(cratchet_options INTERFACE -Werror)
endif()
if(CRATCHET_SANITIZERS)
  target_compile_options(cratchet_options INTERFACE
    -fsanitize=${CRATCHET_SANITIZERS} -fno-omit-frame-pointer)
  target_link_options(cratchet_options INTERFACE -fsanitize=${CRATCHET_SANITIZERS})
  if(CRATCHET_SANITIZERS MATCHES "undefined")
    target_compile_options(cratchet_options INTERFACE -fno-sanitize-recover=undefined)
  endif()
endif()

add_subdirectory(src)
add_subdirectory(apps)

if(CRATCHET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CRATCHET_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(CRATCHET_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
