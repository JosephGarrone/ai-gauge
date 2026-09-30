/*
 * AI-Gauge entry point.
 *
 * Startup ordering matters and is documented in docs/architecture.md: the gauge must show
 * something before anything slow or fallible is touched. A driver turning the ignition on
 * should see a needle immediately.
 *
 * Values come from, in order: a fresh UDP telemetry feed, the rear sensor board (sensor_hub), and
 * -- only until a sensor board has been seen -- the simulated sweep. See value_timer_cb().
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
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
#include "net_svc.h"
#include "sensor_hub.h"

static const char *TAG = "app_main";

/*
 * How often the UI samples the value source. Deliberately faster than the LVGL refresh
 * period, so the needle differs on every frame -- otherwise a measured frame rate just
 * reports this tick rate rather than what the renderer can sustain. The face's damping is
 * applied per sample, so it is also tuned against this rate.
 */
#define VALUE_TICK_MS 5

/* Pause between the UI coming up and WiFi claiming its internal memory (see app_main()). */
#define NET_START_SETTLE_MS 1000

/* A telemetry value older than this no longer overrides the local sources. */
#define TELEMETRY_FRESH_US (1000 * 1000)

/*
 * The configuration currently on screen. A copy held here rather than a pointer into someone
 * else's, because switching gauges replaces it and anything holding a pointer to the old one (the
 * simulator's range, for instance) would keep using stale limits.
 *
 * Allocated in PSRAM at startup, not static: a gauge_config_t with custom shapes is over 2KB, and
 * a static would sit in internal RAM, which WiFi needs (docs/performance.md).
 */
static gauge_config_t *s_active_cfg;

/*
 * The stored id of the face on screen, or "" for the built-in face. The file name is what the
 * picker, the HTTP API and the active-gauge setting all use, so this is what changes are matched
 * against.
 */
static char s_active_id[GAUGE_CONFIG_MAX_ID_LEN];

static void set_active_id(const char *id)
{
    strlcpy(s_active_id, id, sizeof(s_active_id));
    net_svc_set_active_gauge(s_active_id);
}

/* ------------------------------------------------------------- value source ------- */

/*
 * The latest telemetry value. Written by the network task, read by the LVGL task, so it goes
 * through a spinlock held for a few instructions rather than the display lock: a network burst
 * must never stall a frame.
 */
static struct {
    portMUX_TYPE lock;
    char         channel[GAUGE_CONFIG_MAX_ID_LEN];
    float        value;
    int64_t      at_us;
} s_telemetry = {.lock = portMUX_INITIALIZER_UNLOCKED};

static bool telemetry_fresh(const char *channel, float *value)
{
    bool fresh = false;
    taskENTER_CRITICAL(&s_telemetry.lock);
    if (s_telemetry.at_us != 0 && esp_timer_get_time() - s_telemetry.at_us < TELEMETRY_FRESH_US &&
        strcmp(s_telemetry.channel, channel) == 0) {
        *value = s_telemetry.value;
        fresh  = true;
    }
    taskEXIT_CRITICAL(&s_telemetry.lock);
    return fresh;
}

#if CONFIG_AI_GAUGE_SIMULATED_SOURCE

/*
 * Stands in for the sensor board on a bare display. A full-scale triangle sweep is deliberately
 * harsher than real boost behaviour: it keeps the needle moving every frame, which is the worst
 * realistic case for the dirty region.
 */
static float sim_value(void)
{
    static uint32_t elapsed_ms;
    elapsed_ms += VALUE_TICK_MS;

    const uint32_t period_ms = 4000; /* one full up-and-down sweep */
    float          phase     = (float)(elapsed_ms % period_ms) / (float)period_ms;
    float          tri       = (phase < 0.5f) ? (phase * 2.0f) : ((1.0f - phase) * 2.0f);

    float min = s_active_cfg->source.min;
    float max = s_active_cfg->source.max;
    return min + tri * (max - min);
}

