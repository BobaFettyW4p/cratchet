find_package(benchmark CONFIG REQUIRED)

add_executable(cratchet_bench
  result_bench.cpp)

target_link_libraries(cratchet_bench PRIVATE
  cratchet::core
  benchmark::benchmark
  benchmark::benchmark_main)
