cmake_minimum_required(VERSION 3.16)

# Pull PROJECT_VER from FIRMWARE_VERSION in main/main.c so esp_app_get_description()
# returns the same string we log (and OTA compares). Reconfigure if main.c changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/main/main.c")
file(STRINGS "main/main.c" _fw_ver_line REGEX "#define FIRMWARE_VERSION")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _ "${_fw_ver_line}")
set(PROJECT_VER "${CMAKE_MATCH_1}")

# Test images (cache variables: they stick in the build dir until set back to 0):
#   -DTMM_ROLLBACK_TEST=1  image aborts at boot and must be rolled back (version <v>-rbtest)
#   -DTMM_VER_SUFFIX=-x    distinct version string
#   -DTMM_TX_DEFAULT=20    default TX power; -DTMM_TX_TEST=1 short autotune timers
if(TMM_ROLLBACK_TEST)
    set(PROJECT_VER "${PROJECT_VER}-rbtest")
endif()
if(TMM_VER_SUFFIX)
    set(PROJECT_VER "${PROJECT_VER}${TMM_VER_SUFFIX}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# idf_build_set_property only exists after project.cmake is included (must precede project()).
if(TMM_ROLLBACK_TEST)
    idf_build_set_property(COMPILE_DEFINITIONS "TMM_ROLLBACK_TEST=1" APPEND)
endif()
if(TMM_TX_DEFAULT)
    idf_build_set_property(COMPILE_DEFINITIONS "TMM_TX_DEFAULT=${TMM_TX_DEFAULT}" APPEND)
endif()
if(TMM_TX_TEST)
    idf_build_set_property(COMPILE_DEFINITIONS "TMM_TX_TEST=1" APPEND)
endif()
project(tedder-metar-map)

# Maintainer only: with TMM_S3 set in the environment (s3://bucket/prefix; see tools/tmm_env.py),
# every build uploads the signed image to $TMM_S3/bin/beta.bin when it changed (tools/ota_upload.sh;
# AWS_PROFILE from the environment). Promote beta to stable with tools/promote.py.
# -DTMM_NO_UPLOAD=1 skips the upload (test images, local builds).
if(DEFINED ENV{TMM_S3} AND NOT TMM_NO_UPLOAD AND NOT TMM_ROLLBACK_TEST AND NOT TMM_VER_SUFFIX)
    add_custom_target(ota_upload ALL
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/ota_upload.sh
                ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin $ENV{TMM_S3}/bin/beta.bin
                ${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.elf ${PROJECT_VER}
        COMMENT "OTA upload check -> $ENV{TMM_S3}/bin/beta.bin"
        VERBATIM
    )
    add_dependencies(ota_upload app)
endif()
