cmake_minimum_required(VERSION 3.16)

# Keep firmware version in main/main.c as single source of truth.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/main/main.c")
file(STRINGS "main/main.c" _fw_ver_line REGEX "#define FIRMWARE_VERSION")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_fw_ver_line}")
set(PROJECT_VER "${CMAKE_MATCH_1}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tedder-metar-map)
