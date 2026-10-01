idf_component_register(
    SRCS
        "app_main.c"
        "panel_gate.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        app_settings
        app_ui
        board_profile
        gauge_config
        gauge_perf
        gauge_render
        gauge_store
        net_svc
        sensor_hub
        nvs_flash
        esp_timer
        app_update
        esp_lcd
)

# panel_gate.c holds the BSP's display-on command back until the first frame (docs/boot-splash.md).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_lcd_panel_io_tx_param")
