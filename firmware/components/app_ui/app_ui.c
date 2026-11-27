/*
 * Screens and navigation. See app_ui.h.
 *
 * The dial tile is kept deliberately bare: everything on it costs frame budget every time
 * the needle moves (docs/display-pipeline.md). The settings tile is app_ui_settings.c
 * (docs/settings-ui.md).
 */

#include "app_ui.h"
#include "app_ui_priv.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"

#include "bsp/display.h"

#include "app_settings.h"
#include "gauge_perf.h"

static const char *TAG = "app_ui";

/* How long a long-press shows min/max before the live readout returns. */
#define MINMAX_RECALL_MS 3000

/* A dial "tap" that moved further than this was a swipe, and must not clear the peak. */
#define TAP_SLOP_PX 20

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

/*
 * Frame-rate cap, as the period of LVGL's display refresh timer. Each is 1ms shorter than the
 * target's frame time: measured on hardware, the real interval runs about 1ms longer than the
 * period (the timer is serviced on 1ms ticks after the previous frame's work), so 33/22/16ms gave
 * 29.2, 43.3 and 59.2 fps -- and "60" must not land below 60. "Off" is a 1ms period: the display
 * redraws as soon as a frame is rendered and the value has moved, so the rate is set by render
 * time, faster than the 60 Hz panel can show. Measurements: docs/performance.md.
 */
static const uint32_t k_fps_cap_period_ms[APP_SETTINGS_FPS_CAP_COUNT] = {
    [APP_SETTINGS_FPS_CAP_30]       = 32,
    [APP_SETTINGS_FPS_CAP_45]       = 21,
    [APP_SETTINGS_FPS_CAP_60]       = 15,
    [APP_SETTINGS_FPS_CAP_UNCAPPED] = 1,
};

static struct {
    lv_obj_t *tileview;
    lv_obj_t *tile_gauge;
    lv_obj_t *tile_settings;

    gauge_render_t *gauge;
    lv_obj_t       *fps_badge; /**< On the dial tile, optional. */

    lv_timer_t *shift_timer;   /**< Pixel shift; NULL when off. */
    uint8_t     shift_step;

    /* Engine-off screen, on lv_layer_top(). Exists only while shown. */
    bool        engine_running;
    lv_obj_t   *off_screen;
    lv_obj_t   *off_notice;
    lv_timer_t *off_timer;      /**< Moves the notice. */
    lv_timer_t *peek_timer;     /**< A tap shows the dial for a while; this brings the screen back. */
    lv_point_t      press_at;  /**< Where the current touch on the dial began. */

    gauge_render_alert_cb_t alert_cb;
    void                   *alert_cb_user;

    const board_profile_t *board;
    char unit[GAUGE_CONFIG_MAX_UNIT_LEN]; /**< The unit of the face on screen. */
} s;

/* ------------------------------------------------------------ shared with settings -- */

void app_ui_priv_apply_rotation(app_settings_rotation_t rotation)
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

void app_ui_priv_apply_fps_cap(app_settings_fps_cap_t cap)
{
    lv_display_t *disp  = lv_display_get_default();
    lv_timer_t   *timer = (disp != NULL) ? lv_display_get_refr_timer(disp) : NULL;
    if (timer == NULL || (unsigned)cap >= APP_SETTINGS_FPS_CAP_COUNT) {
        return;
    }
    lv_timer_set_period(timer, k_fps_cap_period_ms[cap]);
    ESP_LOGI(TAG, "refresh period %" PRIu32 " ms", k_fps_cap_period_ms[cap]);
}

