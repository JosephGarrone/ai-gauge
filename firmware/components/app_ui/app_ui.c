/*
 * Screens and navigation. See app_ui.h.
 *
 * The dial tile is kept deliberately bare: everything on it costs frame budget every time
 * the needle moves (docs/display-pipeline.md). The settings tile has more latitude, since it
 * is not in the driver's field of view while the vehicle is moving.
 */

#include "app_ui.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/display.h"

#include "app_settings.h"
#include "gauge_perf.h"
#include "gauge_store.h"

static const char *TAG = "app_ui";

/*
 * The whole panel is 1.75" across (~266 ppi), so the settings page is sized for a finger, not a
 * stylus: 26px text is about the smallest that reads comfortably at arm's length, and controls
 * are at least 54px tall.
 */

/* Round panels clip their corners, so settings content is inset from the edge. */
#define SETTINGS_SIDE_PAD 44
#define SETTINGS_TOP_PAD  56
/* Generous, so the last rows can be scrolled up into the wide middle of the panel. */
#define SETTINGS_BOTTOM_PAD 140
#define SETTINGS_ROW_GAP    28

#define SETTINGS_CONTROL_H 64
#define SETTINGS_SWITCH_W  100
#define SETTINGS_SWITCH_H  54

#define SETTINGS_FONT (&lv_font_montserrat_26)
#if LV_FONT_MONTSERRAT_32
#define SETTINGS_TITLE_FONT (&lv_font_montserrat_32)
#else
/* sdkconfig predates CONFIG_LV_FONT_MONTSERRAT_32 (see sdkconfig.defaults). */
#define SETTINGS_TITLE_FONT (&lv_font_montserrat_26)
#endif

#define COLOR_CAPTION 0x9e9e9e
#define COLOR_VALUE   0xffffff

#define COLOR_RESET_IDLE  0x37373d
#define COLOR_RESET_ARMED 0xc62828

/* How long "Reset network" waits for the confirming second tap. */
#define NET_RESET_CONFIRM_MS 4000

/*
 * Rotation is done by the panel (MADCTL), not by LVGL. The BSP registers this QSPI panel with
 * the adapter as interface OTHER, for which the adapter never rotates frames -- and a hardware
 * rotation costs nothing per frame anyway. The panel is square, so LVGL's resolution does not
 * change and nothing needs rebuilding.
 *
 * LVGL is still told, purely so it maps touch input to match. Degrees in the settings are
 * clockwise; LVGL counts the other way, hence 90 <-> 270 in its column. If taps land mirrored
 * at 90 and 270 on hardware, swap the two LVGL entries.
 */
static const struct {
    bsp_display_rotation_t panel;
    lv_display_rotation_t  lvgl;
} k_rotations[APP_SETTINGS_ROTATION_COUNT] = {
    [APP_SETTINGS_ROTATION_0]   = {BSP_DISPLAY_ROTATE_0,   LV_DISPLAY_ROTATION_0},
    [APP_SETTINGS_ROTATION_90]  = {BSP_DISPLAY_ROTATE_90,  LV_DISPLAY_ROTATION_270},
    [APP_SETTINGS_ROTATION_180] = {BSP_DISPLAY_ROTATE_180, LV_DISPLAY_ROTATION_180},
    [APP_SETTINGS_ROTATION_270] = {BSP_DISPLAY_ROTATE_270, LV_DISPLAY_ROTATION_90},
};

static const char *const k_rotation_map[] = {
    "0" "\xC2\xB0", "90" "\xC2\xB0", "180" "\xC2\xB0", "270" "\xC2\xB0", "",
};

static struct {
    lv_obj_t *tileview;
    lv_obj_t *tile_gauge;
    lv_obj_t *tile_settings;

    gauge_render_t *gauge;

    lv_obj_t *fps_badge;      /**< On the dial tile, optional. */
    lv_obj_t *perf_label;     /**< On the settings tile. */
    lv_obj_t *warning_label;
    lv_obj_t *heap_label;
    lv_obj_t *gauge_dropdown;
    lv_obj_t *gauge_info_label;
    lv_obj_t *network_label;

    lv_obj_t   *net_reset_btn;
    lv_obj_t   *net_reset_label;
    lv_timer_t *net_reset_timer; /**< Disarms the button if no second tap comes. */
    bool        net_reset_armed;