#endif /* CONFIG_AI_GAUGE_SIMULATED_SOURCE */

/*
 * Feeds the needle. Runs on the LVGL task. Source priority:
 *
 * 1. Telemetry for this channel, while it keeps arriving -- a remote feed is an explicit choice.
 * 2. The sensor board, once it has answered at all. From then on a fault shows as dashes; it is
 *    never papered over with the simulator (docs/sensor-frontend.md, Fault handling).
 * 3. The simulated sweep, if built in, while no board has ever been seen.
 * 4. Otherwise invalid: dashes, needle parked.
 */
static void value_timer_cb(lv_timer_t *t)
{
    (void)t;

    gauge_render_t *gauge   = app_ui_get_gauge();
    const char     *channel = s_active_cfg->source.channel;
    float           value;
    bool            valid;

    if (telemetry_fresh(channel, &value)) {
        gauge_render_set_value(gauge, value, true);
        return;
    }

    if (sensor_hub_read(channel, s_active_cfg->source.unit, &value, &valid)) {
        gauge_render_set_value(gauge, value, valid);
        return;
    }

#if CONFIG_AI_GAUGE_SIMULATED_SOURCE
    gauge_render_set_value(gauge, sim_value(), true);
#else
    gauge_render_set_value(gauge, s_active_cfg->source.min, false);
#endif
}

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
        gauge_store_load(wanted, s_active_cfg, err, sizeof(err)) == ESP_OK) {
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
        if (gauge_store_load(list.ids[i], s_active_cfg, alt_err, sizeof(alt_err)) == ESP_OK) {
            snprintf(loaded_id, sizeof(loaded_id), "%s", list.ids[i]);
            snprintf(warning, warning_len, "Gauge '%s' unavailable (%s); showing '%s'.",
                     wanted, err[0] ? err : "not found", list.ids[i]);
            return loaded_id;
        }
    }

    /* Nothing usable on the filesystem. The built-in face is the last line of defence. */
    gauge_config_builtin_default(s_active_cfg);

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
    /* PSRAM, not the LVGL task's internal-RAM stack: a config is over 2KB. */
    gauge_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM);
    char            err[GAUGE_STORE_ERR_LEN] = {0};

    if (cfg == NULL) {
        app_ui_set_warning("Out of memory; keeping the current gauge.");
        return;
    }

    esp_err_t load_err = gauge_store_load(id, cfg, err, sizeof(err));
    if (load_err != ESP_OK) {
        ESP_LOGE(TAG, "could not switch to '%s': %s", id, err);
        app_ui_set_warning(err);
        free(cfg);
        return;
    }

    if (app_ui_set_config(cfg) != ESP_OK) {
        app_ui_set_warning("Could not build that gauge; keeping the current one.");
        free(cfg);
        return;
    }

    *s_active_cfg = *cfg;
    free(cfg);
    set_active_id(id);

    app_settings_set_active_gauge(id);
    app_settings_commit();

    app_ui_set_warning(err[0] != '\0' ? err : NULL);
}

/* --------------------------------------------------------------- draw buffers ------- */

/*
 * Lines per LVGL flush buffer: 466 x 20 x 2B = 18,640 bytes each, in internal RAM.
 *
 * Load-bearing, measured on hardware. 12 lines hangs the LVGL task outright with no error
 * logged -- the cause is not yet understood -- and larger buffers do not leave room for the
 * network stack. Re-measure before changing it (docs/display-pipeline.md).
 */
#define DRAW_BUF_LINES 20

