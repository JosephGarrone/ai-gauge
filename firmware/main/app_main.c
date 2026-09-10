/*
 * AI-Gauge entry point.
 *
 * Startup ordering matters and is documented in docs/architecture.md: the gauge must show
 * something before anything slow or fallible is touched. A driver turning the ignition on
 * should see a needle immediately.
 *
 * The value source is still simulated -- sensor_hub and the external I2C front-end do not
 * exist yet. See CONFIG_AI_GAUGE_SIMULATED_SOURCE.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

#include "app_settings.h"
#include "app_ui.h"
#include "board_profile.h"
#include "gauge_config.h"
#include "gauge_perf.h"
#include "gauge_render.h"
#include "gauge_store.h"

static const char *TAG = "app_main";

/*
 * How often the UI samples the value source. Deliberately faster than the LVGL refresh
 * period, so the needle differs on every frame -- otherwise a measured frame rate just
 * reports this tick rate rather than what the renderer can sustain.
 */
#define SIM_TICK_MS 5

/*
 * The configuration currently on screen. Held here rather than pointed at, because switching
 * gauges replaces it and anything holding a pointer to the old one (the simulator's range,
 * for instance) would keep using stale limits.
 */
static gauge_config_t s_active_cfg;

#if CONFIG_AI_GAUGE_SIMULATED_SOURCE

/*
 * Stands in for sensor_hub. A full-scale triangle sweep is deliberately harsher than real
 * boost behaviour: it keeps the needle moving every frame, which is the worst realistic case
 * for the dirty region.
 */
static void sim_timer_cb(lv_timer_t *t)
{
    static uint32_t elapsed_ms;
    (void)t;

    elapsed_ms += SIM_TICK_MS;

    const uint32_t period_ms = 4000; /* one full up-and-down sweep */
    float          phase     = (float)(elapsed_ms % period_ms) / (float)period_ms;
    float          tri       = (phase < 0.5f) ? (phase * 2.0f) : ((1.0f - phase) * 2.0f);

    float min = s_active_cfg.source.min;
    float max = s_active_cfg.source.max;

    gauge_render_set_value(app_ui_get_gauge(), min + tri * (max - min), true);
}

#endif /* CONFIG_AI_GAUGE_SIMULATED_SOURCE */

#if CONFIG_AI_GAUGE_BENCH_TRANSITION

/*
 * Scenario 4 in docs/performance.md. Swipe transitions redraw the whole screen for the
 * duration of the animation, so they are measured separately from steady state -- and
 * measured mechanically, because a human swiping is not a repeatable input.
 */
static void bench_timer_cb(lv_timer_t *t)
{
    (void)t;
    app_ui_show_tile(app_ui_get_tile() == APP_UI_TILE_GAUGE ? APP_UI_TILE_SETTINGS
                                                            : APP_UI_TILE_GAUGE,
                     true);
}

#endif /* CONFIG_AI_GAUGE_BENCH_TRANSITION */

/* ------------------------------------------------------------ configuration --------- */

/*
 * Decide which face to show. In preference order: the gauge the user last selected, then the
 * first one on the filesystem, then the compiled-in default.
 *
 * Returns the id that was actually loaded, or NULL if the built-in face is in use.
 */
static const char *load_initial_config(char *warning, size_t warning_len)
{
    static char loaded_id[GAUGE_CONFIG_MAX_ID_LEN];

    char err[GAUGE_STORE_ERR_LEN] = {0};

    const char *wanted = app_settings_get()->active_gauge;
    if (wanted[0] != '\0' &&
        gauge_store_load(wanted, &s_active_cfg, err, sizeof(err)) == ESP_OK) {
        snprintf(loaded_id, sizeof(loaded_id), "%s", wanted);
        if (err[0] != '\0') {
            snprintf(warning, warning_len, "Gauge '%s': %s", wanted, err);
        }
        return loaded_id;
    }

    if (wanted[0] != '\0' && err[0] != '\0') {
        ESP_LOGW(TAG, "could not load '%s': %s", wanted, err);
    }

    /* The selected gauge is gone or broken. Fall back to whatever else is there. */
    gauge_store_list_t list;
    gauge_store_list(&list);

    for (int i = 0; i < list.count; i++) {
        /* No point retrying the one that just failed. */
        if (strcmp(list.ids[i], wanted) == 0) {
            continue;
        }

        char alt_err[GAUGE_STORE_ERR_LEN] = {0};
        if (gauge_store_load(list.ids[i], &s_active_cfg, alt_err, sizeof(alt_err)) == ESP_OK) {
            snprintf(loaded_id, sizeof(loaded_id), "%s", list.ids[i]);
            snprintf(warning, warning_len, "Gauge '%s' unavailable (%s); showing '%s'.",
                     wanted, err[0] ? err : "not found", list.ids[i]);
            return loaded_id;
        }
    }

    /* Nothing usable on the filesystem. The built-in face is the last line of defence. */
    s_active_cfg = *gauge_config_builtin_default();

    if (!gauge_store_mounted()) {
        snprintf(warning, warning_len, "Storage unavailable; showing the built-in gauge.");
    } else if (list.count == 0) {
        snprintf(warning, warning_len, "No gauge configs found; showing the built-in gauge.");
    } else {
        snprintf(warning, warning_len, "No gauge config could be loaded (%s); "
                 "showing the built-in gauge.", err[0] ? err : "unknown error");
    }

    return NULL;
}

