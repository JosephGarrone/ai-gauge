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

/** Whole-screen rotation, clockwise, so the gauge can be mounted at any angle. */
typedef enum {
    APP_SETTINGS_ROTATION_0 = 0,
    APP_SETTINGS_ROTATION_90,
    APP_SETTINGS_ROTATION_180,
    APP_SETTINGS_ROTATION_270,
    APP_SETTINGS_ROTATION_COUNT,
} app_settings_rotation_t;

/**
 * Frame-rate cap: the most frames per second the display will draw. Lower caps leave the CPU idle
 * between frames; "uncapped" draws as fast as rendering allows, past what the 60 Hz panel can show.
 */
typedef enum {
    APP_SETTINGS_FPS_CAP_30 = 0,
    APP_SETTINGS_FPS_CAP_45,
    APP_SETTINGS_FPS_CAP_60,
    APP_SETTINGS_FPS_CAP_UNCAPPED,
    APP_SETTINGS_FPS_CAP_COUNT,
} app_settings_fps_cap_t;

/**
 * Decimal places on a pressure readout. "Auto" follows the face's unit: none for psi and kPa, one
 * for bar. A face in any other unit keeps its own format.
 */
typedef enum {
    APP_SETTINGS_DECIMALS_AUTO = 0,
    APP_SETTINGS_DECIMALS_0,
    APP_SETTINGS_DECIMALS_1,
    APP_SETTINGS_DECIMALS_2,
    APP_SETTINGS_DECIMALS_COUNT,
} app_settings_decimals_t;

/** How a temperature readout is rounded: to the degree, or to the nearest 10 degrees. */
typedef enum {
    APP_SETTINGS_TEMP_STEP_1 = 0,
    APP_SETTINGS_TEMP_STEP_10,
    APP_SETTINGS_TEMP_STEP_COUNT,
} app_settings_temp_step_t;

/** How much history the sparkline strip shows. */
typedef enum {
    APP_SETTINGS_TREND_30S = 0,
    APP_SETTINGS_TREND_60S,
    APP_SETTINGS_TREND_120S,
    APP_SETTINGS_TREND_COUNT,
} app_settings_trend_t;

/** How often the dial moves a few pixels, so its static artwork does not burn into the AMOLED. */
typedef enum {
    APP_SETTINGS_SHIFT_OFF = 0,
    APP_SETTINGS_SHIFT_1MIN,
    APP_SETTINGS_SHIFT_3MIN,
    APP_SETTINGS_SHIFT_10MIN,
    APP_SETTINGS_SHIFT_COUNT,
} app_settings_shift_t;

typedef struct {
    uint8_t brightness;                          /**< Percent, clamped to the range above. */
    bool    show_fps;                            /**< FPS badge on the gauge screen. */
    app_settings_rotation_t rotation;            /**< Screen rotation. */
    app_settings_fps_cap_t  fps_cap;             /**< Frame-rate cap. */
    app_settings_decimals_t  pressure_decimals;  /**< Boost and other pressure readouts. */
    app_settings_temp_step_t temp_step;          /**< EGT and other temperature readouts. */
    bool                     show_peak;          /**< The peak figure under the readout. */
    app_settings_trend_t     trend_window;       /**< Sparkline history. */
    app_settings_shift_t     pixel_shift;        /**< Burn-in protection. */
    bool                     engine_off_screen;  /**< Go dark with a drifting notice when the engine stops. */
    char    active_gauge[APP_SETTINGS_MAX_GAUGE_ID];
} app_settings_t;

/** @brief Seconds of history for a trend window setting. */
uint32_t app_settings_trend_seconds(app_settings_trend_t w);

/** @brief Milliseconds between pixel-shift steps, or 0 for off. */
uint32_t app_settings_shift_period_ms(app_settings_shift_t s);

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

/** @brief Update the screen rotation in memory. Out-of-range values are ignored. */
void app_settings_set_rotation(app_settings_rotation_t rotation);

/** @brief Update the frame-rate cap in memory. Out-of-range values are ignored. */
void app_settings_set_fps_cap(app_settings_fps_cap_t cap);

/** @brief Update the pressure readout's decimal places in memory. Out-of-range values are ignored. */
void app_settings_set_pressure_decimals(app_settings_decimals_t d);

/** @brief Update the temperature readout's rounding in memory. Out-of-range values are ignored. */
void app_settings_set_temp_step(app_settings_temp_step_t step);

/** @brief Update whether the peak figure is shown, in memory. */
void app_settings_set_show_peak(bool show);

/** @brief Update the sparkline window in memory. Out-of-range values are ignored. */
void app_settings_set_trend_window(app_settings_trend_t w);

/** @brief Update the pixel-shift interval in memory. Out-of-range values are ignored. */
void app_settings_set_pixel_shift(app_settings_shift_t s);

/** @brief Update whether the engine-off screen is used, in memory. */
void app_settings_set_engine_off_screen(bool on);

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
