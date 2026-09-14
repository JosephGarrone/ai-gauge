/**
 * @file app_settings.h
 * @brief User settings, persisted in NVS.
 *
 * Deliberately small and flat. Settings are read often and written rarely -- a slider drag
 * must not write flash on every pixel of movement, so writes are explicit and callers are
 * expected to commit on release, not continuously.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SETTINGS_MAX_GAUGE_ID 32

#define APP_SETTINGS_BRIGHTNESS_MIN 10 /**< Below this the AMOLED is unreadable in daylight. */
#define APP_SETTINGS_BRIGHTNESS_MAX 100

typedef struct {
    uint8_t brightness;                          /**< Percent, clamped to the range above. */
    bool    show_fps;                            /**< FPS badge on the gauge screen. */
    bool    alert_sound;                         /**< Audible chime on alerts that request one. */
    char    active_gauge[APP_SETTINGS_MAX_GAUGE_ID];
} app_settings_t;

/**
 * @brief Load settings from NVS, filling in defaults for anything missing.
 *
 * Never fails in a way the caller must handle: a missing or corrupt namespace yields
 * defaults, because a settings problem must not stop the gauge from displaying.
 */
esp_err_t app_settings_init(void);

/** @brief The current settings. Never NULL. */
const app_settings_t *app_settings_get(void);

/** @brief Update brightness in memory. Clamped to the valid range. */
void app_settings_set_brightness(uint8_t percent);

/** @brief Update the FPS badge preference in memory. */
void app_settings_set_show_fps(bool show);

/** @brief Update the alert sound preference in memory. */
void app_settings_set_alert_sound(bool on);

/** @brief Update the active gauge id in memory. */
void app_settings_set_active_gauge(const char *id);

/**
 * @brief Write any in-memory changes to NVS.
 *
 * A no-op when nothing has changed, so it is safe to call on every interaction that ends.
 */
esp_err_t app_settings_commit(void);

#ifdef __cplusplus
}
#endif