void app_ui_priv_show_fps_badge(bool show)
{
    if (s.fps_badge == NULL) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(s.fps_badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s.fps_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------ gauge preferences --- */

static bool unit_is(const char *unit, const char *want)
{
    return unit != NULL && strcasecmp(unit, want) == 0;
}

/*
 * The readout's number format from the user's settings and the face's unit. Pressure: "Auto" is
 * none for psi and kPa, one for bar. Temperature: whole degrees, optionally to the nearest 10.
 * Any other unit keeps the face's own formats.
 */
void app_ui_priv_apply_gauge_prefs(void)
{
    if (s.gauge == NULL) {
        return;
    }
    const app_settings_t *set  = app_settings_get();
    const char           *unit = s.unit;

    int   decimals = -1;
    float step     = 0.0f;
    if (unit_is(unit, "psi") || unit_is(unit, "kpa") || unit_is(unit, "bar")) {
        if (set->pressure_decimals == APP_SETTINGS_DECIMALS_AUTO) {
            decimals = unit_is(unit, "bar") ? 1 : 0;
        } else {
            decimals = (int)set->pressure_decimals - (int)APP_SETTINGS_DECIMALS_0;
        }
    } else if (unit_is(unit, "c") || strcmp(unit, "\xc2\xb0" "C") == 0) {
        decimals = 0;
        step     = (set->temp_step == APP_SETTINGS_TEMP_STEP_10) ? 10.0f : 0.0f;
    }
    gauge_render_set_number_format(s.gauge, decimals, step);
    gauge_render_show_peak_value(s.gauge, set->show_peak);
    gauge_render_set_trend_window(s.gauge, app_settings_trend_seconds(set->trend_window));
}

/* ------------------------------------------------------------------ pixel shift ----- */

/*
 * A slow orbit, 3 px from centre, so no edge of the static artwork sits on the same pixels for
 * long. Each step redraws the whole dial once: one dropped frame every few minutes.
 */
static const int8_t k_shift_orbit[][2] = {
    {0, 0}, {3, 0}, {2, 2}, {0, 3}, {-2, 2}, {-3, 0}, {-2, -2}, {0, -3}, {2, -2},
};
#define SHIFT_STEPS (sizeof(k_shift_orbit) / sizeof(k_shift_orbit[0]))

static void shift_to(uint8_t step)
{
    s.shift_step = step % SHIFT_STEPS;
    lv_obj_set_style_translate_x(s.tile_gauge, k_shift_orbit[s.shift_step][0], LV_PART_MAIN);
    lv_obj_set_style_translate_y(s.tile_gauge, k_shift_orbit[s.shift_step][1], LV_PART_MAIN);
    lv_obj_update_layout(s.tile_gauge);
    gauge_render_parent_moved(s.gauge);
    lv_obj_invalidate(s.tile_gauge);
}

static void shift_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* Not while the settings tile is up: moving the tile under a finger would be odd. */
    if (app_ui_get_tile() == APP_UI_TILE_GAUGE) {
        shift_to((uint8_t)(s.shift_step + 1));
    }
}

void app_ui_priv_apply_pixel_shift(void)
{
    uint32_t period = app_settings_shift_period_ms(app_settings_get()->pixel_shift);
    if (s.shift_timer != NULL) {
        lv_timer_delete(s.shift_timer);
        s.shift_timer = NULL;
    }
    if (period == 0) {
        shift_to(0);
        return;
    }
    s.shift_timer = lv_timer_create(shift_timer_cb, period, NULL);
}

/* ------------------------------------------------------------------ engine off ------ */

#define OFF_BRIGHTNESS   3      /* percent: enough to read the notice in a dark garage */
#define OFF_MOVE_MS      20000  /* the notice moves this often */
#define OFF_PEEK_MS      60000  /* a tap shows the dial for this long */
#define OFF_NOTICE_COLOR 0x50575c

static void off_move_notice(void)
{
    if (s.off_notice == NULL || s.board == NULL) {
        return;
    }
    /* Anywhere inside the middle 60% of the panel, so it stays on the round glass. */
    int32_t span_x = s.board->width_px * 3 / 10;
    int32_t span_y = s.board->height_px * 3 / 10;
    int32_t dx     = (int32_t)(lv_rand(0, (uint32_t)(2 * span_x))) - span_x;
    int32_t dy     = (int32_t)(lv_rand(0, (uint32_t)(2 * span_y))) - span_y;
    lv_obj_align(s.off_notice, LV_ALIGN_CENTER, dx, dy);
}

static void off_timer_cb(lv_timer_t *t)
{
    (void)t;
    off_move_notice();
}

static void off_screen_hide(void)
{
    if (s.off_timer != NULL) {
        lv_timer_delete(s.off_timer);
        s.off_timer = NULL;
    }
    if (s.off_screen != NULL) {
        lv_obj_delete(s.off_screen);
        s.off_screen = NULL;
        s.off_notice = NULL;
        bsp_display_brightness_set(app_settings_get()->brightness);
    }
}

static void peek_timer_cb(lv_timer_t *t);

static void off_screen_tapped_cb(lv_event_t *e)
{
    (void)e;
    off_screen_hide();
    if (s.peek_timer == NULL) {
        s.peek_timer = lv_timer_create(peek_timer_cb, OFF_PEEK_MS, NULL);
        if (s.peek_timer != NULL) {
            lv_timer_set_repeat_count(s.peek_timer, 1);
        }
    }
}

static void off_screen_show(void)
{
    if (s.off_screen != NULL || s.peek_timer != NULL) {
        return;
    }
    /* An opaque cover: LVGL then repaints only it where the needle moves underneath. */
    s.off_screen = lv_obj_create(lv_layer_top());
    if (s.off_screen == NULL) {
        return;
    }
    lv_obj_remove_style_all(s.off_screen);
    lv_obj_set_size(s.off_screen, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.off_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.off_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(s.off_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s.off_screen, off_screen_tapped_cb, LV_EVENT_CLICKED, NULL);

    s.off_notice = lv_label_create(s.off_screen);
    if (s.off_notice != NULL) {
        lv_label_set_text(s.off_notice, "ENGINE OFF");
        lv_obj_set_style_text_font(s.off_notice, &lv_font_montserrat_22, LV_PART_MAIN);
        lv_obj_set_style_text_color(s.off_notice, lv_color_hex(OFF_NOTICE_COLOR), LV_PART_MAIN);
        off_move_notice();
    }
    s.off_timer = lv_timer_create(off_timer_cb, OFF_MOVE_MS, NULL);
    bsp_display_brightness_set(OFF_BRIGHTNESS);
}

static void peek_timer_cb(lv_timer_t *t)
{
    (void)t;
    s.peek_timer = NULL; /* one-shot: LVGL deletes it after this callback */
    if (!s.engine_running && app_settings_get()->engine_off_screen) {
        off_screen_show();
    }
}

void app_ui_set_engine_running(bool running)
{
    s.engine_running = running;
    if (running || !app_settings_get()->engine_off_screen) {
        if (s.peek_timer != NULL) {
            lv_timer_delete(s.peek_timer);
            s.peek_timer = NULL;
        }
        off_screen_hide();
    } else {
        off_screen_show();
    }
}

void app_ui_priv_apply_engine_off(void)
{
    app_ui_set_engine_running(s.engine_running);
}

/* ------------------------------------------------------------------ dial ------------ */

static void dial_pressed_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_active(), &s.press_at);
}

