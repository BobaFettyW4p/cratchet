find_package(benchmark CONFIG REQUIRED)

add_executable(cratchet_bench
  feed_bench.cpp
  result_bench.cpp)

target_compile_definitions(cratchet_bench PRIVATE
  CRATCHET_BENCH_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")

target_link_libraries(cratchet_bench PRIVATE
  cratchet::core
  benchmark::benchmark
  benchmark::benchmark_main)
