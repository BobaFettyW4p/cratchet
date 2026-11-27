# libFuzzer targets. Only built with -DCRATCHET_BUILD_FUZZ=ON (the "fuzz" preset), with Clang.
# The preset also builds cratchet_core with -fsanitize=address,undefined,fuzzer-no-link
# (via CRATCHET_SANITIZERS) so the library under test is coverage-instrumented.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "CRATCHET_BUILD_FUZZ needs Clang (libFuzzer), got ${CMAKE_CXX_COMPILER_ID}")
endif()

add_executable(coinbase_parser_fuzz coinbase_parser_fuzz.cpp)

# -fsanitize=fuzzer (the libFuzzer main and its instrumentation) is on this target only.
target_compile_options(coinbase_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(coinbase_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_libraries(coinbase_parser_fuzz PRIVATE cratchet::core)