/*
 * Move the LVGL flush buffers into internal DMA-capable memory.
 *
 * The BSP allocates them in PSRAM (`use_psram = true`), and esp_ptr_dma_capable() is false
 * for PSRAM addresses, so the SPI driver quietly allocated a ~46KB internal bounce buffer
 * and memcpy'd the whole flush into it *on every single transfer*. That works only while a
 * contiguous 46KB block happens to be free; once WiFi starts, the largest free DMA block
 * drops to ~20KB and every transfer fails with ESP_ERR_NO_MEM -- the display simply stops.
 *
 * Allocating our own smaller buffers in internal RAM removes the bounce buffer and the copy
 * entirely, and does it before WiFi has taken its share. This is what
 * docs/display-pipeline.md always specified; the BSP just does not offer a way to configure
 * it, so the buffers are swapped afterwards through LVGL's public API.
 */
static void retarget_draw_buffers(const board_profile_t *board)
{
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        return;
    }

    size_t bytes = (size_t)board->width_px * DRAW_BUF_LINES * 2; /* RGB565 */

    void *buf1 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (buf1 == NULL || buf2 == NULL) {
        /* Keep the BSP's PSRAM buffers rather than leaving the display with none. */
        free(buf1);
        free(buf2);
        ESP_LOGW(TAG, "could not allocate internal draw buffers (%u B each); "
                      "staying on PSRAM buffers", (unsigned)bytes);
        return;
    }

    lv_display_set_buffers(disp, buf1, buf2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "draw buffers: 2 x %u B in internal DMA memory", (unsigned)bytes);
}

/* ---------------------------------------------------------------- networking -------- */

/*
 * These run on core-0 network tasks, never on the LVGL task, so every LVGL call is wrapped
 * in the display lock (docs/architecture.md).
 */

/*
 * The face on screen was deleted over HTTP. Switch to the first remaining face that loads, else
 * the built-in one, as startup would, rather than leave a face on screen that the picker no longer
 * lists. Runs on the HTTP task with the display lock held.
 */
static void show_fallback_after_delete(const char *deleted_id)
{
    /* PSRAM, not the HTTP task's internal-RAM stack, which also rebuilds the face from here. */
    gauge_config_t     *cfg  = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM);
    gauge_store_list_t *list = heap_caps_malloc(sizeof(*list), MALLOC_CAP_SPIRAM);
    if (cfg == NULL || list == NULL) {
        free(cfg);
        free(list);
        app_ui_set_warning("Out of memory; the deleted gauge stays on screen until restart.");
        return;
    }

    char chosen[GAUGE_CONFIG_MAX_ID_LEN] = "";
    gauge_store_list(list);
    for (int i = 0; i < list->count && chosen[0] == '\0'; i++) {
        char err[GAUGE_STORE_ERR_LEN] = {0};
        if (gauge_store_load(list->ids[i], cfg, err, sizeof(err)) == ESP_OK &&
            app_ui_set_config(cfg) == ESP_OK) {
            strlcpy(chosen, list->ids[i], sizeof(chosen));
        }
    }
    free(list);

    if (chosen[0] == '\0') {
        gauge_config_builtin_default(cfg);
        if (app_ui_set_config(cfg) != ESP_OK) {
            free(cfg);
            app_ui_set_warning("Could not build a replacement; the deleted gauge stays on screen.");
            return;
        }
    }

    *s_active_cfg = *cfg;
    free(cfg);
    set_active_id(chosen);
    app_settings_set_active_gauge(chosen);
    app_settings_commit();

    char warning[128];
    if (chosen[0] != '\0') {
        snprintf(warning, sizeof(warning), "Gauge '%s' was deleted; showing '%s'.", deleted_id, chosen);
    } else {
        snprintf(warning, sizeof(warning), "Gauge '%s' was deleted; showing the built-in gauge.",
                 deleted_id);
    }
    app_ui_set_warning(warning);
    ESP_LOGW(TAG, "%s", warning);
}

