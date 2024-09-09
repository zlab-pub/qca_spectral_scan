cmake_minimum_required(VERSION 3.13)
project(SoftSA C)
include(ExternalProject)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wconversion -Wshadow -Wno-unused-parameter -Werror)

set(distribution_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../distribution")

ExternalProject_Add(hostapd
  URL https://w1.fi/releases/hostapd-2.11.tar.gz
  URL_HASH SHA256=2b3facb632fd4f65e32f4bf82a76b4b72c501f995a4f62e330219fe7aed1747a
  DOWNLOAD_DIR "${distribution_DIR}/src"
  SOURCE_DIR "${distribution_DIR}/src/hostapd"
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND ""
  TEST_COMMAND ""
)
add_custom_command(
  TARGET hostapd POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy
    "${distribution_DIR}/src/hostapd/src/common/qca-vendor.h"
    "${distribution_DIR}/include/qca-vendor.h"
  BYPRODUCTS "${distribution_DIR}/include/qca-vendor.h"
  VERBATIM
)
add_custom_target(qca_vendor_h DEPENDS "${distribution_DIR}/include/qca-vendor.h")

ExternalProject_Add(libnl
  URL https://github.com/thom311/libnl/releases/download/libnl3_10_0/libnl-3.10.0.tar.gz
  URL_HASH SHA256=49b3e2235fdb58f5910bbb3ed0de8143b71ffc220571540502eb6c2471f204f5
  DOWNLOAD_DIR "${distribution_DIR}/src"
  SOURCE_DIR "${distribution_DIR}/src/libnl"
  PATCH_COMMAND sed -i.bak -e "s/-lpthread//g" "${distribution_DIR}/src/libnl/configure"
  CONFIGURE_COMMAND "${distribution_DIR}/src/libnl/configure" --enable-cli=no
    --host "${CMAKE_C_COMPILER_TARGET}"
    --prefix "${distribution_DIR}"
    --exec-prefix "${distribution_DIR}/${ANDROID_ABI}"
    "AS=${ANDROID_TOOLCHAIN_ROOT}/bin/clang --target=${CMAKE_C_COMPILER_TARGET}"
    "CC=${ANDROID_TOOLCHAIN_ROOT}/bin/clang --target=${CMAKE_C_COMPILER_TARGET}"
    "CXX=${ANDROID_TOOLCHAIN_ROOT}/bin/clang++ --target=${CMAKE_CXX_COMPILER_TARGET}"
    "LD=${ANDROID_TOOLCHAIN_ROOT}/bin/ld"
    "AR=${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-ar"
    "NM=${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-nm"
    "OBJCOPY=${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-objcopy"
    "OBJDUMP=${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-objdump"
    "RANLIB=${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-ranlib"
    "STRIP=${ANDROID_TOOLCHAIN_ROOT}/bin/llvm-strip"
    "CFLAGS=-fPIC -Din_addr_t=uint32_t"
    "CXXFLAGS=-fPIC -Din_addr_t=uint32_t"
  BUILD_COMMAND make install
  BUILD_BYPRODUCTS
    "${distribution_DIR}/${ANDROID_ABI}/lib/libnl-3.so"
    "${distribution_DIR}/${ANDROID_ABI}/lib/libnl-genl-3.so"
  INSTALL_COMMAND ""
  TEST_COMMAND ""
)

add_library(libnl-3 SHARED IMPORTED)
add_dependencies(libnl-3 libnl)
set_target_properties(libnl-3 PROPERTIES
  IMPORTED_LOCATION "${distribution_DIR}/${ANDROID_ABI}/lib/libnl-3.so"
)
add_library(libnl-genl-3 SHARED IMPORTED)
add_dependencies(libnl-genl-3 libnl)
set_target_properties(libnl-genl-3 PROPERTIES
  IMPORTED_LOCATION "${distribution_DIR}/${ANDROID_ABI}/lib/libnl-genl-3.so"
)

add_library(spectral-scan SHARED spectral-scan.c)
add_dependencies(spectral-scan qca_vendor_h)
target_link_libraries(spectral-scan android libnl-3 libnl-genl-3 log m)
target_include_directories(spectral-scan
  PRIVATE "${distribution_DIR}/include" "${distribution_DIR}/include/libnl3"
)

add_library(spectral-plot SHARED spectral-plot.c)
target_link_libraries(spectral-plot android jnigraphics log m)

execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
  "${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
)
