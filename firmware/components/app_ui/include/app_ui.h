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
 * All functions must be called with the LVGL lock held (bsp_display_lock), except the remote
 * control ones, which say otherwise.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

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
 * Also applies the stored screen rotation (app_settings), so app_settings_init() must have run.
 *
 * @param cfg   Parsed gauge configuration.
 * @param board Panel geometry. Must outlive the UI.
 */
esp_err_t app_ui_create(const gauge_config_t *cfg, const board_profile_t *board);

/** @brief The gauge on the dial tile, so a value source can drive it. NULL before create. */
gauge_render_t *app_ui_get_gauge(void);

/**
 * @brief Replace the displayed gauge with a different configuration.
 *
 * Tears down the current gauge and rebuilds it, which re-rasterises the face -- tens of
 * milliseconds, fine for a user action but never per frame. No reboot required.
 */
esp_err_t app_ui_set_config(const gauge_config_t *cfg);

/**
 * @brief Populate the gauge picker on the settings page.
 *
 * @param ids     Available configuration ids.
 * @param count   How many.
 * @param active  Currently selected id, or NULL.
 */
void app_ui_set_gauge_list(const char (*ids)[GAUGE_CONFIG_MAX_ID_LEN], int count,
                           const char *active);

/**
 * @brief Called when the user picks a different gauge from the settings page.
 *
 * The handler is expected to load the configuration and call app_ui_set_config().
 */
typedef void (*app_ui_gauge_selected_cb_t)(const char *id);

void app_ui_set_gauge_selected_cb(app_ui_gauge_selected_cb_t cb);

/**
 * @brief Called when the user confirms "Reset network" on the settings page.
 *
 * Runs on the LVGL task. The button asks for a second tap before calling this, so the handler
 * can act straight away -- typically by forgetting the stored WiFi credentials.
 */
typedef void (*app_ui_network_reset_cb_t)(void);

void app_ui_set_network_reset_cb(app_ui_network_reset_cb_t cb);

/**
 * @brief Register a callback for alert transitions on the displayed gauge.
 *
 * Re-applied automatically whenever the gauge is rebuilt (for example on a config change), so
 * the caller registers it once.
 */
void app_ui_set_alert_cb(gauge_render_alert_cb_t cb, void *user_data);

/** @brief Navigate to a tile programmatically. */
void app_ui_show_tile(app_ui_tile_t tile, bool animate);

/** @brief Which tile is currently shown. */
app_ui_tile_t app_ui_get_tile(void);

/**
 * @brief Update the WiFi status line on the settings page.
 *
 * @param state Short state word, e.g. "connected".
 * @param detail Address or network name; may be NULL.
 */
void app_ui_set_network_status(const char *state, const char *detail);

/**
 * @brief Called when the user taps the firmware version line on the settings page.
 *
 * Runs on the LVGL task. The line is the update control: the owner decides what a tap means
 * (check, confirm, install) and says so with app_ui_set_update_status().
 */
typedef void (*app_ui_firmware_tap_cb_t)(void);

void app_ui_set_firmware_tap_cb(app_ui_firmware_tap_cb_t cb);

/**
 * @brief Set the update line shown under the firmware version, e.g. "Tap to check for updates".
 *
 * @param text One or two short lines; NULL or "" shows the version alone.
 */
void app_ui_set_update_status(const char *text);

/* ------------------------------------------------------------ remote control ----- */

/**
 * A press at (x1, y1) that moves in a straight line to (x2, y2) over duration_ms, then releases.
 * A tap or long press is one that does not move. Coordinates are screen pixels, the same as a
 * screenshot's, whatever the rotation.
 */
typedef struct {
    int32_t  x1, y1, x2, y2;
    uint32_t duration_ms;
} app_ui_gesture_t;

typedef struct {
    uint16_t       width, height;
    uint32_t       stride; /**< Bytes per row. */
    const uint8_t *rgb565; /**< Native little-endian RGB565. */
    void          *handle; /**< For app_ui_screenshot_free(). */
} app_ui_image_t;

/** @brief Create the remote pointer device. On the LVGL task or under bsp_display_lock(). */
esp_err_t app_ui_remote_start(void);

/**
 * @brief Play a gesture through the remote pointer and wait for it to finish.
 *
 * Call from any task *except* the LVGL task, which plays it. Returns ESP_ERR_INVALID_STATE while
 * another gesture is playing. An animation the gesture starts, such as a tile swipe, may still be
 * running on return.
 */
esp_err_t app_ui_remote_gesture(const app_ui_gesture_t *g);

/**
 * @brief Render the screen into a new PSRAM image, on the LVGL task, and wait for it.
 *
 * Call from any task except the LVGL task. Costs about one full-screen render (a dropped frame)
 * and ~430KB of PSRAM until app_ui_screenshot_free().
 */
esp_err_t app_ui_screenshot(app_ui_image_t *out);

void app_ui_screenshot_free(app_ui_image_t *img);

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