static void on_config_changed(const char *id, bool deleted)
{
    ESP_LOGI(TAG, "config '%s' %s over HTTP", id, deleted ? "deleted" : "uploaded");

    if (bsp_display_lock(-1) != ESP_OK) {
        return;
    }

    bool on_screen = (strcmp(id, s_active_id) == 0);

    if (deleted && on_screen) {
        show_fallback_after_delete(id);
    }

    /* After any switch, so the picker selects what is now on screen. */
    gauge_store_list_t list;
    gauge_store_list(&list);
    app_ui_set_gauge_list(list.count > 0 ? list.ids : NULL, list.count, s_active_id);

    /* Reload live only when the gauge on screen is the one that changed. */
    if (!deleted && on_screen) {
        /*
         * PSRAM, not the stack: this runs on the HTTP server task, whose stack is internal RAM
         * and which also has to rebuild the face from here (docs/performance.md).
         */
        gauge_config_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM);
        char            err[GAUGE_STORE_ERR_LEN] = {0};

        if (cfg != NULL && gauge_store_load(id, cfg, err, sizeof(err)) == ESP_OK &&
            app_ui_set_config(cfg) == ESP_OK) {
            *s_active_cfg = *cfg;
            app_ui_set_warning(err[0] != '\0' ? err : NULL);
            ESP_LOGI(TAG, "reloaded '%s' without rebooting", id);
        }
        free(cfg);
    }

    bsp_display_unlock();
}

/*
 * Runs on the network task. Only records the value: value_timer_cb() picks it up on the next UI
 * tick, so telemetry and the sensors feed the needle through one path and never fight over it.
 * Telemetry values are in the face's display unit.
 */
static void on_telemetry(const char *channel, float value)
{
    taskENTER_CRITICAL(&s_telemetry.lock);
    strlcpy(s_telemetry.channel, channel, sizeof(s_telemetry.channel));
    s_telemetry.value = value;
    s_telemetry.at_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_telemetry.lock);
}

/*
 * Firmware updates from the settings page. The firmware line is the control: a tap checks, and
 * once a newer release is known, a tap arms the install and a second tap within a few seconds
 * starts it. Two taps because installing restarts the gauge. Runs on the LVGL task.
 */
#define UPDATE_CONFIRM_MS 4000

static int64_t s_update_armed_until_us;

static void update_status_refresh(void)
{
    net_svc_update_status_t u;
    net_svc_update_get_status(&u);
    net_svc_status_t net;
    net_svc_get_status(&net);

    bool armed = esp_timer_get_time() < s_update_armed_until_us;
    char text[96];

    switch (u.state) {
    case NET_SVC_UPDATE_CHECKING:
        snprintf(text, sizeof(text), "Checking for updates...");
        break;
    case NET_SVC_UPDATE_UP_TO_DATE:
        if (u.latest[0] == '\0') {
            snprintf(text, sizeof(text), "No releases yet\nTap to check again");
        } else {
            snprintf(text, sizeof(text), "Up to date (latest %s)\nTap to check again", u.latest);
        }
        break;
    case NET_SVC_UPDATE_AVAILABLE:
        snprintf(text, sizeof(text), armed ? "Tap again to install %s" : "%s available\nTap to install",
                 u.latest);
        break;
    case NET_SVC_UPDATE_DOWNLOADING:
        snprintf(text, sizeof(text), "Downloading %s  %u%%", u.latest, (unsigned)u.progress);
        break;
    case NET_SVC_UPDATE_INSTALLING:
        snprintf(text, sizeof(text), "Installing %s  %u%%", u.latest, (unsigned)u.progress);
        break;
    case NET_SVC_UPDATE_REBOOTING:
        snprintf(text, sizeof(text), "Installed. Restarting...");
        break;
    case NET_SVC_UPDATE_FAILED:
        snprintf(text, sizeof(text), "Update failed: %s\nTap to retry", u.message);
        break;
    case NET_SVC_UPDATE_IDLE:
    default:
        snprintf(text, sizeof(text), "Tap to check for updates");
        break;
    }

    /* Nothing that needs the network can start without it; say so instead of "tap to ...". */
    bool idle_state = u.state == NET_SVC_UPDATE_IDLE || u.state == NET_SVC_UPDATE_UP_TO_DATE ||
                      u.state == NET_SVC_UPDATE_AVAILABLE || u.state == NET_SVC_UPDATE_FAILED;
    if (idle_state && net.state != NET_SVC_CONNECTED) {
        snprintf(text, sizeof(text), "Updates need WiFi");
    }

    app_ui_set_update_status(text);
}

