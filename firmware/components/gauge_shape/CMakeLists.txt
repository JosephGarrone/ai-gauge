# Like gauge_config, deliberately free of ESP-IDF drivers and LVGL: the rasteriser is pure
# geometry, so it builds and is tested on a host with no board attached.
# See docs/adr/0006-custom-shapes-as-polygons.md.
idf_component_register(
    SRCS
        "gauge_shape.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        gauge_config
)
