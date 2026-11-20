find_package(OpenSSL REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(boost_asio CONFIG REQUIRED)
find_package(boost_beast CONFIG REQUIRED)
find_package(cnats CONFIG REQUIRED)

add_library(cratchet_core STATIC
  backoff.cpp
  capture.cpp
  coinbase/parser.cpp
  coinbase/subscribe.cpp
  config.cpp
  feed_client.cpp
  json.cpp
  log.cpp
  nats_publisher.cpp
  normalizer.cpp
  pipeline.cpp
  shard.cpp
  tick.cpp
  timestamp.cpp
  version.cpp)

add_library(cratchet::core ALIAS cratchet_core)

target_include_directories(cratchet_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_compile_definitions(cratchet_core
  PRIVATE CRATCHET_VERSION="${PROJECT_VERSION}"
  PUBLIC BOOST_ASIO_NO_DEPRECATED)

target_link_libraries(cratchet_core
  PUBLIC cratchet_options simdjson::simdjson
  PRIVATE Boost::asio Boost::beast cnats::nats_static OpenSSL::SSL OpenSSL::Crypto)
