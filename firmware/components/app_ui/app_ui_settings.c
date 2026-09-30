/*
 * The settings tile. Design and rationale: docs/settings-ui.md.
 *
 * Built for a 466 px round panel on the base of an A-pillar, read at arm's length and mostly used
 * parked:
 *   - A home screen: brightness on an arc around the top rim, six large round buttons, and a pill
 *     back to the dial at the bottom, where the circle is still wide.
 *   - One page per topic. A page is built when it opens and deleted when it closes, so only one
 *     page's widgets ever exist. LVGL widgets are small mallocs, which land in internal RAM, the
 *     board's binding constraint (ADR 0009).
 *   - Night-safe: true black, dark controls, one accent. Nothing large and white.
 *   - After SETTINGS_IDLE_MS untouched, back to the dial, so a glance always finds the gauge.
 */

#include "app_ui_priv.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "bsp/display.h"

#include "gauge_perf.h"
#include "gauge_store.h"
#include "sensor_hub.h"

static const char *TAG = "app_ui_settings";

/* ------------------------------------------------------------------ look ----------- */

#define COLOR_BG        0x000000 /* AMOLED pixels off: no glow at night */
#define COLOR_SURFACE   0x1c1c22
#define COLOR_PRESSED   0x33333c
#define COLOR_TEXT      0xf0f0f0
#define COLOR_MUTED     0x8c8c96
#define COLOR_ACCENT    0x22b8ff /* the needle's cyan */
#define COLOR_ON_ACCENT 0x000000
#define COLOR_WARN      0xffab00
#define COLOR_DANGER    0xc62828
#define COLOR_GOOD      0x00c853

#define FONT_TITLE   (&lv_font_montserrat_26)
#define FONT_BODY    (&lv_font_montserrat_24)
#define FONT_CAPTION (&lv_font_montserrat_20)
#define FONT_SMALL   (&lv_font_montserrat_18)
#define FONT_ICON    (&lv_font_montserrat_32)
#define FONT_VALUE   (&lv_font_montserrat_48)

#define DEG "\xC2\xB0"

/* Geometry, for a 466 px panel; scaled from the board's width so another round panel follows. */
#define MENU_BTN      84  /* round menu buttons */
#define CONTROL_H     64  /* the shortest touch target */
#define PILL_W        184
#define PILL_H        60
#define PAGE_SIDE_PAD 56
#define PAGE_TOP_PAD  48
#define PAGE_BOTTOM   150 /* room to scroll the last row above the floating Back pill */

#define SETTINGS_IDLE_MS     30000
#define CONFIRM_MS           4000
#define SENSOR_TEXT_LEN      640
#define NETWORK_TEXT_LEN     160
#define UPDATE_TEXT_LEN      160
#define WARNING_TEXT_LEN     160

/* ------------------------------------------------------------------ calibration ---- */

/*
 * Edited with one picker and one -/+ pair rather than a row each: a row per parameter cost 24KB of
 * internal RAM when it was tried (ADR 0009).
 */
typedef enum { CAL_FLOAT, CAL_BOOL } cal_kind_t;

typedef struct {
    const char *name;
    cal_kind_t  kind;
    const char *fmt;
    size_t      offset;
    float       step;
    float       scale;
} cal_param_t;

#define CAL_F(name, fmt, field, step, scale) \
    {name, CAL_FLOAT, fmt, offsetof(sensor_hub_cal_t, field), step, scale}
#define CAL_B(name, field) {name, CAL_BOOL, NULL, offsetof(sensor_hub_cal_t, field), 0.0f, 0.0f}

static const cal_param_t k_cal_params[] = {
    CAL_B("Auto-zero at start", auto_zero),
    CAL_F("Stored zero",        "%.1f kPa",       baro_kpa,     0.1f,   1.0f),
    CAL_F("Sensor range",       "%.1f bar",       map_p_hi_kpa, 10.0f,  0.01f),
    CAL_F("Output at 0 bar",    "%.2f V",         map_v_lo,     0.01f,  1.0f),
    CAL_F("Output at max",      "%.2f V",         map_v_hi,     0.01f,  1.0f),
    CAL_B("Ratiometric",        ratiometric),
    CAL_F("Boost smoothing",    "%.0f ms",        boost_tau_ms, 10.0f,  1.0f),
    CAL_F("EGT offset",         "%+.1f " DEG "C", egt_offset_c, 0.5f,   1.0f),
    CAL_F("EGT smoothing",      "%.0f ms",        egt_tau_ms,   50.0f,  1.0f),
    CAL_F("MAP divider",        "%.3f",           map_div,      0.002f, 1.0f),
    CAL_F("Supply divider",     "%.3f",           supply_div,   0.002f, 1.0f),
    CAL_F("Ignition divider",   "%.3f",           ignition_div, 0.01f,  1.0f),
};
#define CAL_PARAM_COUNT (sizeof(k_cal_params) / sizeof(k_cal_params[0]))

/* ------------------------------------------------------------------ state ---------- */

typedef void (*page_fn_t)(lv_obj_t *page);

static struct {
    lv_style_t btn, btn_pressed, round, caption, body, seg_main, seg_item, seg_checked, seg_pressed;
    bool       styles_ready;
} st;

/*
 * In PSRAM (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY): only the LVGL task touches it, never
 * from an ISR or with the cache off, and the gauge ids alone are 384 bytes.
 */