    app_ui_network_reset_cb_t on_network_reset;

    /* Ids backing the dropdown, in the order they appear in it. */
    char gauge_ids[GAUGE_STORE_MAX_GAUGES][GAUGE_CONFIG_MAX_ID_LEN];
    int  gauge_count;

    app_ui_gauge_selected_cb_t on_gauge_selected;

    gauge_render_alert_cb_t alert_cb;
    void                   *alert_cb_user;

    const board_profile_t *board;
} s;

static void update_gauge_info(const gauge_config_t *cfg)
{
    if (s.gauge_info_label == NULL) {
        return;
    }
    lv_label_set_text_fmt(s.gauge_info_label, "%s\n%.0f-%.0f %s", cfg->id,
                          (double)cfg->source.min, (double)cfg->source.max,
                          cfg->source.unit);
}

static void apply_rotation(app_settings_rotation_t rotation)
{
    if ((unsigned)rotation >= APP_SETTINGS_ROTATION_COUNT) {
        return;
    }

    esp_err_t err = bsp_display_rotation_set(k_rotations[rotation].panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel rotation failed: %s", esp_err_to_name(err));
        return;
    }

    lv_display_set_rotation(NULL, k_rotations[rotation].lvgl);

    /* What is on the glass was drawn for the old orientation. */
    lv_obj_invalidate(lv_screen_active());
}

static void apply_rotation_async_cb(void *arg)
{
    apply_rotation((app_settings_rotation_t)(uintptr_t)arg);
}

/* ---------------------------------------------------------------- settings UI ------- */

static lv_obj_t *add_row(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 8, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    if (caption != NULL) {
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, caption);
        lv_obj_set_style_text_font(label, SETTINGS_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CAPTION), LV_PART_MAIN);
    }

    return row;
}

/* A wrapping read-only value under a row's caption. */
static lv_obj_t *add_value_label(lv_obj_t *row, const char *text, uint32_t color)
{
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, SETTINGS_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    return label;
}

/* Caption on the left, switch on the right. The whole row height is a comfortable target. */
static lv_obj_t *add_toggle_row(lv_obj_t *parent, const char *caption, bool on,
                                lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, SETTINGS_CONTROL_H, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, caption);
    lv_obj_set_style_text_font(label, SETTINGS_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_CAPTION), LV_PART_MAIN);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, SETTINGS_SWITCH_W, SETTINGS_SWITCH_H);
    lv_obj_set_ext_click_area(sw, 12);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);

    return sw;
}

static void brightness_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t   value  = lv_slider_get_value(slider);

    /* Apply immediately so the change is visible while dragging... */
    bsp_display_brightness_set((int)value);
    app_settings_set_brightness((uint8_t)value);

    /* ...but only write flash when the interaction ends. */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_settings_commit();
    }
}

static void rotation_changed_cb(lv_event_t *e)
{
    lv_obj_t *bm  = lv_event_get_target(e);
    uint32_t  idx = lv_buttonmatrix_get_selected_button(bm);

    if (idx >= APP_SETTINGS_ROTATION_COUNT) {
        return; /* LV_BUTTONMATRIX_BUTTON_NONE */
    }

    app_settings_rotation_t rotation = (app_settings_rotation_t)idx;
    if (rotation == app_settings_get()->rotation) {
        return;
    }

    app_settings_set_rotation(rotation);
    app_settings_commit();

    /*
     * Rotating remaps touch coordinates, so LVGL sees the finger jump. Done while this tap is
     * still being processed, the buttonmatrix ended up checking the wrong button (or none), so
     * wait until the tap has been fully handled.
     */
    lv_async_call(apply_rotation_async_cb, (void *)(uintptr_t)rotation);
}

static void set_net_reset_armed(bool armed)
{
    s.net_reset_armed = armed;
    lv_label_set_text(s.net_reset_label, armed ? "Tap again to reset" : "Reset network");
    lv_obj_set_style_bg_color(s.net_reset_btn,
                              lv_color_hex(armed ? COLOR_RESET_ARMED : COLOR_RESET_IDLE),
                              LV_PART_MAIN);
}

static void net_reset_disarm_cb(lv_timer_t *t)
{
    (void)t;
    s.net_reset_timer = NULL; /* one-shot: LVGL deletes it after this returns */
    set_net_reset_armed(false);
}