/* Runs on the LVGL task, from the settings-page picker. */
static void on_gauge_selected(const char *id)
{
    gauge_config_t cfg;
    char           err[GAUGE_STORE_ERR_LEN] = {0};

    esp_err_t load_err = gauge_store_load(id, &cfg, err, sizeof(err));
    if (load_err != ESP_OK) {
        ESP_LOGE(TAG, "could not switch to '%s': %s", id, err);
        app_ui_set_warning(err);
        return;
    }

    if (app_ui_set_config(&cfg) != ESP_OK) {
        app_ui_set_warning("Could not build that gauge; keeping the current one.");
        return;
    }

    s_active_cfg = cfg;

    app_settings_set_active_gauge(id);
    app_settings_commit();

    app_ui_set_warning(err[0] != '\0' ? err : NULL);
}

/* ------------------------------------------------------------------- startup -------- */

void app_main(void)
{
    /* NVS backs both WiFi credentials and user settings, so it comes up first. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_settings_init();

    /* Non-fatal by design: the built-in face covers an unusable filesystem. */
    gauge_store_init();

    const board_profile_t *board = board_profile_get();
    ESP_LOGI(TAG, "board: %s (%" PRIu16 "x%" PRIu16 ", %" PRIu8 " Hz)",
             board->name, board->width_px, board->height_px, board->refresh_hz);

    char        warning[128] = {0};
    const char *loaded_id    = load_initial_config(warning, sizeof(warning));

    ESP_LOGI(TAG, "config '%s' (%s): channel=%s range=%.1f..%.1f %s warnings=%" PRIu16,
             s_active_cfg.id, loaded_id ? "storage" : "built-in", s_active_cfg.source.channel,
             (double)s_active_cfg.source.min, (double)s_active_cfg.source.max,
             s_active_cfg.source.unit, s_active_cfg.warning_count);

    /* Brings up the CO5300 QSPI panel, CST9217 touch and the LVGL port. */
    bsp_display_start();
    bsp_display_brightness_set(app_settings_get()->brightness);

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

    int64_t t0 = esp_timer_get_time();
    err = app_ui_create(&s_active_cfg, board);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_ui_create failed: %s", esp_err_to_name(err));
        bsp_display_unlock();
        return;
    }
    ESP_LOGI(TAG, "UI built in %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);

    gauge_store_list_t list;
    gauge_store_list(&list);
    app_ui_set_gauge_list(list.count > 0 ? list.ids : NULL, list.count, loaded_id);
    app_ui_set_gauge_selected_cb(on_gauge_selected);

    if (warning[0] != '\0') {
        ESP_LOGW(TAG, "%s", warning);
        app_ui_set_warning(warning);
    }

#if CONFIG_AI_GAUGE_SIMULATED_SOURCE
    lv_timer_create(sim_timer_cb, SIM_TICK_MS, NULL);
    ESP_LOGW(TAG, "using a SIMULATED value source; no sensors are being read");
#endif

    ESP_ERROR_CHECK(gauge_perf_attach(NULL));

#if CONFIG_AI_GAUGE_BENCH_TRANSITION
    lv_timer_create(bench_timer_cb, 1200, NULL);
    ESP_ERROR_CHECK(gauge_perf_start_reporting(5000, "tile transition"));
    ESP_LOGW(TAG, "transition benchmark active; the display will flip tiles continuously");
#else
    ESP_ERROR_CHECK(gauge_perf_start_reporting(5000, "needle sweep"));
#endif

    bsp_display_unlock();

    ESP_LOGI(TAG, "running. internal heap free: %u B, PSRAM free: %u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
