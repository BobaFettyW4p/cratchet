add_library(cratchet_core STATIC
  version.cpp)

add_library(cratchet::core ALIAS cratchet_core)

target_include_directories(cratchet_core PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)

target_compile_definitions(cratchet_core PRIVATE
  CRATCHET_VERSION="${PROJECT_VERSION}")

target_link_libraries(cratchet_core PUBLIC cratchet_options)
