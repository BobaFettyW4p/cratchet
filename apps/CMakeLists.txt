find_package(boost_asio CONFIG REQUIRED)

add_executable(cratchet cratchet.cpp)
target_link_libraries(cratchet PRIVATE cratchet::core Boost::asio)
