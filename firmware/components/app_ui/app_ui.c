/*
 * Screens and navigation. See app_ui.h.
 *
 * The dial tile is kept deliberately bare: everything on it costs frame budget every time
 * the needle moves (docs/display-pipeline.md). The settings tile has more latitude, since it
 * is not in the driver's field of view while the vehicle is moving.
 */

#include "app_ui.h"

#include <inttypes.h>
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

/* Round panels clip their corners, so settings content is inset from the edge. */
#define SETTINGS_SIDE_PAD 56

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
    lv_label_set_text_fmt(s.gauge_info_label, "%s  %.0f-%.0f %s", cfg->id,
                          (double)cfg->source.min, (double)cfg->source.max,
                          cfg->source.unit);
}

/* ---------------------------------------------------------------- settings UI ------- */

static lv_obj_t *add_row(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(row, 4, LV_PART_MAIN);

    if (caption != NULL) {
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, caption);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(0x9e9e9e), LV_PART_MAIN);
    }

    return row;
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
                              "%.1f fps  %.1f%% dirty\n%.2f ms render",
                              (double)st.fps, (double)st.dirty_pct_mean,
                              (double)st.render_ms_mean);
    }

    if (s.heap_label != NULL && app_ui_get_tile() == APP_UI_TILE_SETTINGS) {
        lv_label_set_text_fmt(s.heap_label, "%u KB internal  %u KB PSRAM",
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
    lv_obj_set_style_pad_ver(col, 40, LV_PART_MAIN);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 14, LV_PART_MAIN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    /* --- heading --- */
    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(title, LV_PCT(100));

    /* --- brightness --- */
    lv_obj_t *bright_row = add_row(col, "Brightness");
    lv_obj_t *slider     = lv_slider_create(bright_row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_slider_set_range(slider, APP_SETTINGS_BRIGHTNESS_MIN, APP_SETTINGS_BRIGHTNESS_MAX);
    lv_slider_set_value(slider, set->brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_changed_cb, LV_EVENT_RELEASED, NULL);

    /* --- FPS badge toggle --- */
    lv_obj_t *fps_row = lv_obj_create(col);
    lv_obj_remove_style_all(fps_row);
    lv_obj_set_width(fps_row, LV_PCT(100));
    lv_obj_set_height(fps_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fps_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fps_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *fps_caption = lv_label_create(fps_row);
    lv_label_set_text(fps_caption, "Show FPS");
    lv_obj_set_style_text_font(fps_caption, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(fps_caption, lv_color_hex(0x9e9e9e), LV_PART_MAIN);

    lv_obj_t *sw = lv_switch_create(fps_row);
    if (set->show_fps) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, show_fps_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- alert sound toggle --- */
    lv_obj_t *sound_row = lv_obj_create(col);
    lv_obj_remove_style_all(sound_row);
    lv_obj_set_width(sound_row, LV_PCT(100));
    lv_obj_set_height(sound_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sound_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sound_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *sound_caption = lv_label_create(sound_row);
    lv_label_set_text(sound_caption, "Alert sound");
    lv_obj_set_style_text_font(sound_caption, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sound_caption, lv_color_hex(0x9e9e9e), LV_PART_MAIN);

    lv_obj_t *sound_sw = lv_switch_create(sound_row);
    if (set->alert_sound) {
        lv_obj_add_state(sound_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sound_sw, alert_sound_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- network --- */
    lv_obj_t *net_row = add_row(col, "Network");
    s.network_label   = lv_label_create(net_row);
    lv_label_set_long_mode(s.network_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s.network_label, LV_PCT(100));
    lv_label_set_text(s.network_label, "starting...");
    lv_obj_set_style_text_font(s.network_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s.network_label, lv_color_hex(0xffffff), LV_PART_MAIN);

    /* --- live performance --- */
    lv_obj_t *perf_row = add_row(col, "Performance");
    s.perf_label       = lv_label_create(perf_row);
    lv_label_set_text(s.perf_label, "measuring...");
    lv_obj_set_style_text_font(s.perf_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s.perf_label, lv_color_hex(0x00c853), LV_PART_MAIN);

    /* --- gauge picker --- */
    lv_obj_t *gauge_row = add_row(col, "Gauge");

    s.gauge_dropdown = lv_dropdown_create(gauge_row);
    lv_obj_set_width(s.gauge_dropdown, LV_PCT(100));
    lv_dropdown_set_options(s.gauge_dropdown, "(built-in)");
    lv_obj_add_event_cb(s.gauge_dropdown, gauge_selected_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s.gauge_info_label = lv_label_create(gauge_row);
    lv_obj_set_style_text_font(s.gauge_info_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s.gauge_info_label, lv_color_hex(0xffffff), LV_PART_MAIN);
    update_gauge_info(cfg);

    /* --- memory --- */
    lv_obj_t *heap_row = add_row(col, "Free memory");
    s.heap_label       = lv_label_create(heap_row);
    lv_label_set_text(s.heap_label, "-");
    lv_obj_set_style_text_font(s.heap_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s.heap_label, lv_color_hex(0xffffff), LV_PART_MAIN);

    /* --- firmware version --- */
    const esp_app_desc_t *desc     = esp_app_get_description();
    lv_obj_t             *ver_row  = add_row(col, "Firmware");
    lv_obj_t             *ver      = lv_label_create(ver_row);
    lv_label_set_text_fmt(ver, "%s", desc ? desc->version : "unknown");
    lv_obj_set_style_text_font(ver, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(ver, lv_color_hex(0xffffff), LV_PART_MAIN);

    /* --- warnings, hidden unless there is something to say --- */
    s.warning_label = lv_label_create(col);
    lv_label_set_long_mode(s.warning_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s.warning_label, LV_PCT(100));
    lv_obj_set_style_text_font(s.warning_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s.warning_label, lv_color_hex(0xffab00), LV_PART_MAIN);
    lv_obj_add_flag(s.warning_label, LV_OBJ_FLAG_HIDDEN);
}

/* --------------------------------------------------------------------- public ------- */

esp_err_t app_ui_create(const gauge_config_t *cfg, const board_profile_t *board)
{
    ESP_RETURN_ON_FALSE(cfg && board, ESP_ERR_INVALID_ARG, TAG, "bad args");

    s.board = board;

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
        lv_obj_add_flag(s.warning_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(s.warning_label, text);
    lv_obj_remove_flag(s.warning_label, LV_OBJ_FLAG_HIDDEN);
}
