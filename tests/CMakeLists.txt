find_package(GTest CONFIG REQUIRED)

add_executable(cratchet_tests
  hello_test.cpp
  result_test.cpp)

target_link_libraries(cratchet_tests PRIVATE
  cratchet::core
  GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(cratchet_tests)
