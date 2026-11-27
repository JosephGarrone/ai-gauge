/*
 * NVS-backed user settings. See app_settings.h.
 *
 * Every load path falls back to a default rather than failing: a settings problem must never
 * be the reason a gauge does not come up.
 */

#include "app_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG       = "app_settings";
static const char *NAMESPACE = "ai_gauge";

static const char *KEY_BRIGHTNESS = "bright";
/*
 * "showfps2", not "showfps": the badge is debug, and a gauge that had it switched on before
 * 2026-11-27 starts with it off once after the update. Do not rename it back.
 */
static const char *KEY_SHOW_FPS   = "showfps2";
static const char *KEY_ROTATION   = "rotate";
static const char *KEY_FPS_CAP    = "fpscap";
static const char *KEY_GAUGE      = "gauge";
static const char *KEY_PRESS_DEC  = "pdec";
static const char *KEY_TEMP_STEP  = "tstep";
static const char *KEY_SHOW_PEAK  = "showpeak";
static const char *KEY_TREND      = "trend";
static const char *KEY_SHIFT      = "shift";
static const char *KEY_ENGINE_OFF = "engoff";
static const char *KEY_FOLLOW     = "follow";

static app_settings_t s_settings = {
    /* Modest: AMOLED wear rises steeply with brightness. */
    .brightness        = 50,
    .show_fps          = false,
    .rotation          = APP_SETTINGS_ROTATION_0,
    .fps_cap           = APP_SETTINGS_FPS_CAP_60,
    .pressure_decimals = APP_SETTINGS_DECIMALS_AUTO,
    .temp_step         = APP_SETTINGS_TEMP_STEP_1,
    .show_peak         = false,
    .trend_window      = APP_SETTINGS_TREND_60S,
    .pixel_shift       = APP_SETTINGS_SHIFT_3MIN,
    .engine_off_screen = true,
    .follow_display    = true,
    .active_gauge      = "boost",
};

static bool s_dirty;

static uint8_t clamp_brightness(int v)
{
    if (v < APP_SETTINGS_BRIGHTNESS_MIN) {
        return APP_SETTINGS_BRIGHTNESS_MIN;
    }
    if (v > APP_SETTINGS_BRIGHTNESS_MAX) {
        return APP_SETTINGS_BRIGHTNESS_MAX;
    }
    return (uint8_t)v;
}

esp_err_t app_settings_init(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NAMESPACE, NVS_READONLY, &h);

    if (err != ESP_OK) {
        /* First boot, or the namespace has never been written. Defaults are correct here. */
        ESP_LOGI(TAG, "no stored settings (%s), using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    uint8_t u8;
    if (nvs_get_u8(h, KEY_BRIGHTNESS, &u8) == ESP_OK) {
        s_settings.brightness = clamp_brightness(u8);
    }
    if (nvs_get_u8(h, KEY_SHOW_FPS, &u8) == ESP_OK) {
        s_settings.show_fps = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_ROTATION, &u8) == ESP_OK && u8 < APP_SETTINGS_ROTATION_COUNT) {
        s_settings.rotation = (app_settings_rotation_t)u8;
    }
    if (nvs_get_u8(h, KEY_FPS_CAP, &u8) == ESP_OK && u8 < APP_SETTINGS_FPS_CAP_COUNT) {
        s_settings.fps_cap = (app_settings_fps_cap_t)u8;
    }

    if (nvs_get_u8(h, KEY_PRESS_DEC, &u8) == ESP_OK && u8 < APP_SETTINGS_DECIMALS_COUNT) {
        s_settings.pressure_decimals = (app_settings_decimals_t)u8;
    }
    if (nvs_get_u8(h, KEY_TEMP_STEP, &u8) == ESP_OK && u8 < APP_SETTINGS_TEMP_STEP_COUNT) {
        s_settings.temp_step = (app_settings_temp_step_t)u8;
    }
    if (nvs_get_u8(h, KEY_SHOW_PEAK, &u8) == ESP_OK) {
        s_settings.show_peak = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_TREND, &u8) == ESP_OK && u8 < APP_SETTINGS_TREND_COUNT) {
        s_settings.trend_window = (app_settings_trend_t)u8;
    }
    if (nvs_get_u8(h, KEY_SHIFT, &u8) == ESP_OK && u8 < APP_SETTINGS_SHIFT_COUNT) {
        s_settings.pixel_shift = (app_settings_shift_t)u8;
    }
    if (nvs_get_u8(h, KEY_ENGINE_OFF, &u8) == ESP_OK) {
        s_settings.engine_off_screen = (u8 != 0);
    }
    if (nvs_get_u8(h, KEY_FOLLOW, &u8) == ESP_OK) {
        s_settings.follow_display = (u8 != 0);
    }

    size_t len = sizeof(s_settings.active_gauge);
    if (nvs_get_str(h, KEY_GAUGE, s_settings.active_gauge, &len) != ESP_OK) {
        /* nvs_get_str may leave the buffer partly written on failure. */
        snprintf(s_settings.active_gauge, sizeof(s_settings.active_gauge), "boost");
    }

    nvs_close(h);

    ESP_LOGI(TAG, "loaded: brightness=%u%% show_fps=%d rotation=%d fps_cap=%d gauge='%s'",
             s_settings.brightness, (int)s_settings.show_fps, (int)s_settings.rotation * 90,
             (int)s_settings.fps_cap, s_settings.active_gauge);
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_settings;
}