EXT_RAM_BSS_ATTR static struct {
    const board_profile_t *board;
    lv_obj_t *tile;
    lv_obj_t *home;
    lv_obj_t *bright_label;
    lv_obj_t *system_icon;

    /* The open page, and the widgets on it that change. NULL when absent. */
    lv_obj_t *page;
    page_fn_t page_tick;
    lv_obj_t *sensor_label;
    lv_obj_t *sensor_msg;
    lv_obj_t *cal_name;
    lv_obj_t *cal_value;
    lv_obj_t *net_label;
    lv_obj_t *net_reset_btn;
    lv_obj_t *net_reset_label;
    lv_obj_t *fw_label;
    lv_obj_t *perf_label;
    lv_obj_t *heap_label;
    lv_obj_t *gauge_info;

    lv_timer_t *net_reset_timer;
    bool        net_reset_armed;
    bool        sensor_raw;
    size_t      cal_index;

    char  gauge_ids[GAUGE_STORE_MAX_GAUGES][GAUGE_CONFIG_MAX_ID_LEN];
    const char *gauge_map[2 * GAUGE_STORE_MAX_GAUGES + 1];
    int   gauge_count;
    int   gauge_active;
    char  gauge_info_text[96];

    /* Text for labels that may not exist yet, handed to LVGL as static text. */
    char sensor_text[SENSOR_TEXT_LEN];
    char network_text[NETWORK_TEXT_LEN];
    char update_text[UPDATE_TEXT_LEN];
    char warning_text[WARNING_TEXT_LEN];

    app_ui_gauge_selected_cb_t on_gauge_selected;
    app_ui_network_reset_cb_t  on_network_reset;
    app_ui_firmware_tap_cb_t   on_firmware_tap;
} g;

static void cal_refresh(void);

static const char *const k_rotation_map[] ={"0" DEG, "90" DEG, "180" DEG, "270" DEG, ""};
static const char *const k_fps_map[]      = {"30", "45", "60", "Off", ""};
static const char *const k_badge_map[]    = {"Hidden", "Shown", ""};

/* ------------------------------------------------------------------ helpers -------- */

static void styles_init(void)
{
    if (st.styles_ready) {
        return;
    }
    st.styles_ready = true;

    lv_style_init(&st.btn);
    lv_style_set_bg_color(&st.btn, lv_color_hex(COLOR_SURFACE));
    lv_style_set_bg_opa(&st.btn, LV_OPA_COVER);
    lv_style_set_radius(&st.btn, 20);
    lv_style_set_text_color(&st.btn, lv_color_hex(COLOR_TEXT));
    lv_style_set_text_font(&st.btn, FONT_BODY);

    lv_style_init(&st.btn_pressed);
    lv_style_set_bg_color(&st.btn_pressed, lv_color_hex(COLOR_PRESSED));

    lv_style_init(&st.round);
    lv_style_set_radius(&st.round, LV_RADIUS_CIRCLE);

    lv_style_init(&st.caption);
    lv_style_set_text_font(&st.caption, FONT_CAPTION);
    lv_style_set_text_color(&st.caption, lv_color_hex(COLOR_MUTED));

    lv_style_init(&st.body);
    lv_style_set_text_font(&st.body, FONT_BODY);
    lv_style_set_text_color(&st.body, lv_color_hex(COLOR_TEXT));

    lv_style_init(&st.seg_main);
    lv_style_set_pad_all(&st.seg_main, 0);
    lv_style_set_pad_gap(&st.seg_main, 8);
    lv_style_set_bg_opa(&st.seg_main, LV_OPA_TRANSP);
    lv_style_set_border_width(&st.seg_main, 0);

    lv_style_init(&st.seg_item);
    lv_style_set_bg_color(&st.seg_item, lv_color_hex(COLOR_SURFACE));
    lv_style_set_bg_opa(&st.seg_item, LV_OPA_COVER);
    lv_style_set_radius(&st.seg_item, 18);
    lv_style_set_text_font(&st.seg_item, FONT_BODY);
    lv_style_set_text_color(&st.seg_item, lv_color_hex(COLOR_TEXT));

    lv_style_init(&st.seg_checked);
    lv_style_set_bg_color(&st.seg_checked, lv_color_hex(COLOR_ACCENT));
    lv_style_set_text_color(&st.seg_checked, lv_color_hex(COLOR_ON_ACCENT));

    lv_style_init(&st.seg_pressed);
    lv_style_set_bg_color(&st.seg_pressed, lv_color_hex(COLOR_PRESSED));
}

/* A container with no theme styling: layout only. */
static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_style_t *style, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, style, 0);
    lv_label_set_text(l, text);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, int32_t w, int32_t h, bool round)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &st.btn, 0);
    lv_obj_add_style(b, &st.btn_pressed, LV_STATE_PRESSED);
    if (round) {
        lv_obj_add_style(b, &st.round, 0);
    }
    lv_obj_set_size(b, w, h);
    /*
     * Pages keep a tall bottom padding so their last row can scroll clear of the Back pill. With
     * scroll-on-focus, a tap on a button in that band scrolled the page and cut off its title.
     */
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return b;
}

/* A button with centred text, e.g. "Zero boost now". Returns the label. */
static lv_obj_t *text_button(lv_obj_t *parent, int32_t w, int32_t h, const char *text,
                             lv_event_cb_t cb, lv_obj_t **btn_out)
{
    lv_obj_t *b = button(parent, w, h, false);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(b, &st.body, text);
    lv_obj_center(l);
    if (btn_out != NULL) {
        *btn_out = b;
    }
    return l;
}

