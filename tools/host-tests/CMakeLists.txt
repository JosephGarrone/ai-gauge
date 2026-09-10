# Host-side tests for components that have no ESP-IDF or LVGL dependency.
#
# gauge_config is deliberately a pure-data leaf (docs/architecture.md), so the schema can be
# developed and regression-tested with no board attached. CI runs this on every push.
cmake_minimum_required(VERSION 3.16)
project(ai_gauge_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()

set(GAUGE_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/components/gauge_config")

add_executable(test_gauge_config
    test_gauge_config.c
    "${GAUGE_CONFIG_DIR}/gauge_config.c"
    "${GAUGE_CONFIG_DIR}/gauge_config_default.c"
)

target_include_directories(test_gauge_config PRIVATE "${GAUGE_CONFIG_DIR}/include")

if(NOT MSVC)
    target_compile_options(test_gauge_config PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(test_gauge_config PRIVATE m)
endif()

add_test(NAME gauge_config COMMAND test_gauge_config)