static void on_firmware_tap(void)
{
    net_svc_update_status_t u;
    net_svc_update_get_status(&u);

    if (u.state == NET_SVC_UPDATE_AVAILABLE) {
        if (esp_timer_get_time() < s_update_armed_until_us) {
            s_update_armed_until_us = 0;
            if (net_svc_update_install() != ESP_OK) {
                ESP_LOGW(TAG, "update install refused");
            }
        } else {
            s_update_armed_until_us = esp_timer_get_time() + (int64_t)UPDATE_CONFIRM_MS * 1000;
        }
    } else if (net_svc_update_check() != ESP_OK) {
        ESP_LOGW(TAG, "update check refused (no connection, or busy)");
    }
    update_status_refresh();
}

/* Keeps the settings page's network and update lines current. Runs on the LVGL task. */
static void network_status_timer_cb(lv_timer_t *t)
{
    (void)t;

    net_svc_status_t net;
    net_svc_get_status(&net);

    char detail[96] = {0};

    switch (net.state) {
    case NET_SVC_CONNECTED:
        snprintf(detail, sizeof(detail), "%s\n%s  (%s.local)",
                 net.ssid, net.ip, net.hostname);
        break;
    case NET_SVC_AP_MODE:
        snprintf(detail, sizeof(detail), "Join '%s'\nthen open %s", net.ssid, net.ip);
        break;
    case NET_SVC_CONNECTING:
        snprintf(detail, sizeof(detail), "%s", net.ssid);
        break;
    case NET_SVC_DISABLED:
    case NET_SVC_FAILED:
    default:
        break;
    }

    app_ui_set_network_status(net_svc_state_str(net.state), detail);
    update_status_refresh();
}

/* Runs on the LVGL task, from the settings page's confirmed "Reset network" button. */
static void on_network_reset(void)
{
    esp_err_t err = net_svc_forget_credentials();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network reset failed: %s", esp_err_to_name(err));
    }
    /* network_status_timer_cb shows the setup-network instructions within a second. */
}

/* Why the chip last reset. A brownout reports itself here, which makes power problems visible
 * instead of looking like a flaky USB connection. */
static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB:      return "USB";
    case ESP_RST_JTAG:     return "JTAG";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "unknown";
    }
}

/*
 * Confirm a freshly installed OTA image only once it has run healthily for a while.
 *
 * Confirming as soon as the UI was built proved too early. A test image that panicked a few
 * hundred milliseconds later, during audio start-up, had already confirmed itself, so the
 * bootloader never rolled it back and the board boot-looped until it was reflashed over USB.
 * Waiting until startup has finished and the system has stayed up means a crash anywhere in
 * that window leaves the image unconfirmed, and the bootloader reverts on the next boot.
 */
#define OTA_CONFIRM_AFTER_MS 15000

static void ota_confirm_timer_cb(void *arg)
{
    (void)arg;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "running image confirmed after %d s", OTA_CONFIRM_AFTER_MS / 1000);
    } else if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "could not confirm the running image: %s", esp_err_to_name(err));
    }
}

/* ------------------------------------------------------------- UI watchdog -------- */

/*
 * Catches the LVGL task blocking forever. That has been seen on hardware: the panel showed
 * white and green noise and every LVGL timer, the perf report included, stopped. The task
 * watchdog cannot see it, because a blocked task leaves the idle tasks free to run.
 *
 * A heartbeat timer on the LVGL task bumps a counter; an esp_timer, which does not depend on
 * the LVGL task, checks it. If it stalls, log the memory state and abort. The core dump then
 * shows where every task was waiting, and the reboot recovers a gauge that would otherwise
 * stay dead until the ignition was cycled.
 *
 * Longest legitimate stall measured is a face rebuild under the display lock, ~80ms
 * (docs/performance.md), so the limit is far above anything real.
 */