/* A segmented control: one buttonmatrix, so four choices cost one widget. */
static lv_obj_t *segmented(lv_obj_t *parent, const char *const *map, uint32_t checked,
                           lv_event_cb_t cb)
{
    lv_obj_t *bm = lv_buttonmatrix_create(parent);
    lv_obj_remove_style_all(bm);
    lv_obj_add_style(bm, &st.seg_main, 0);
    lv_obj_add_style(bm, &st.seg_item, LV_PART_ITEMS);
    lv_obj_add_style(bm, &st.seg_pressed, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(bm, &st.seg_checked, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_buttonmatrix_set_map(bm, map);
    /* CLICK_TRIG: report on release, after the button has been checked. */
    lv_buttonmatrix_set_button_ctrl_all(
        bm, (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CHECKABLE | LV_BUTTONMATRIX_CTRL_CLICK_TRIG));
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_obj_remove_flag(bm, LV_OBJ_FLAG_SCROLL_ON_FOCUS); /* see button() */
    lv_buttonmatrix_set_button_ctrl(bm, checked, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(bm, LV_PCT(100), CONTROL_H);
    lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return bm;
}

/* A caption above a control, left-aligned across the page's width. */
static void caption(lv_obj_t *page, const char *text)
{
    lv_obj_t *l = label(page, &st.caption, text);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_margin_top(l, 8, 0);
}

static lv_obj_t *wrap_label(lv_obj_t *parent, const lv_style_t *style, const char *text)
{
    lv_obj_t *l = label(parent, style, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    return l;
}

static int append(char *buf, size_t len, int used, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int append(char *buf, size_t len, int used, const char *fmt, ...)
{
    if (used < 0 || (size_t)used >= len) {
        return used;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + used, len - (size_t)used, fmt, ap);
    va_end(ap);
    return (n < 0) ? used : used + n;
}

/* ------------------------------------------------------------------ pages ---------- */

static void close_page(void)
{
    if (g.page == NULL) {
        return;
    }
    if (g.net_reset_timer != NULL) {
        lv_timer_delete(g.net_reset_timer);
        g.net_reset_timer = NULL;
    }
    /* Async: this usually runs inside the Back button's own click event. */
    lv_obj_delete_async(g.page);
    g.page            = NULL;
    g.page_tick       = NULL;
    g.sensor_label    = NULL;
    g.sensor_msg      = NULL;
    g.cal_name        = NULL;
    g.cal_value       = NULL;
    g.net_label       = NULL;
    g.net_reset_btn   = NULL;
    g.net_reset_label = NULL;
    g.fw_label        = NULL;
    g.perf_label      = NULL;
    g.heap_label      = NULL;
    g.gauge_info      = NULL;
    g.net_reset_armed = false;
    lv_obj_remove_flag(g.home, LV_OBJ_FLAG_HIDDEN);
}

static void back_clicked_cb(lv_event_t *e)
{
    (void)e;
    close_page();
}

static void open_page(const char *title, page_fn_t build, page_fn_t tick)
{
    close_page();

    lv_obj_t *p = lv_obj_create(g.tile);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(p, PAGE_TOP_PAD, 0);
    lv_obj_set_style_pad_bottom(p, PAGE_BOTTOM, 0);
    lv_obj_set_style_pad_hor(p, PAGE_SIDE_PAD, 0);
    lv_obj_set_style_pad_row(p, 12, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    g.page = p;

    lv_obj_t *t = label(p, &st.body, title);
    lv_obj_set_style_text_font(t, FONT_TITLE, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_margin_bottom(t, 6, 0);

    build(p);
    g.page_tick = tick;

    /* Floating: stays put at the bottom centre, the widest part of the circle left, as the page scrolls. */
    lv_obj_t *back = button(p, PILL_W, PILL_H, true);
    lv_obj_add_flag(back, LV_OBJ_FLAG_FLOATING);
    /* Aligned within the padded content box, so undo the bottom padding: 30 px off the rim. */
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, PAGE_BOTTOM - 30);
    lv_obj_set_style_border_width(back, 2, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(COLOR_PRESSED), 0);
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = label(back, &st.body, LV_SYMBOL_LEFT "  Back");
    lv_obj_center(bl);

    lv_obj_add_flag(g.home, LV_OBJ_FLAG_HIDDEN);
    if (tick != NULL) {
        tick(p);
    }
}

/* ---- Display ---- */

static void apply_rotation_async_cb(void *arg)
{
    app_ui_priv_apply_rotation((app_settings_rotation_t)(uintptr_t)arg);
}

static void rotation_changed_cb(lv_event_t *e)
{
    uint32_t idx = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (idx >= APP_SETTINGS_ROTATION_COUNT || idx == (uint32_t)app_settings_get()->rotation) {
        return;
    }
    app_settings_set_rotation((app_settings_rotation_t)idx);
    app_settings_commit();
    /*
     * Rotating remaps touch coordinates, so LVGL sees the finger jump. Done inside this tap, the
     * buttonmatrix checked the wrong button, so wait until the tap has been handled.
     */
    lv_async_call(apply_rotation_async_cb, (void *)(uintptr_t)idx);
}

static void fps_cap_changed_cb(lv_event_t *e)
{
    uint32_t idx = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (idx >= APP_SETTINGS_FPS_CAP_COUNT) {
        return;
    }
    app_settings_set_fps_cap((app_settings_fps_cap_t)idx);
    app_settings_commit();
    app_ui_priv_apply_fps_cap((app_settings_fps_cap_t)idx);
}

static void badge_changed_cb(lv_event_t *e)
{
    uint32_t idx = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (idx > 1) {
        return;
    }
    app_settings_set_show_fps(idx == 1);
    app_settings_commit();
    app_ui_priv_show_fps_badge(idx == 1);
}

static void build_display(lv_obj_t *p)
{
    const app_settings_t *set = app_settings_get();
    caption(p, "Rotation");
    segmented(p, k_rotation_map, (uint32_t)set->rotation, rotation_changed_cb);
    caption(p, "Frame rate limit");
    segmented(p, k_fps_map, (uint32_t)set->fps_cap, fps_cap_changed_cb);
}

/* ---- Gauge ---- */

static void gauge_list_cb(lv_event_t *e)
{
    uint32_t idx = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if ((int)idx >= g.gauge_count || g.on_gauge_selected == NULL) {
        return;
    }
    g.gauge_active = (int)idx;
    /* The owner loads it; app_ui knows widgets, not the filesystem. */
    g.on_gauge_selected(g.gauge_ids[idx]);
}

static void build_gauge(lv_obj_t *p)
{
    g.gauge_info = wrap_label(p, &st.caption, g.gauge_info_text);
    lv_obj_set_style_text_align(g.gauge_info, LV_TEXT_ALIGN_CENTER, 0);

    if (g.gauge_count == 0) {
        wrap_label(p, &st.body, "Only the built-in face is available. Add faces with the editor at "
                                "http://<gauge>/editor/");
        return;
    }

    /* One buttonmatrix, a face per row: twelve faces cost one widget, not twenty-four. */
    lv_obj_t *bm = segmented(p, g.gauge_map, (uint32_t)g.gauge_active, gauge_list_cb);
    lv_obj_set_height(bm, g.gauge_count * CONTROL_H + (g.gauge_count - 1) * 8);
}

/* ---- Sensors ---- */

/*
 * Raw conversions for validating a board on the bench: the ADC code, the voltage at the pin (before
 * any divider), and how many reads per second each input is getting. Tapping the readout switches
 * here and rescans the bus, so a part that does not answer shows where, if anywhere, it is.
 */
static void format_sensor_raw(char *buf, size_t len, const sensor_hub_snapshot_t *snap)
{
    static const char *const names[SENSOR_HUB_RAW_COUNT] = {
        [SENSOR_HUB_RAW_AIN0] = "AIN0 MAP",
        [SENSOR_HUB_RAW_AIN1] = "AIN1 5V",
        [SENSOR_HUB_RAW_AIN2] = "AIN2 IGN",
        [SENSOR_HUB_RAW_AIN3] = "AIN3 TC",
        [SENSOR_HUB_RAW_TMP]  = "TMP1075",
    };

    int n = append(buf, len, 0, "Raw  (tap for summary)");
    for (int i = 0; i < SENSOR_HUB_RAW_COUNT; i++) {
        const sensor_hub_raw_t *r = &snap->raw[i];
        bool present = (i == SENSOR_HUB_RAW_TMP) ? snap->tmp1075_ok : snap->ads1115_ok;

        n = append(buf, len, n, "\n%s  %.1f/s", names[i], (double)r->rate_hz);
        if (r->count == 0) {
            n = append(buf, len, n, "\n  %s", present ? "waiting" : "no reads");
        } else if (i == SENSOR_HUB_RAW_TMP) {
            n = append(buf, len, n, "\n  0x%04x  %.2f " DEG "C", (unsigned)(uint16_t)r->code,
                       (double)r->value);
        } else if (i == SENSOR_HUB_RAW_AIN3) {
            n = append(buf, len, n, "\n  %d  %.3f mV", r->code, (double)(r->value * 1000.0f));
        } else {
            n = append(buf, len, n, "\n  %d  %.4f V", r->code, (double)r->value);
        }
        if (r->count != 0 && !present) {
            n = append(buf, len, n, " (stale)");
        }
    }

    n = append(buf, len, n, "\nBus:");
    if (!snap->scanned) {
        n = append(buf, len, n, " scanning");
    } else {
        int found = 0;
        for (unsigned a = 0; a < 128; a++) {
            if (sensor_hub_scan_found(snap, (uint8_t)a)) {
                n = append(buf, len, n, " %02x", a);
                found++;
            }
        }
        if (found == 0) {
            n = append(buf, len, n, " nothing answers");
        }
    }
    append(buf, len, n, "\nALERT %s, I2C errors %" PRIu32,
           !snap->ads1115_ok ? "--" : snap->alert_ok ? "ok" : "silent", snap->i2c_errors);
}

/* Live readings in native units, so they compare directly with a meter. */
static void format_sensor_summary(char *buf, size_t len, const sensor_hub_snapshot_t *snap)
{
    const sensor_hub_reading_t *ch = snap->ch;
    int n = append(buf, len, 0, "Board %s", sensor_hub_state_str(snap->state));

    if (snap->state != SENSOR_HUB_ONLINE) {
#if CONFIG_AI_GAUGE_SIMULATED_SOURCE
        if (snap->state == SENSOR_HUB_SEARCHING) {
            n = append(buf, len, n, "\nNeedle is simulated");
        }
#endif
        append(buf, len, n, "\nTap for raw readings");
        return;
    }
    if (!snap->ads1115_ok) {
        n = append(buf, len, n, "\nADC missing");
    }
    if (!snap->tmp1075_ok) {
        n = append(buf, len, n, "\nCJ sensor missing");
    }
    if (ch[SENSOR_HUB_CH_MAP].valid) {
        n = append(buf, len, n, "\nMAP %.1f kPa (%.3f V)", (double)ch[SENSOR_HUB_CH_MAP].value,
                   (double)snap->map_sensor_v);
        n = append(buf, len, n, "\nBoost %.1f kPa", (double)ch[SENSOR_HUB_CH_BOOST].value);
    } else if (snap->ads1115_ok) {
        n = append(buf, len, n, "\nBoost: %s", snap->boost_fault ? snap->boost_fault : "--");
    }
    if (ch[SENSOR_HUB_CH_EGT].valid) {
        n = append(buf, len, n, "\nEGT %.0f " DEG "C (%.0f uV)", (double)ch[SENSOR_HUB_CH_EGT].value,
                   (double)snap->tc_uv);
    } else if (snap->ads1115_ok || snap->tmp1075_ok) {
        n = append(buf, len, n, "\nEGT: %s", snap->egt_fault ? snap->egt_fault : "--");
    }
    if (ch[SENSOR_HUB_CH_COLD_JUNCTION].valid) {
        n = append(buf, len, n, "\nCold junction %.1f " DEG "C",
                   (double)ch[SENSOR_HUB_CH_COLD_JUNCTION].value);
    }
    if (snap->ads1115_ok) {
        n = append(buf, len, n, "\nSupply %.2f V", (double)ch[SENSOR_HUB_CH_SENSOR_SUPPLY].value);
        n = append(buf, len, n, "\nIgnition %.1f V", (double)ch[SENSOR_HUB_CH_IGNITION].value);
    }
    n = append(buf, len, n, "\nZero %.1f kPa%s", (double)snap->baro_kpa,
               snap->auto_zeroed ? " (auto)" : "");
    if (snap->ads1115_ok && !snap->alert_ok) {
        n = append(buf, len, n, "\nALERT line silent");
    }
    if (snap->i2c_errors > 0) {
        n = append(buf, len, n, "\nI2C errors %" PRIu32, snap->i2c_errors);
    }
    append(buf, len, n, "\nTap for raw readings");
}

static void sensors_tick(lv_obj_t *p)
{
    (void)p;
    if (g.sensor_label == NULL) {
        return;
    }
    sensor_hub_snapshot_t snap;
    sensor_hub_get_snapshot(&snap);
    if (g.sensor_raw) {
        format_sensor_raw(g.sensor_text, sizeof(g.sensor_text), &snap);
    } else {
        format_sensor_summary(g.sensor_text, sizeof(g.sensor_text), &snap);
    }
    lv_label_set_text_static(g.sensor_label, g.sensor_text);
}

static void sensor_label_clicked_cb(lv_event_t *e)
{
    (void)e;
    g.sensor_raw = !g.sensor_raw;
    if (g.sensor_raw) {
        sensor_hub_request_scan();
    }
    sensors_tick(NULL);
}

static void zero_boost_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (g.sensor_msg == NULL) {
        return;
    }
    char      msg[80];
    esp_err_t err = sensor_hub_zero_boost(msg, sizeof(msg));
    lv_label_set_text(g.sensor_msg, msg);
    lv_obj_set_style_text_color(g.sensor_msg, lv_color_hex(err == ESP_OK ? COLOR_GOOD : COLOR_WARN), 0);
    lv_obj_remove_flag(g.sensor_msg, LV_OBJ_FLAG_HIDDEN);
    cal_refresh(); /* the stored zero may be the parameter on show */
}

static void build_sensors(lv_obj_t *p)
{
    g.sensor_raw   = false;
    g.sensor_label = wrap_label(p, &st.body, "");
    lv_obj_set_style_text_font(g.sensor_label, FONT_CAPTION, 0);
    lv_obj_set_style_text_line_space(g.sensor_label, 4, 0);
    /* Tapping the readout flips to the raw view: the whole block is the target. */
    lv_obj_add_flag(g.sensor_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.sensor_label, sensor_label_clicked_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- Calibrate ---- */

static void cal_refresh(void)
{
    if (g.cal_value == NULL) {
        return;
    }
    const cal_param_t *prm = &k_cal_params[g.cal_index];
    lv_label_set_text(g.cal_name, prm->name);

    sensor_hub_cal_t cal;
    sensor_hub_get_cal(&cal);
    void *field = (char *)&cal + prm->offset;
    if (prm->kind == CAL_BOOL) {
        lv_label_set_text(g.cal_value, *(bool *)field ? "On" : "Off");
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), prm->fmt, (double)(*(float *)field * prm->scale));
        lv_label_set_text(g.cal_value, buf);
    }
}

static void cal_pick_cb(lv_event_t *e)
{
    int dir     = (int)(intptr_t)lv_event_get_user_data(e);
    g.cal_index = (dir > 0) ? (g.cal_index + 1) % CAL_PARAM_COUNT
                            : (g.cal_index + CAL_PARAM_COUNT - 1) % CAL_PARAM_COUNT;
    cal_refresh();
}

/* User data is +1 or -1. A tap steps once, holding repeats, and the value is saved on release. */
static void cal_step_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        sensor_hub_save_cal();
        return;
    }

    const cal_param_t *prm = &k_cal_params[g.cal_index];
    sensor_hub_cal_t   cal;
    sensor_hub_get_cal(&cal);
    void *field = (char *)&cal + prm->offset;
    if (prm->kind == CAL_BOOL) {
        *(bool *)field = (dir > 0);
    } else {
        /* Snap to the step grid, so repeated taps land on round numbers rather than drifting. */
        float *f = field;
        *f       = roundf(*f / prm->step + (float)dir) * prm->step;
    }
    sensor_hub_set_cal(&cal);
    cal_refresh(); /* shows the value after clamping */
}

static lv_obj_t *icon_button(lv_obj_t *parent, int32_t size, const char *icon)
{
    lv_obj_t *b = button(parent, size, size, true);
    lv_obj_t *l = label(b, &st.body, icon);
    lv_obj_set_style_text_font(l, FONT_ICON, 0);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = box(parent);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static void build_calibrate(lv_obj_t *p)
{
    lv_obj_t *pick = row(p);
    lv_obj_t *prev = icon_button(pick, CONTROL_H, LV_SYMBOL_LEFT);
    lv_obj_add_event_cb(prev, cal_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    g.cal_name = label(pick, &st.body, "");
    lv_label_set_long_mode(g.cal_name, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(g.cal_name, 1);
    lv_obj_set_style_text_align(g.cal_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *next = icon_button(pick, CONTROL_H, LV_SYMBOL_RIGHT);
    lv_obj_add_event_cb(next, cal_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);

    /* - value +, on one row across the widest part of the circle. Hold to repeat; saved on release. */
    lv_obj_t *steps = row(p);
    for (int dir = -1; dir <= 1; dir += 2) {
        lv_obj_t *b = icon_button(steps, 88, dir < 0 ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS);
        lv_obj_add_event_cb(b, cal_step_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)dir);
        lv_obj_add_event_cb(b, cal_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)dir);
        lv_obj_add_event_cb(b, cal_step_cb, LV_EVENT_RELEASED, (void *)(intptr_t)dir);
        if (dir < 0) {
            g.cal_value = label(steps, &st.body, "");
            lv_obj_set_style_text_font(g.cal_value, FONT_ICON, 0);
            lv_obj_set_flex_grow(g.cal_value, 1);
            lv_obj_set_style_text_align(g.cal_value, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    /* Zeroing is calibration too, and needs the engine off: the result says if it refused. */
    text_button(p, LV_PCT(100), CONTROL_H, "Zero boost now", zero_boost_clicked_cb, NULL);
    g.sensor_msg = wrap_label(p, &st.body, "");
    lv_obj_set_style_text_font(g.sensor_msg, FONT_CAPTION, 0);
    lv_obj_set_style_text_align(g.sensor_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(g.sensor_msg, LV_OBJ_FLAG_HIDDEN);

    cal_refresh();
}

/* ---- Network ---- */

static void set_net_reset_armed(bool armed)
{
    g.net_reset_armed = armed;
    if (g.net_reset_btn == NULL) {
        return;
    }
    lv_label_set_text(g.net_reset_label, armed ? "Tap again to reset" : "Reset network");
    lv_obj_set_style_bg_color(g.net_reset_btn, lv_color_hex(armed ? COLOR_DANGER : COLOR_SURFACE), 0);
}

static void net_reset_disarm_cb(lv_timer_t *t)
{
    (void)t;
    g.net_reset_timer = NULL; /* one-shot: LVGL deletes it after this returns */
    set_net_reset_armed(false);
}

/* Two taps: dropping the connection from a stray touch in a moving vehicle would be a nuisance. */
static void net_reset_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!g.net_reset_armed) {
        set_net_reset_armed(true);
        g.net_reset_timer = lv_timer_create(net_reset_disarm_cb, CONFIRM_MS, NULL);
        lv_timer_set_repeat_count(g.net_reset_timer, 1);
        return;
    }
    if (g.net_reset_timer != NULL) {
        lv_timer_delete(g.net_reset_timer);
        g.net_reset_timer = NULL;
    }
    set_net_reset_armed(false);
    if (g.on_network_reset != NULL) {
        g.on_network_reset();
    }
}

static void build_network(lv_obj_t *p)
{
    g.net_label = wrap_label(p, &st.body, "");
    lv_obj_set_style_text_align(g.net_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(g.net_label, g.network_text);

    lv_obj_t *hint = wrap_label(p, &st.caption,
                                "Faces and settings: open the address above in a browser.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    g.net_reset_label = text_button(p, LV_PCT(100), CONTROL_H, "Reset network",
                                    net_reset_clicked_cb, &g.net_reset_btn);
    lv_obj_set_style_margin_top(g.net_reset_btn, 10, 0);
}

/* ---- System ---- */

static void firmware_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (g.on_firmware_tap != NULL) {
        g.on_firmware_tap();
    }
}

static void system_tick(lv_obj_t *p)
{
    (void)p;
    if (g.perf_label != NULL) {
        gauge_perf_stats_t pst;
        gauge_perf_get(&pst);
        lv_label_set_text_fmt(g.perf_label, "%.1f fps, %.2f ms render", (double)pst.fps,
                              (double)pst.render_ms_mean);
    }
    if (g.heap_label != NULL) {
        lv_label_set_text_fmt(g.heap_label, "%u KB internal, %u KB PSRAM",
                              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                              (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

static void build_system(lv_obj_t *p)
{
    if (g.warning_text[0] != '\0') {
        lv_obj_t *w = wrap_label(p, &st.body, "");
        lv_label_set_text_static(w, g.warning_text);
        lv_obj_set_style_text_color(w, lv_color_hex(COLOR_WARN), 0);
        lv_obj_set_style_text_font(w, FONT_CAPTION, 0);
    }

    caption(p, "Firmware");
    /* The whole card is the update control; app_main decides what a tap means. */
    lv_obj_t *fw = button(p, LV_PCT(100), LV_SIZE_CONTENT, false);
    lv_obj_set_style_pad_all(fw, 16, 0);
    lv_obj_add_event_cb(fw, firmware_clicked_cb, LV_EVENT_CLICKED, NULL);
    g.fw_label = wrap_label(fw, &st.body, "");
    lv_obj_set_style_text_align(g.fw_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(g.fw_label, g.update_text);

    /* Diagnostics: the FPS counter is only useful next to the numbers it explains. */
    caption(p, "FPS counter on the dial");
    segmented(p, k_badge_map, app_settings_get()->show_fps ? 1 : 0, badge_changed_cb);

    /* This page only redraws when something changes, so a low figure here is not a fault. */
    caption(p, "Frame rate, this page");
    g.perf_label = wrap_label(p, &st.body, "measuring...");
    lv_obj_set_style_text_color(g.perf_label, lv_color_hex(COLOR_GOOD), 0);
    lv_obj_set_style_text_font(g.perf_label, FONT_CAPTION, 0);

    caption(p, "Free memory");
    g.heap_label = wrap_label(p, &st.body, "-");
    lv_obj_set_style_text_font(g.heap_label, FONT_CAPTION, 0);
}

/* ------------------------------------------------------------------ home ----------- */

typedef struct {
    const char *icon;
    const char *name;
    page_fn_t   build;
    page_fn_t   tick;
} menu_item_t;

static const menu_item_t k_menu[] = {
    {LV_SYMBOL_IMAGE,    "Display",   build_display,   NULL},
    {LV_SYMBOL_LIST,     "Faces",     build_gauge,     NULL},
    {LV_SYMBOL_CHARGE,   "Sensors",   build_sensors,   sensors_tick},
    {LV_SYMBOL_EDIT,     "Calibrate", build_calibrate, NULL},
    {LV_SYMBOL_WIFI,     "Network",   build_network,   NULL},
    {LV_SYMBOL_SETTINGS, "System",    build_system,    system_tick},
};
#define MENU_COUNT (sizeof(k_menu) / sizeof(k_menu[0]))

static void menu_clicked_cb(lv_event_t *e)
{
    const menu_item_t *m = lv_event_get_user_data(e);
    open_page(m->name, m->build, m->tick);
}

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *arc = lv_event_get_target(e);
    /* 5% steps: finer is invisible, and a coarse step is easier to land from the driver's seat. */
    int32_t v = (lv_arc_get_value(arc) + 2) / 5 * 5;
    if (v < APP_SETTINGS_BRIGHTNESS_MIN) {
        v = APP_SETTINGS_BRIGHTNESS_MIN;
    }
    if (v != lv_arc_get_value(arc)) {
        lv_arc_set_value(arc, v);
    }

    /* Applied while dragging, so the change is visible; flash is written once, on release. */
    bsp_display_brightness_set((int)v);
    app_settings_set_brightness((uint8_t)v);
    lv_label_set_text_fmt(g.bright_label, "%" PRId32 "%%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_settings_commit();
    }
}

static void to_gauge_clicked_cb(lv_event_t *e)
{
    (void)e;
    app_ui_show_tile(APP_UI_TILE_GAUGE, true);
}

static void build_home(lv_obj_t *tile)
{
    const int32_t w  = g.board->width_px;
    const int32_t cx = w / 2;

    g.home = box(tile);
    lv_obj_set_size(g.home, LV_PCT(100), LV_PCT(100));

    /*
     * Brightness on an arc around the top of the rim: the setting used most in a car (day, dusk,
     * night), the biggest target on the page, and a shape the round panel suits. Only the ring
     * itself takes touches (ADV_HITTEST), so the buttons inside it are unaffected.
     */
    lv_obj_t *arc = lv_arc_create(g.home);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, w - 22, w - 22);
    lv_obj_center(arc);
    lv_arc_set_rotation(arc, 0);
    lv_arc_set_bg_angles(arc, 205, 335);
    lv_arc_set_range(arc, APP_SETTINGS_BRIGHTNESS_MIN, APP_SETTINGS_BRIGHTNESS_MAX);
    lv_arc_set_value(arc, app_settings_get()->brightness);
    lv_obj_set_style_arc_width(arc, 26, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 26, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(arc, lv_color_hex(COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(arc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 6, LV_PART_KNOB);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(arc, 14);
    lv_obj_add_event_cb(arc, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(arc, brightness_cb, LV_EVENT_RELEASED, NULL);

    g.bright_label = label(g.home, &st.body, "");
    lv_obj_set_style_text_font(g.bright_label, FONT_TITLE, 0);
    lv_label_set_text_fmt(g.bright_label, "%u%%", (unsigned)app_settings_get()->brightness);
    lv_obj_align(g.bright_label, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_t *bc = label(g.home, &st.caption, "BRIGHTNESS");
    lv_obj_set_style_text_font(bc, FONT_SMALL, 0);
    lv_obj_set_style_text_letter_space(bc, 2, 0);
    lv_obj_align(bc, LV_ALIGN_TOP_MID, 0, 92);

    /* Six round buttons, three by two, in the wide middle of the circle. */
    const int32_t col = w * 112 / 466;
    const int32_t rows[2] = {w * 168 / 466, w * 292 / 466};
    for (size_t i = 0; i < MENU_COUNT; i++) {
        int32_t x = cx + (int32_t)((int)(i % 3) - 1) * col;
        int32_t y = rows[i / 3];

        lv_obj_t *b = button(g.home, MENU_BTN, MENU_BTN, true);
        lv_obj_set_pos(b, x - MENU_BTN / 2, y - MENU_BTN / 2);
        lv_obj_add_event_cb(b, menu_clicked_cb, LV_EVENT_CLICKED, (void *)&k_menu[i]);
        lv_obj_t *icon = label(b, &st.body, k_menu[i].icon);
        lv_obj_set_style_text_font(icon, FONT_ICON, 0);
        lv_obj_center(icon);
        if (k_menu[i].build == build_system) {
            g.system_icon = icon;
        }

        lv_obj_t *cap = label(g.home, &st.caption, k_menu[i].name);
        lv_obj_set_style_text_font(cap, FONT_SMALL, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(cap, LV_ALIGN_TOP_MID, x - cx, y + MENU_BTN / 2 + 6);
    }

    /* Back to the dial, where "Back" sits on every page: bottom centre. Swiping down works too. */
    lv_obj_t *to_gauge = button(g.home, PILL_W, PILL_H, true);
    lv_obj_align(to_gauge, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_border_width(to_gauge, 2, 0);
    lv_obj_set_style_border_color(to_gauge, lv_color_hex(COLOR_PRESSED), 0);
    lv_obj_add_event_cb(to_gauge, to_gauge_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = label(to_gauge, &st.body, LV_SYMBOL_DOWN "  Done");
    lv_obj_center(tl);
}

/* ------------------------------------------------------------------ interface ------ */

void settings_build(lv_obj_t *tile, const board_profile_t *board, const gauge_config_t *cfg)
{
    styles_init();
    g.tile  = tile;
    g.board = board;

    lv_obj_set_style_bg_color(tile, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    const esp_app_desc_t *desc = esp_app_get_description();
    snprintf(g.update_text, sizeof(g.update_text), "%s", desc ? desc->version : "unknown");
    snprintf(g.network_text, sizeof(g.network_text), "starting...");
    settings_set_gauge_info(cfg);

    build_home(tile);
    ESP_LOGI(TAG, "settings ready");
}

void settings_tick(bool settings_visible)
{
    if (!settings_visible) {
        return;
    }
    /* A glance should find the gauge: nobody drives with the settings open on purpose. */
    if (lv_display_get_inactive_time(NULL) > SETTINGS_IDLE_MS) {
        close_page();
        app_ui_show_tile(APP_UI_TILE_GAUGE, true);
        return;
    }
    if (g.page != NULL && g.page_tick != NULL) {
        g.page_tick(g.page);
    }
}

void settings_tile_changed(bool settings_visible)
{
    if (!settings_visible) {
        close_page();
    }
}

void settings_set_gauge_info(const gauge_config_t *cfg)
{
    snprintf(g.gauge_info_text, sizeof(g.gauge_info_text), "Showing %s, %.0f to %.0f %s", cfg->id,
             (double)cfg->source.min, (double)cfg->source.max, cfg->source.unit);
    if (g.gauge_info != NULL) {
        lv_label_set_text(g.gauge_info, g.gauge_info_text);
    }
}

void settings_set_gauge_list(const char (*ids)[GAUGE_CONFIG_MAX_ID_LEN], int count, const char *active)
{
    if (ids == NULL || count < 0) {
        count = 0;
    }
    if (count > GAUGE_STORE_MAX_GAUGES) {
        count = GAUGE_STORE_MAX_GAUGES;
    }
    g.gauge_active = 0;
    int m          = 0;
    for (int i = 0; i < count; i++) {
        snprintf(g.gauge_ids[i], GAUGE_CONFIG_MAX_ID_LEN, "%s", ids[i]);
        if (i > 0) {
            g.gauge_map[m++] = "\n";
        }
        g.gauge_map[m++] = g.gauge_ids[i];
        if (active != NULL && strcmp(ids[i], active) == 0) {
            g.gauge_active = i;
        }
    }
    g.gauge_map[m] = "";
    g.gauge_count  = count;
}

void settings_set_gauge_selected_cb(app_ui_gauge_selected_cb_t cb) { g.on_gauge_selected = cb; }
void settings_set_network_reset_cb(app_ui_network_reset_cb_t cb)   { g.on_network_reset = cb; }
void settings_set_firmware_tap_cb(app_ui_firmware_tap_cb_t cb)     { g.on_firmware_tap = cb; }

/* Only redraws on a change: these are pushed every second whether or not anything moved. */
static void set_cached(char *cache, size_t len, lv_obj_t *lbl, const char *text)
{
    if (strncmp(cache, text, len) == 0) {
        return;
    }
    snprintf(cache, len, "%s", text);
    if (lbl != NULL) {
        lv_label_set_text_static(lbl, cache);
    }
}

void settings_set_network_status(const char *state, const char *detail)
{
    char next[NETWORK_TEXT_LEN];
    snprintf(next, sizeof(next), "%s%s%s", state, (detail && detail[0]) ? "\n" : "",
             detail ? detail : "");
    set_cached(g.network_text, sizeof(g.network_text), g.net_label, next);
}

void settings_set_update_status(const char *text)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char                  next[UPDATE_TEXT_LEN];
    snprintf(next, sizeof(next), "%s%s%s", desc ? desc->version : "unknown",
             (text && text[0]) ? "\n" : "", text ? text : "");
    set_cached(g.update_text, sizeof(g.update_text), g.fw_label, next);
}

void settings_set_warning(const char *text)
{
    snprintf(g.warning_text, sizeof(g.warning_text), "%s", text ? text : "");
    if (g.system_icon != NULL) {
        lv_obj_set_style_text_color(
            g.system_icon, lv_color_hex(g.warning_text[0] ? COLOR_WARN : COLOR_TEXT), 0);
    }
}
