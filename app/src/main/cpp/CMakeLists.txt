cmake_minimum_required(VERSION 3.13)
project(spectral-plot)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wconversion -Wshadow -Wno-unused-parameter -Werror)

set(distribution_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../distribution)

add_library(libnl-3 STATIC IMPORTED)
set_target_properties(libnl-3 PROPERTIES IMPORTED_LOCATION
    ${distribution_DIR}/lib/libnl-3.a)

add_library(libnl-genl-3 STATIC IMPORTED)
set_target_properties(libnl-genl-3 PROPERTIES IMPORTED_LOCATION
    ${distribution_DIR}/lib/libnl-genl-3.a)

add_library(spectral-scan SHARED
    spectral-scan.c)
target_include_directories(spectral-scan PRIVATE
    ${distribution_DIR}/include)
target_include_directories(spectral-scan PRIVATE
    ${distribution_DIR}/include/libnl3)
target_link_libraries(spectral-scan
    android
    libnl-3
    libnl-genl-3
    log
    m)

add_library(spectral-plot SHARED
    spectral-plot.c)
target_link_libraries(spectral-plot
    android
    jnigraphics
    log
    m)

execute_process(COMMAND
    ${CMAKE_COMMAND}
    -E
    create_symlink
    ${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json
    ${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json
)
