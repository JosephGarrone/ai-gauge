# Deliberately has no REQUIRES: this component depends on neither ESP-IDF drivers nor LVGL,
# so its parser tests build and run on a host with no board attached.
# See docs/adr/0002-custom-gauge-xml-schema.md.
idf_component_register(
    SRCS
        "gauge_config.c"
        "gauge_config_default.c"
    INCLUDE_DIRS
        "include"
)