/*
 * Tapping the dial clears the peak marker. SHORT_CLICKED, not CLICKED: LVGL sends CLICKED on
 * release after a long press too, which would clear the peak on every min/max recall.
 *
 * The dial tile does not scroll, so LVGL also reports a swipe that goes nowhere (downwards, with
 * no tile above) as a click. Found by driving the UI over HTTP: a downward swipe cleared the peak.
 * So a tap only counts if the finger stayed put.
 */
static void dial_tapped_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t now;
    lv_indev_get_point(lv_indev_active(), &now);
    if (abs(now.x - s.press_at.x) > TAP_SLOP_PX || abs(now.y - s.press_at.y) > TAP_SLOP_PX) {
        return;
    }
    if (s.gauge != NULL) {
        gauge_render_reset_peak(s.gauge);
    }
}

/* Long-press recalls min/max; a second long-press while it is showing resets them. */
static void dial_long_pressed_cb(lv_event_t *e)
{
    (void)e;
    if (s.gauge == NULL) {
        return;
    }
    if (gauge_render_minmax_recalled(s.gauge)) {
        gauge_render_reset_minmax(s.gauge);
    }
    gauge_render_recall_minmax(s.gauge, MINMAX_RECALL_MS);
}

/* ------------------------------------------------------------------ timers ---------- */

