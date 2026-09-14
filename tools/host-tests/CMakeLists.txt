# Host-side tests for components that have no ESP-IDF or LVGL dependency.
#
# gauge_config and gauge_shape are deliberately pure-data leaves (docs/architecture.md), so the
# schema and the shape rasteriser can be developed and regression-tested with no board attached.
# CI runs this on every push.
cmake_minimum_required(VERSION 3.16)
project(ai_gauge_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(GAUGE_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/components/gauge_config")
set(GAUGE_SHAPE_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/components/gauge_shape")

add_executable(test_gauge_config
    test_gauge_config.c
    "${GAUGE_CONFIG_DIR}/gauge_config.c"
    "${GAUGE_CONFIG_DIR}/gauge_config_default.c"
)
target_include_directories(test_gauge_config PRIVATE "${GAUGE_CONFIG_DIR}/include")

add_executable(test_gauge_shape
    test_gauge_shape.c
    "${GAUGE_SHAPE_DIR}/gauge_shape.c"
)
target_include_directories(test_gauge_shape PRIVATE
    "${GAUGE_SHAPE_DIR}/include"
    "${GAUGE_CONFIG_DIR}/include"
)

# Not a test: prints parse results as JSON so the web editor's parser port can be checked against
# this one (tools/config-app/test/firmware-diff.test.mjs).
add_executable(gauge_config_dump
    gauge_config_dump.c
    "${GAUGE_CONFIG_DIR}/gauge_config.c"
)
target_include_directories(gauge_config_dump PRIVATE "${GAUGE_CONFIG_DIR}/include")

foreach(t test_gauge_config test_gauge_shape gauge_config_dump)
    if(NOT MSVC)
        target_compile_options(${t} PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(${t} PRIVATE m)
    endif()
endforeach()

add_test(NAME gauge_config COMMAND test_gauge_config)
add_test(NAME gauge_shape COMMAND test_gauge_shape)