#define UI_HEARTBEAT_MS   100
#define UI_STALL_LIMIT_MS 5000

static volatile uint32_t s_ui_heartbeat;
static TaskHandle_t      s_ui_task;

static void ui_heartbeat_cb(lv_timer_t *t)
{
    (void)t;
    s_ui_task = xTaskGetCurrentTaskHandle();
    s_ui_heartbeat++;
}

static void ui_watchdog_cb(void *arg)
{
    (void)arg;
    static uint32_t last_beat;
    static uint32_t stalled_ms;

    uint32_t beat = s_ui_heartbeat;
    if (beat != last_beat) {
        last_beat  = beat;
        stalled_ms = 0;
        return;
    }

    stalled_ms += 1000;
    if (stalled_ms < UI_STALL_LIMIT_MS) {
        return;
    }

    ESP_LOGE(TAG, "UI stalled for %" PRIu32 " ms: LVGL task '%s' state %d",
             stalled_ms, s_ui_task ? pcTaskGetName(s_ui_task) : "?",
             s_ui_task ? (int)eTaskGetState(s_ui_task) : -1);
    ESP_LOGE(TAG, "internal free %u B (lowest %u B), largest DMA block %u B, PSRAM free %u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    abort();
}

static void ui_watchdog_start(void)
{
    lv_timer_create(ui_heartbeat_cb, UI_HEARTBEAT_MS, NULL);

    const esp_timer_create_args_t args = {.callback = ui_watchdog_cb, .name = "ui_watchdog"};
    esp_timer_handle_t            timer = NULL;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_periodic(timer, 1000 * 1000) != ESP_OK) {
        ESP_LOGW(TAG, "UI watchdog not started");
    }
}

/* ------------------------------------------------------------------- startup -------- */

void app_main(void)
{
    ESP_LOGW(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));

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

    s_active_cfg = heap_caps_calloc(1, sizeof(*s_active_cfg), MALLOC_CAP_SPIRAM);
    if (s_active_cfg == NULL) {
        /* No PSRAM means no face buffer either, so there is nothing useful to fall back to. */
        ESP_LOGE(TAG, "no PSRAM for the active gauge config");
        return;
    }

    const board_profile_t *board = board_profile_get();
    ESP_LOGI(TAG, "board: %s (%" PRIu16 "x%" PRIu16 ", %" PRIu8 " Hz)",
             board->name, board->width_px, board->height_px, board->refresh_hz);

    char        warning[128] = {0};
    const char *loaded_id    = load_initial_config(warning, sizeof(warning));
    set_active_id(loaded_id != NULL ? loaded_id : "");

    ESP_LOGI(TAG, "config '%s' (%s): channel=%s range=%.1f..%.1f %s warnings=%" PRIu16,
             s_active_cfg->id, loaded_id ? "storage" : "built-in", s_active_cfg->source.channel,
             (double)s_active_cfg->source.min, (double)s_active_cfg->source.max,
             s_active_cfg->source.unit, s_active_cfg->warning_count);

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

    /* Before anything draws, and before WiFi claims its share of internal memory. */
    retarget_draw_buffers(board);

    int64_t t0 = esp_timer_get_time();
    err = app_ui_create(s_active_cfg, board);
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
    app_ui_set_network_reset_cb(on_network_reset);
    app_ui_set_firmware_tap_cb(on_firmware_tap);

    if (warning[0] != '\0') {
        ESP_LOGW(TAG, "%s", warning);
        app_ui_set_warning(warning);
    }

    lv_timer_create(value_timer_cb, VALUE_TICK_MS, NULL);
