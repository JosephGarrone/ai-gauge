/*
 * AI-Gauge entry point.
 *
 * Milestone 2: the 60fps gate. Renders the dial from the built-in config and drives it with
 * a simulated source so the frame cost can be measured before sensors or networking are
 * layered on top. See docs/performance.md for the gate and docs/display-pipeline.md for why
 * the renderer is shaped the way it is.
 *
 * Startup ordering is documented in docs/architecture.md: the gauge must show something
 * before the network is touched.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "board_profile.h"
#include "gauge_config.h"
#include "gauge_perf.h"
#include "gauge_render.h"

static const char *TAG = "app_main";

/*
 * How often the UI samples the value source. Deliberately faster than the LVGL refresh
 * period, so the needle differs on every single frame -- otherwise the measured frame rate
 * just reports this tick rate rather than what the renderer can sustain.
 */
#define SIM_TICK_MS 5

typedef struct {
    gauge_render_t       *gauge;
    const gauge_config_t *cfg;
    uint32_t              elapsed_ms;
} sim_ctx_t;

/*
 * Stands in for sensor_hub until the I2C front-end exists. A full-scale triangle sweep is
 * deliberately harsher than real boost behaviour: it keeps the needle moving every single
 * frame, which is the worst realistic case for the dirty region we are measuring.
 */
static void sim_timer_cb(lv_timer_t *t)
{
    sim_ctx_t *ctx = lv_timer_get_user_data(t);

    ctx->elapsed_ms += SIM_TICK_MS;

    const uint32_t period_ms = 4000; /* one full up-and-down sweep */
    float          phase     = (float)(ctx->elapsed_ms % period_ms) / (float)period_ms;
    float          tri       = (phase < 0.5f) ? (phase * 2.0f) : ((1.0f - phase) * 2.0f);

    float min = ctx->cfg->source.min;
    float max = ctx->cfg->source.max;

    gauge_render_set_value(ctx->gauge, min + tri * (max - min), true);
}

void app_main(void)
{
    /* NVS backs both WiFi credentials and user settings, so it comes up first. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    const board_profile_t *board = board_profile_get();
    ESP_LOGI(TAG, "board: %s (%" PRIu16 "x%" PRIu16 ", %" PRIu8 " Hz)",
             board->name, board->width_px, board->height_px, board->refresh_hz);

    /*
     * The built-in default face until LittleFS loading lands. It is parsed from XML through
     * the same path as any other config, so this also exercises the parser on target.
     */
    const gauge_config_t *cfg = gauge_config_builtin_default();
    ESP_LOGI(TAG, "config '%s': channel=%s range=%.1f..%.1f %s warnings=%" PRIu16,
             cfg->id, cfg->source.channel, (double)cfg->source.min, (double)cfg->source.max,
             cfg->source.unit, cfg->warning_count);

    /* Brings up the CO5300 QSPI panel, CST9217 touch and the LVGL port. */
    bsp_display_start();
    bsp_display_backlight_on();

    /*
     * -1 blocks indefinitely. The BSP declares this as uint32_t but forwards it to an
     * int32_t parameter, so -1 round-trips to "wait forever" -- passing 0 means "do not
     * wait" and fails immediately, which then corrupts LVGL state from the wrong task.
     */
    esp_err_t lock_err = bsp_display_lock(-1);
    if (lock_err != ESP_OK) {
        ESP_LOGE(TAG, "could not take the LVGL lock: %s", esp_err_to_name(lock_err));
        return;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    int64_t         t0 = esp_timer_get_time();
    gauge_render_t *gauge = NULL;
    err = gauge_render_create(scr, cfg, board, &gauge);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gauge_render_create failed: %s", esp_err_to_name(err));
        bsp_display_unlock();
        return;
    }
    ESP_LOGI(TAG, "face pre-render took %" PRId64 " ms",
             (esp_timer_get_time() - t0) / 1000);

    static sim_ctx_t sim;
    sim.gauge = gauge;
    sim.cfg   = cfg;
    lv_timer_create(sim_timer_cb, SIM_TICK_MS, &sim);

    ESP_ERROR_CHECK(gauge_perf_attach(NULL));
    ESP_ERROR_CHECK(gauge_perf_start_reporting(5000, "needle sweep"));

    bsp_display_unlock();

    ESP_LOGI(TAG, "running. internal heap free: %u B, PSRAM free: %u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
