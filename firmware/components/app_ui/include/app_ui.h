/**
 * @file app_ui.h
 * @brief Screen layout and navigation.
 *
 * Two vertically stacked tiles: the dial, and a settings page revealed by swiping up.
 * lv_tileview provides momentum-tracked swipe for free.
 *
 * The transition is the most expensive thing the UI does -- it necessarily redraws the whole
 * screen for the duration of the animation. See docs/display-pipeline.md.
 *
 * All functions must be called with the LVGL lock held (bsp_display_lock).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "board_profile.h"
#include "gauge_config.h"
#include "gauge_render.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_UI_TILE_GAUGE = 0,
    APP_UI_TILE_SETTINGS,
} app_ui_tile_t;

/**
 * @brief Build the screens and the gauge on the active display.
 *
 * @param cfg   Parsed gauge configuration.
 * @param board Panel geometry. Must outlive the UI.
 */
esp_err_t app_ui_create(const gauge_config_t *cfg, const board_profile_t *board);

/** @brief The gauge on the dial tile, so a value source can drive it. NULL before create. */
gauge_render_t *app_ui_get_gauge(void);

/** @brief Navigate to a tile programmatically. */
void app_ui_show_tile(app_ui_tile_t tile, bool animate);

/** @brief Which tile is currently shown. */
app_ui_tile_t app_ui_get_tile(void);

/**
 * @brief Show a warning on the settings page.
 *
 * Used for problems the user can act on but which must not stop the gauge -- a config that
 * failed to parse, a sensor that did not respond. Pass NULL to clear.
 */
void app_ui_set_warning(const char *text);

#ifdef __cplusplus
}
#endif