#if CONFIG_AI_GAUGE_SIMULATED_SOURCE
    ESP_LOGW(TAG, "the needle sweeps SIMULATED values until a sensor board answers");
#endif

    ESP_ERROR_CHECK(gauge_perf_attach(NULL));

#if CONFIG_AI_GAUGE_BENCH_TRANSITION
    lv_timer_create(bench_timer_cb, 1200, NULL);
    ESP_ERROR_CHECK(gauge_perf_start_reporting(5000, "tile transition"));
    ESP_LOGW(TAG, "transition benchmark active; the display will flip tiles continuously");
#else
    ESP_ERROR_CHECK(gauge_perf_start_reporting(5000, "needle sweep"));
#endif

    lv_timer_create(network_status_timer_cb, 1000, NULL);

    ui_watchdog_start();

    bsp_display_unlock();


    /*
     * Sensors after the face is on screen (docs/architecture.md, Startup). The task's stack is in
     * PSRAM, so this costs WiFi almost no internal memory. A missing board is not a failure: the
     * task keeps probing for one.
     */
    if (board->sensor_i2c_port >= 0) {
        const sensor_hub_bus_t sensor_bus = {
            .i2c_port   = board->sensor_i2c_port,
            .sda_gpio   = board->sensor_sda_gpio,
            .scl_gpio   = board->sensor_scl_gpio,
            .alert_gpio = board->sensor_alert_gpio,
            .scl_hz     = board->sensor_i2c_hz,
        };
        if (sensor_hub_start(&sensor_bus) != ESP_OK) {
            ESP_LOGE(TAG, "sensor hub failed to start; the gauge continues without sensors");
        }
    }


    /*
     * Networking starts last and never blocks the display: by this point the gauge is on
     * screen. Starting WiFi before building the UI was tried and was worse, not better -- WiFi
     * initialised 4 of its static RX buffers instead of 5 (docs/performance.md).
     *
     * Wait for the first frames first. Measured: starting WiFi ~80ms after the UI unlock failed
     * intermittently ("Expected to init 6 rx buffer, actual is 5") -- the first full-screen
     * render's transient allocations were still live. Audio initialisation used to supply this
     * pause by accident; since it was removed, the pause is explicit.
     */
    vTaskDelay(pdMS_TO_TICKS(NET_START_SETTLE_MS));

    const net_svc_callbacks_t net_cb = {
        .on_config_changed = on_config_changed,
        .on_telemetry      = on_telemetry,
    };
    bool net_ok = (net_svc_start(&net_cb) == ESP_OK);
    if (!net_ok) {
        ESP_LOGE(TAG, "networking failed to start; the gauge continues without it");
    }

    /* Last thing at startup: arm the delayed confirmation (see ota_confirm_timer_cb). */
    const esp_timer_create_args_t confirm_args = {.callback = ota_confirm_timer_cb, .name = "ota_confirm"};
    esp_timer_handle_t            confirm_timer = NULL;
    /*
     * An image whose networking failed to start cannot be updated remotely, so it must not
     * confirm itself: leaving it unconfirmed lets the bootloader return to the previous
     * image after the next reset. A missing WiFi network does not count as a failure --
     * net_svc_start() only fails when the stack itself cannot initialise.
     */
    if (!net_ok) {
        ESP_LOGW(TAG, "not confirming this image: networking failed to start");
    } else if (esp_timer_create(&confirm_args, &confirm_timer) == ESP_OK) {
        esp_timer_start_once(confirm_timer, (uint64_t)OTA_CONFIRM_AFTER_MS * 1000);
    }

    /*
     * The largest free DMA-capable block, not total free internal heap, is what predicts
     * whether the QSPI driver can still queue a transfer. Bringing WiFi up eats into it, and
     * when it runs out the display fails outright rather than merely slowing down.
     */
    ESP_LOGI(TAG, "running. internal free: %u B (largest DMA block %u B), PSRAM free: %u KB",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