void app_settings_set_brightness(uint8_t percent)
{
    uint8_t v = clamp_brightness(percent);
    if (v != s_settings.brightness) {
        s_settings.brightness = v;
        s_dirty               = true;
    }
}

void app_settings_set_show_fps(bool show)
{
    if (show != s_settings.show_fps) {
        s_settings.show_fps = show;
        s_dirty             = true;
    }
}

void app_settings_set_rotation(app_settings_rotation_t rotation)
{
    if ((unsigned)rotation >= APP_SETTINGS_ROTATION_COUNT) {
        return;
    }
    if (rotation != s_settings.rotation) {
        s_settings.rotation = rotation;
        s_dirty             = true;
    }
}

void app_settings_set_fps_cap(app_settings_fps_cap_t cap)
{
    if ((unsigned)cap >= APP_SETTINGS_FPS_CAP_COUNT) {
        return;
    }
    if (cap != s_settings.fps_cap) {
        s_settings.fps_cap = cap;
        s_dirty            = true;
    }
}

/* Out-of-range values are ignored; a change marks the settings for the next commit. */
#define SET_FIELD(field, value, count)                \
    do {                                              \
        if ((unsigned)(value) >= (unsigned)(count)) { \
            return;                                   \
        }                                             \
        if ((value) != s_settings.field) {            \
            s_settings.field = (value);               \
            s_dirty          = true;                  \
        }                                             \
    } while (0)

void app_settings_set_pressure_decimals(app_settings_decimals_t d)
{
    SET_FIELD(pressure_decimals, d, APP_SETTINGS_DECIMALS_COUNT);
}

void app_settings_set_temp_step(app_settings_temp_step_t step)
{
    SET_FIELD(temp_step, step, APP_SETTINGS_TEMP_STEP_COUNT);
}

void app_settings_set_show_peak(bool show)
{
    if (show != s_settings.show_peak) {
        s_settings.show_peak = show;
        s_dirty              = true;
    }
}

void app_settings_set_trend_window(app_settings_trend_t w)
{
    SET_FIELD(trend_window, w, APP_SETTINGS_TREND_COUNT);
}

void app_settings_set_pixel_shift(app_settings_shift_t sh)
{
    SET_FIELD(pixel_shift, sh, APP_SETTINGS_SHIFT_COUNT);
}

void app_settings_set_engine_off_screen(bool on)
{
    if (on != s_settings.engine_off_screen) {
        s_settings.engine_off_screen = on;
        s_dirty                      = true;
    }
}

void app_settings_set_follow_display(bool on)
{
    if (on != s_settings.follow_display) {
        s_settings.follow_display = on;
        s_dirty                   = true;
    }
}

uint32_t app_settings_trend_seconds(app_settings_trend_t w)
{
    static const uint16_t k_seconds[APP_SETTINGS_TREND_COUNT] = {30, 60, 120};
    return ((unsigned)w < APP_SETTINGS_TREND_COUNT) ? k_seconds[w] : 60;
}

uint32_t app_settings_shift_period_ms(app_settings_shift_t sh)
{
    static const uint32_t k_ms[APP_SETTINGS_SHIFT_COUNT] = {0, 60000, 180000, 600000};
    return ((unsigned)sh < APP_SETTINGS_SHIFT_COUNT) ? k_ms[sh] : 0;
}

void app_settings_set_active_gauge(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        return;
    }
    if (strcmp(id, s_settings.active_gauge) != 0) {
        snprintf(s_settings.active_gauge, sizeof(s_settings.active_gauge), "%s", id);
        s_dirty = true;
    }
}

esp_err_t app_settings_commit(void)
{
    if (!s_dirty) {
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(h, KEY_BRIGHTNESS, s_settings.brightness);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_SHOW_FPS, s_settings.show_fps ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_ROTATION, (uint8_t)s_settings.rotation);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_FPS_CAP, (uint8_t)s_settings.fps_cap);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_GAUGE, s_settings.active_gauge);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_PRESS_DEC, (uint8_t)s_settings.pressure_decimals);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_TEMP_STEP, (uint8_t)s_settings.temp_step);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_SHOW_PEAK, s_settings.show_peak ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_TREND, (uint8_t)s_settings.trend_window);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_SHIFT, (uint8_t)s_settings.pixel_shift);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_ENGINE_OFF, s_settings.engine_off_screen ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_FOLLOW, s_settings.follow_display ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err == ESP_OK) {
        s_dirty = false;
        ESP_LOGI(TAG, "settings saved");
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }

    return err;
}
