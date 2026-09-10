/*
 * AI-Gauge entry point.
 *
 * Milestone 1, step 2: bring-up smoke test. This confirms the toolchain, octal PSRAM, the
 * CO5300 QSPI panel, touch and the gauge XML parser all work before anything is built on top
 * of them. The dial renderer replaces this screen in a later step.
 *
 * Startup ordering matters and is documented in docs/architecture.md: the gauge must show
 * something before the network is touched.
 */

#include <inttypes.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "board_profile.h"
#include "gauge_config.h"

static const char *TAG = "app_main";

/* Milestone-1 placeholder screen: proves the panel, PSRAM and parser are all alive. */
static void build_smoke_screen(const board_profile_t *board, const gauge_config_t *cfg)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, board->width_px - (board->safe_inset_px * 4), LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 8, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "AI-GAUGE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xff1744), LV_PART_MAIN);

    lv_obj_t *panel = lv_label_create(col);
    lv_label_set_text_fmt(panel, "%" PRIu16 " x %" PRIu16 " %s",
                          board->width_px, board->height_px,
                          board->shape == BOARD_PANEL_ROUND ? "round" : "square");
    lv_obj_set_style_text_font(panel, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(panel, lv_color_hex(0x9e9e9e), LV_PART_MAIN);

    lv_obj_t *conf = lv_label_create(col);
    lv_label_set_text_fmt(conf, "config: %s\n%s %.0f-%.0f %s",
                          cfg->id, cfg->source.channel,
                          (double)cfg->source.min, (double)cfg->source.max,
                          cfg->source.unit);
    lv_obj_set_style_text_align(conf, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(conf, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(conf, lv_color_hex(0xffffff), LV_PART_MAIN);

    lv_obj_t *mem = lv_label_create(col);
    lv_label_set_text_fmt(mem, "PSRAM free: %u KB",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_obj_set_style_text_font(mem, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(mem, lv_color_hex(0x00c853), LV_PART_MAIN);
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
     * the same path as any other config, so this also smoke-tests the parser on target.
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
    build_smoke_screen(board, cfg);
    bsp_display_unlock();

    ESP_LOGI(TAG, "display up; free internal heap: %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