/* Refreshes the optional badge on the dial and the live values on the settings tile. */
static void status_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* Only touch the badge when it is actually visible -- it sits on the gauge tile. */
    if (s.fps_badge != NULL && !lv_obj_has_flag(s.fps_badge, LV_OBJ_FLAG_HIDDEN)) {
        gauge_perf_stats_t st;
        gauge_perf_get(&st);
        lv_label_set_text_fmt(s.fps_badge, "%.0f fps", (double)st.fps);
    }

    settings_tick(app_ui_get_tile() == APP_UI_TILE_SETTINGS);
}

static void tile_changed_cb(lv_event_t *e)
{
    (void)e;
    settings_tile_changed(app_ui_get_tile() == APP_UI_TILE_SETTINGS);
}

/* --------------------------------------------------------------------- public ------- */

esp_err_t app_ui_create(const gauge_config_t *cfg, const board_profile_t *board)
{
    ESP_RETURN_ON_FALSE(cfg && board, ESP_ERR_INVALID_ARG, TAG, "bad args");

    s.board          = board;
    s.engine_running = true;
    strlcpy(s.unit, cfg->source.unit, sizeof(s.unit));

    app_ui_priv_apply_rotation(app_settings_get()->rotation);
    app_ui_priv_apply_fps_cap(app_settings_get()->fps_cap);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s.tileview = lv_tileview_create(scr);
    ESP_RETURN_ON_FALSE(s.tileview != NULL, ESP_ERR_NO_MEM, TAG, "tileview failed");
    lv_obj_set_size(s.tileview, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.tileview, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s.tileview, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s.tileview, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s.tileview, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

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
    lv_obj_remove_flag(s.tile_settings, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_remove_flag(s.tile_gauge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s.tile_gauge, dial_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s.tile_gauge, dial_tapped_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s.tile_gauge, dial_long_pressed_cb, LV_EVENT_LONG_PRESSED, NULL);
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
    app_ui_priv_show_fps_badge(app_settings_get()->show_fps);

    app_ui_priv_apply_gauge_prefs();
    app_ui_priv_apply_pixel_shift();

    settings_build(s.tile_settings, board, cfg);

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

    gauge_render_t *old_gauge   = s.gauge;
    gauge_render_t *replacement = NULL;

    /*
     * Build the replacement before destroying the old one. If it fails -- most likely on the
     * PSRAM allocation for the face -- the gauge on screen keeps working rather than the
     * driver being left with nothing.
     */
    esp_err_t err = gauge_render_create(s.tile_gauge, cfg, s.board, &replacement);
    ESP_RETURN_ON_ERROR(err, TAG, "could not build the replacement gauge");

    s.gauge = replacement;
    strlcpy(s.unit, cfg->source.unit, sizeof(s.unit));
    gauge_render_set_alert_cb(s.gauge, s.alert_cb, s.alert_cb_user);
    app_ui_priv_apply_gauge_prefs();
    gauge_render_parent_moved(s.gauge); /* built where the tile is now, but say so anyway */

    if (old_gauge != NULL) {
        gauge_render_destroy(old_gauge);
    }

    /* The new gauge objects were created after the badge, so lift it back on top. */
    if (s.fps_badge != NULL) {
        lv_obj_move_foreground(s.fps_badge);
    }

    settings_set_gauge_info(cfg);

    ESP_LOGI(TAG, "switched to gauge '%s'", cfg->id);
    return ESP_OK;
}

void app_ui_set_gauge_list(const char (*ids)[GAUGE_CONFIG_MAX_ID_LEN], int count,
                           const char *active)
{
    settings_set_gauge_list(ids, count, active);
}

void app_ui_set_gauge_selected_cb(app_ui_gauge_selected_cb_t cb)
{
    settings_set_gauge_selected_cb(cb);
}

void app_ui_set_network_reset_cb(app_ui_network_reset_cb_t cb)
{
    settings_set_network_reset_cb(cb);
}

void app_ui_set_firmware_tap_cb(app_ui_firmware_tap_cb_t cb)
{
    settings_set_firmware_tap_cb(cb);
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
    settings_set_network_status(state, detail);
}

void app_ui_set_update_status(const char *text)
{
    settings_set_update_status(text);
}

void app_ui_set_warning(const char *text)
{
    settings_set_warning(text);
}
