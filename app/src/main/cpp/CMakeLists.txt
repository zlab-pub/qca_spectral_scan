cmake_minimum_required(VERSION 3.13)
project(spectral-plot)
include(ExternalProject)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wconversion -Wshadow -Wno-unused-parameter -Werror)

set(distribution_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../distribution")

ExternalProject_Add(hostap
  GIT_REPOSITORY git://w1.fi/hostap.git
  GIT_TAG hostap_2_11
  GIT_SHALLOW ON
  SOURCE_DIR "${distribution_DIR}/src/hostap"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  TEST_COMMAND ""
)
add_custom_command(
  TARGET hostap POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy
    "${distribution_DIR}/src/hostap/src/common/qca-vendor.h"
    "${distribution_DIR}/include/qca-vendor.h"
  BYPRODUCTS "${distribution_DIR}/include/qca-vendor.h"
  VERBATIM
)

add_library(libnl-3 STATIC IMPORTED)
set_target_properties(libnl-3 PROPERTIES
  IMPORTED_LOCATION "${distribution_DIR}/${ANDROID_ABI}/lib/libnl-3.a"
)
add_library(libnl-genl-3 STATIC IMPORTED)
set_target_properties(libnl-genl-3 PROPERTIES
  IMPORTED_LOCATION "${distribution_DIR}/${ANDROID_ABI}/lib/libnl-genl-3.a"
)

add_library(spectral-scan SHARED spectral-scan.c)
add_dependencies(spectral-scan hostap)
target_include_directories(spectral-scan
  PRIVATE "${distribution_DIR}/include" "${distribution_DIR}/include/libnl3"
)
target_link_libraries(spectral-scan android libnl-3 libnl-genl-3 log m)

add_library(spectral-plot SHARED spectral-plot.c)
target_link_libraries(spectral-plot android jnigraphics log m)

execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
  "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
)
