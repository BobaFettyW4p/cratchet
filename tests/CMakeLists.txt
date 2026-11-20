find_package(GTest CONFIG REQUIRED)

add_executable(cratchet_tests
  capture_test.cpp
  coinbase_parser_test.cpp
  config_test.cpp
  hello_test.cpp
  json_test.cpp
  normalizer_test.cpp
  replay_test.cpp
  result_test.cpp
  shard_test.cpp
  timestamp_test.cpp)

target_compile_definitions(cratchet_tests PRIVATE
  CRATCHET_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

target_link_libraries(cratchet_tests PRIVATE
  cratchet::core
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cratchet_tests)