/* Two taps: dropping the connection from a stray touch in a moving vehicle would be a nuisance. */
static void network_reset_clicked_cb(lv_event_t *e)
{
    (void)e;

    if (!s.net_reset_armed) {
        set_net_reset_armed(true);
        s.net_reset_timer = lv_timer_create(net_reset_disarm_cb, NET_RESET_CONFIRM_MS, NULL);
        lv_timer_set_repeat_count(s.net_reset_timer, 1);
        return;
    }

    if (s.net_reset_timer != NULL) {
        lv_timer_delete(s.net_reset_timer);
        s.net_reset_timer = NULL;
    }
    set_net_reset_armed(false);

    if (s.on_network_reset != NULL) {
        s.on_network_reset();
    }
}

static void show_fps_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw   = lv_event_get_target(e);
    bool      show = lv_obj_has_state(sw, LV_STATE_CHECKED);

    app_settings_set_show_fps(show);
    app_settings_commit();

    if (s.fps_badge != NULL) {
        if (show) {
            lv_obj_remove_flag(s.fps_badge, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s.fps_badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void gauge_selected_cb(lv_event_t *e)
{
    lv_obj_t *dd  = lv_event_get_target(e);
    uint32_t  idx = lv_dropdown_get_selected(dd);

    if ((int)idx >= s.gauge_count || s.on_gauge_selected == NULL) {
        return;
    }

    /*
     * Hand the id back rather than loading it here: app_ui knows about widgets, not about
     * the filesystem. The owner decides what a selection means.
     */
    s.on_gauge_selected(s.gauge_ids[idx]);
}

/* Tapping the dial clears the peak marker. LVGL does not report a swipe as a click. */
static void dial_tapped_cb(lv_event_t *e)
{
    (void)e;
    if (s.gauge != NULL) {
        gauge_render_reset_peak(s.gauge);
    }
}

static void alert_sound_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    app_settings_set_alert_sound(lv_obj_has_state(sw, LV_STATE_CHECKED));
    app_settings_commit();
}

/* Refreshes the live numbers on the settings page and the optional badge on the dial. */
static void status_timer_cb(lv_timer_t *t)
{
    (void)t;

    gauge_perf_stats_t st;
    gauge_perf_get(&st);

    /* Only touch the badge when it is actually visible -- it sits on the gauge tile. */
    if (s.fps_badge != NULL && !lv_obj_has_flag(s.fps_badge, LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text_fmt(s.fps_badge, "%.0f fps", (double)st.fps);
    }

    if (s.perf_label != NULL && app_ui_get_tile() == APP_UI_TILE_SETTINGS) {
        lv_label_set_text_fmt(s.perf_label,
                              "%.1f fps\n%.1f%% dirty\n%.2f ms render",
                              (double)st.fps, (double)st.dirty_pct_mean,
                              (double)st.render_ms_mean);
    }

    if (s.heap_label != NULL && app_ui_get_tile() == APP_UI_TILE_SETTINGS) {
        lv_label_set_text_fmt(s.heap_label, "%u KB internal\n%u KB PSRAM",
                              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

static void build_settings_tile(lv_obj_t *tile, const gauge_config_t *cfg)
{
    const app_settings_t *set = app_settings_get();

    lv_obj_set_style_bg_color(tile, lv_color_hex(0x0b0b0d), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *col = lv_obj_create(tile);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(col, SETTINGS_SIDE_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(col, SETTINGS_TOP_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(col, SETTINGS_BOTTOM_PAD, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, SETTINGS_ROW_GAP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    /* --- heading --- */
    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, SETTINGS_TITLE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_VALUE), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(title, LV_PCT(100));

    /* --- brightness --- */
    lv_obj_t *bright_row = add_row(col, "Brightness");
    /*
     * The knob is larger than the track, so give it room: inset the row so the knob stays on
     * the glass at either end, and let it draw outside the row's bounds vertically.
     */
    lv_obj_set_style_pad_hor(bright_row, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_row(bright_row, 26, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(bright_row, 14, LV_PART_MAIN);
    lv_obj_add_flag(bright_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *slider = lv_slider_create(bright_row);
    lv_obj_set_size(slider, LV_PCT(100), 20);
    lv_obj_set_style_pad_all(slider, 14, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 24);
    lv_slider_set_range(slider, APP_SETTINGS_BRIGHTNESS_MIN, APP_SETTINGS_BRIGHTNESS_MAX);
    lv_slider_set_value(slider, set->brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_changed_cb, LV_EVENT_RELEASED, NULL);

    /* --- rotation --- */
    lv_obj_t *rot_row = add_row(col, "Rotation");
    lv_obj_t *rot     = lv_buttonmatrix_create(rot_row);
    lv_buttonmatrix_set_map(rot, k_rotation_map);
    /* CLICK_TRIG: report the change on release, after the button has been checked. */
    lv_buttonmatrix_set_button_ctrl_all(
        rot, (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG));
    lv_buttonmatrix_set_one_checked(rot, true);
    lv_buttonmatrix_set_button_ctrl(rot, (uint32_t)set->rotation, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(rot, LV_PCT(100), SETTINGS_CONTROL_H);
    lv_obj_set_style_pad_all(rot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(rot, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(rot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_text_font(rot, SETTINGS_FONT, LV_PART_ITEMS);
    lv_obj_add_event_cb(rot, rotation_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- toggles --- */
    add_toggle_row(col, "Show FPS", set->show_fps, show_fps_changed_cb);
    add_toggle_row(col, "Alert sound", set->alert_sound, alert_sound_changed_cb);

    /* --- gauge picker --- */
    lv_obj_t *gauge_row = add_row(col, "Gauge");

    s.gauge_dropdown = lv_dropdown_create(gauge_row);
    lv_obj_set_width(s.gauge_dropdown, LV_PCT(100));
    lv_obj_set_style_min_height(s.gauge_dropdown, SETTINGS_CONTROL_H, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s.gauge_dropdown, 16, LV_PART_MAIN);
    lv_obj_set_style_text_font(s.gauge_dropdown, SETTINGS_FONT, LV_PART_MAIN);
    lv_dropdown_set_options(s.gauge_dropdown, "(built-in)");
    lv_obj_add_event_cb(s.gauge_dropdown, gauge_selected_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* The open list is a separate object; space its options out so each is easy to hit. */
    lv_obj_t *dd_list = lv_dropdown_get_list(s.gauge_dropdown);
    lv_obj_set_style_text_font(dd_list, SETTINGS_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(dd_list, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(dd_list, 14, LV_PART_MAIN);

    s.gauge_info_label = add_value_label(gauge_row, "", COLOR_VALUE);
    update_gauge_info(cfg);

    /* --- network --- */
    lv_obj_t *net_row = add_row(col, "Network");
    s.network_label   = add_value_label(net_row, "starting...", COLOR_VALUE);

    s.net_reset_btn = lv_button_create(net_row);
    lv_obj_set_size(s.net_reset_btn, LV_PCT(100), SETTINGS_CONTROL_H);
    lv_obj_set_style_margin_top(s.net_reset_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s.net_reset_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s.net_reset_btn, network_reset_clicked_cb, LV_EVENT_CLICKED, NULL);

    s.net_reset_label = lv_label_create(s.net_reset_btn);
    lv_obj_set_style_text_font(s.net_reset_label, SETTINGS_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s.net_reset_label, lv_color_hex(COLOR_VALUE), LV_PART_MAIN);
    lv_obj_center(s.net_reset_label);
    set_net_reset_armed(false);

    /* --- live performance --- */
    lv_obj_t *perf_row = add_row(col, "Performance");
    s.perf_label       = add_value_label(perf_row, "measuring...", 0x00c853);

    /* --- memory --- */
    lv_obj_t *heap_row = add_row(col, "Free memory");
    s.heap_label       = add_value_label(heap_row, "-", COLOR_VALUE);

    /* --- firmware version --- */
    const esp_app_desc_t *desc    = esp_app_get_description();
    lv_obj_t             *ver_row = add_row(col, "Firmware");
    add_value_label(ver_row, desc ? desc->version : "unknown", COLOR_VALUE);

    /* --- warnings, hidden unless there is something to say --- */
    s.warning_label = add_value_label(col, "", 0xffab00);
    lv_obj_add_flag(s.warning_label, LV_OBJ_FLAG_HIDDEN);
}

/* --------------------------------------------------------------------- public ------- */

esp_err_t app_ui_create(const gauge_config_t *cfg, const board_profile_t *board)
{
    ESP_RETURN_ON_FALSE(cfg && board, ESP_ERR_INVALID_ARG, TAG, "bad args");

    s.board = board;

    apply_rotation(app_settings_get()->rotation);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s.tileview = lv_tileview_create(scr);
    ESP_RETURN_ON_FALSE(s.tileview != NULL, ESP_ERR_NO_MEM, TAG, "tileview failed");
    lv_obj_set_size(s.tileview, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.tileview, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.tileview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s.tileview, LV_SCROLLBAR_MODE_OFF);

    /* Dial at row 0, settings at row 1: swipe up from the dial, down to return. */
    s.tile_gauge    = lv_tileview_add_tile(s.tileview, 0, 0, LV_DIR_BOTTOM);
    s.tile_settings = lv_tileview_add_tile(s.tileview, 0, 1, LV_DIR_TOP);
    ESP_RETURN_ON_FALSE(s.tile_gauge && s.tile_settings, ESP_ERR_NO_MEM, TAG, "tiles failed");

    /*
     * Tiles inherit the default theme's padding and border, which shifts the content origin
     * so anything placed at (0,0) -- the full-panel face canvas included -- lands offset.
     *
     * Zero those properties individually rather than calling lv_obj_remove_style_all():
     * lv_tileview_add_tile() stores the tile's own position as a *local* style
     * (lv_pct(row * 100)), so removing all styles collapses every tile onto row 0 and
     * navigation stops working.
     */
    lv_obj_t *tiles[] = {s.tile_gauge, s.tile_settings};
    for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]); i++) {
        lv_obj_set_style_pad_all(tiles[i], 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(tiles[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(tiles[i], 0, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(tiles[i], LV_SCROLLBAR_MODE_OFF);
    }

    lv_obj_remove_flag(s.tile_gauge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s.tile_gauge, dial_tapped_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(s.tile_gauge, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.tile_gauge, LV_OPA_COVER, LV_PART_MAIN);

    esp_err_t err = gauge_render_create(s.tile_gauge, cfg, board, &s.gauge);
    ESP_RETURN_ON_ERROR(err, TAG, "gauge_render_create failed");
    gauge_render_set_alert_cb(s.gauge, s.alert_cb, s.alert_cb_user);

    /*
     * FPS badge. Created regardless so the toggle has something to act on, but hidden by
     * default -- anything permanently on the dial tile costs frame budget on every update.
     */
    s.fps_badge = lv_label_create(s.tile_gauge);
    lv_label_set_text(s.fps_badge, "-- fps");
    lv_obj_set_style_text_font(s.fps_badge, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s.fps_badge, lv_color_hex(0x00c853), LV_PART_MAIN);
    /*
     * Round panels lose their corners, so the usable width narrows sharply towards the top
     * and bottom edges. Sit the badge just below centre where the panel is at its widest and
     * the face is otherwise empty.
     */
    lv_obj_align(s.fps_badge, LV_ALIGN_CENTER, 0, 60);
    if (!app_settings_get()->show_fps) {
        lv_obj_add_flag(s.fps_badge, LV_OBJ_FLAG_HIDDEN);
    }

    build_settings_tile(s.tile_settings, cfg);

    /* 500ms is frequent enough to feel live without dirtying the dial needlessly. */
    lv_timer_create(status_timer_cb, 500, NULL);

    lv_tileview_set_tile_by_index(s.tileview, 0, APP_UI_TILE_GAUGE, LV_ANIM_OFF);

    ESP_LOGI(TAG, "screens ready");
    return ESP_OK;
}

gauge_render_t *app_ui_get_gauge(void)
{
    return s.gauge;
}

esp_err_t app_ui_set_config(const gauge_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
    ESP_RETURN_ON_FALSE(s.tile_gauge != NULL, ESP_ERR_INVALID_STATE, TAG, "no UI");

    gauge_render_t *old_gauge = s.gauge;
    gauge_render_t *replacement = NULL;

    /*
     * Build the replacement before destroying the old one. If it fails -- most likely on the
     * PSRAM allocation for the face -- the gauge on screen keeps working rather than the
     * driver being left with nothing.
     */
    esp_err_t err = gauge_render_create(s.tile_gauge, cfg, s.board, &replacement);
    ESP_RETURN_ON_ERROR(err, TAG, "could not build the replacement gauge");

    s.gauge = replacement;
    gauge_render_set_alert_cb(s.gauge, s.alert_cb, s.alert_cb_user);

    if (old_gauge != NULL) {
        gauge_render_destroy(old_gauge);
    }

    /* The new gauge objects were created after the badge, so lift it back on top. */
    if (s.fps_badge != NULL) {
        lv_obj_move_foreground(s.fps_badge);
    }

    update_gauge_info(cfg);

    ESP_LOGI(TAG, "switched to gauge '%s'", cfg->id);
    return ESP_OK;
}

void app_ui_set_gauge_list(const char (*ids)[GAUGE_CONFIG_MAX_ID_LEN], int count,
                           const char *active)
{
    if (s.gauge_dropdown == NULL) {
        return;
    }

    if (count > GAUGE_STORE_MAX_GAUGES) {
        count = GAUGE_STORE_MAX_GAUGES;
    }

    if (ids == NULL || count <= 0) {
        /* Nothing on the filesystem: say so rather than showing an empty picker. */
        s.gauge_count = 0;
        lv_dropdown_set_options(s.gauge_dropdown, "(built-in)");
        lv_obj_add_state(s.gauge_dropdown, LV_STATE_DISABLED);
        return;
    }

    lv_obj_remove_state(s.gauge_dropdown, LV_STATE_DISABLED);

    char options[GAUGE_STORE_MAX_GAUGES * (GAUGE_CONFIG_MAX_ID_LEN + 1)];
    size_t used     = 0;
    int    selected = 0;

    for (int i = 0; i < count; i++) {
        snprintf(s.gauge_ids[i], GAUGE_CONFIG_MAX_ID_LEN, "%s", ids[i]);

        int n = snprintf(options + used, sizeof(options) - used, "%s%s",
                         (i == 0) ? "" : "\n", ids[i]);
        if (n < 0 || (size_t)n >= sizeof(options) - used) {
            count = i; /* ran out of room; show what fits */
            break;
        }
        used += (size_t)n;

        if (active != NULL && strcmp(ids[i], active) == 0) {
            selected = i;
        }
    }

    s.gauge_count = count;
    lv_dropdown_set_options(s.gauge_dropdown, options);
    lv_dropdown_set_selected(s.gauge_dropdown, (uint32_t)selected);
}

void app_ui_set_gauge_selected_cb(app_ui_gauge_selected_cb_t cb)
{
    s.on_gauge_selected = cb;
}

void app_ui_set_network_reset_cb(app_ui_network_reset_cb_t cb)
{
    s.on_network_reset = cb;
}

void app_ui_set_alert_cb(gauge_render_alert_cb_t cb, void *user_data)
{
    s.alert_cb      = cb;
    s.alert_cb_user = user_data;
    if (s.gauge != NULL) {
        gauge_render_set_alert_cb(s.gauge, cb, user_data);
    }
}

void app_ui_show_tile(app_ui_tile_t tile, bool animate)
{
    if (s.tileview == NULL) {
        return;
    }
    lv_tileview_set_tile_by_index(s.tileview, 0, (uint32_t)tile,
                                  animate ? LV_ANIM_ON : LV_ANIM_OFF);
}

app_ui_tile_t app_ui_get_tile(void)
{
    if (s.tileview == NULL) {
        return APP_UI_TILE_GAUGE;
    }

    /* Derive from scroll position: the tileview reports the tile it has settled on. */
    lv_obj_t *active = lv_tileview_get_tile_active(s.tileview);
    return (active == s.tile_settings) ? APP_UI_TILE_SETTINGS : APP_UI_TILE_GAUGE;
}

void app_ui_set_network_status(const char *state, const char *detail)
{
    if (s.network_label == NULL) {
        return;
    }

    if (detail != NULL && detail[0] != '\0') {
        lv_label_set_text_fmt(s.network_label, "%s\n%s", state, detail);
    } else {
        lv_label_set_text(s.network_label, state);
    }
}

void app_ui_set_warning(const char *text)
{
    if (s.warning_label == NULL) {
        return;
    }

    if (text == NULL || text[0] == '\0') {
        lv_label_set_text(s.warning_label, "");
        lv_obj_add_flag(s.warning_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s.warning_label, text);
    lv_obj_remove_flag(s.warning_label, LV_OBJ_FLAG_HIDDEN);
}
