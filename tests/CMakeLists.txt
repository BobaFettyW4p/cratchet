find_package(GTest CONFIG REQUIRED)
find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(cnats CONFIG REQUIRED)

add_executable(cratchet_tests
  capture_test.cpp
  coinbase_parser_test.cpp
  config_test.cpp
  feed_client_test.cpp
  feed_pieces_test.cpp
  hello_test.cpp
  json_test.cpp
  nats_publisher_test.cpp
  normalizer_test.cpp
  pipeline_test.cpp
  replay_test.cpp
  result_test.cpp
  shard_test.cpp
  timestamp_test.cpp)

target_compile_definitions(cratchet_tests PRIVATE
  CRATCHET_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

target_link_libraries(cratchet_tests PRIVATE
  cratchet::core
  Boost::asio
  Boost::beast
  cnats::nats_static
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cratchet_tests)
