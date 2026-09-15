idf_component_register(
    SRCS
        "net_svc.c"
        "net_svc_http.c"
        "net_svc_telemetry.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "."
    REQUIRES
        gauge_store
    PRIV_REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        esp_app_format
        app_update
        nvs_flash
        lwip
        espressif__mdns
)

# The face editor, gzipped into the image so the gauge serves it at /editor/ (ADR 0008). Built into
# the app rather than stored on LittleFS, because OTA updates only the app: the editor's parser port
# must match the firmware it talks to.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(editor_dir "${CMAKE_CURRENT_LIST_DIR}/../../../tools/config-app")
    set(faces_dir "${CMAKE_CURRENT_LIST_DIR}/../../assets/gauges")
    set(editor_bundle "${CMAKE_CURRENT_BINARY_DIR}/net_svc_editor_files.c")

    file(GLOB editor_inputs CONFIGURE_DEPENDS
        "${editor_dir}/index.html" "${editor_dir}/css/*.css" "${editor_dir}/js/*.js" "${faces_dir}/*.xml")

    add_custom_command(
        OUTPUT "${editor_bundle}"
        COMMAND ${CMAKE_COMMAND} -D EDITOR_DIR=${editor_dir} -D FACES_DIR=${faces_dir}
                -D OUT=${editor_bundle} -P ${CMAKE_CURRENT_LIST_DIR}/editor_bundle.cmake
        DEPENDS ${editor_inputs} "${CMAKE_CURRENT_LIST_DIR}/editor_bundle.cmake"
        VERBATIM)

    target_sources(${COMPONENT_LIB} PRIVATE "${editor_bundle}")
endif()
