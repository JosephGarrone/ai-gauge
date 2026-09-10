cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Version comes from the git tag in CI, falling back to the short SHA for untagged builds.
# See docs/ci-release.md -- a device must always be able to report exactly what it is running.
if(DEFINED ENV{AI_GAUGE_VERSION})
    set(PROJECT_VER "$ENV{AI_GAUGE_VERSION}")
endif()

project(ai-gauge)

# The LittleFS image flashed to the `storage` partition, built from assets/.
# Provides the `storage-flash` target (see docs/build-and-flash.md).
littlefs_create_partition_image(storage "${CMAKE_CURRENT_SOURCE_DIR}/assets" FLASH_IN_PROJECT)
