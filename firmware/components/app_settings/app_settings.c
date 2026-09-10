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
static const char *KEY_SHOW_FPS   = "showfps";
static const char *KEY_GAUGE      = "gauge";

static app_settings_t s_settings = {
    .brightness   = 80,
    .show_fps     = false,
    .active_gauge = "boost",
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

    size_t len = sizeof(s_settings.active_gauge);
    if (nvs_get_str(h, KEY_GAUGE, s_settings.active_gauge, &len) != ESP_OK) {
        /* nvs_get_str may leave the buffer partly written on failure. */
        snprintf(s_settings.active_gauge, sizeof(s_settings.active_gauge), "boost");
    }

    nvs_close(h);

    ESP_LOGI(TAG, "loaded: brightness=%u%% show_fps=%d gauge='%s'",
             s_settings.brightness, (int)s_settings.show_fps, s_settings.active_gauge);
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
        err = nvs_set_str(h, KEY_GAUGE, s_settings.active_gauge);
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
