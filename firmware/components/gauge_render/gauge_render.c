/*
 * Dial gauge renderer.
 *
 * Strategy (docs/adr/0003-static-background-plus-needle-sprite.md):
 *   - The face is rasterised once into a PSRAM RGB565 canvas and becomes the background, so
 *     an ornate dial costs the same per frame as a plain one: nothing.
 *   - Only the needle and the readout move, and both are kept to a tight bounding box. That
 *     bounds the per-frame dirty region, which is what makes 60fps reachable at all.
 *
 * The needle is drawn by a custom LV_EVENT_DRAW_MAIN callback rather than by rotating an
 * image. Rotating an lv_image looks simpler, but LVGL grows a rotated object's invalidation
 * area by a single scalar on all sides, so a long thin needle pivoting about its end
 * invalidates a square the size of the whole dial. Measured, that cost 28.8% of the screen
 * per frame and held only 45fps. Drawing the rotated triangle ourselves lets the object be
 * the needle's exact bounding box instead. See docs/performance.md.
 *
 * Angle convention: the schema uses degrees clockwise from 12 o'clock (see
 * docs/gauge-config-schema.md). LVGL arc drawing puts 0 at 3 o'clock, hence the -90
 * conversion in arc_deg().
 */

#include "gauge_render.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "gauge_render";

#define DEG2RAD(d) ((float)(d) * 3.14159265358979f / 180.0f)

struct gauge_render_t {
    gauge_config_t         cfg;
    const board_profile_t *board;

    lv_obj_t *parent;
    lv_obj_t *face;      /**< Canvas holding the pre-rendered dial. */
    lv_obj_t *needle;    /**< Custom-drawn, sized to the needle's bounding box. */
    lv_obj_t *hub;       /**< Centre cap, drawn over the needle. */
    lv_obj_t *readout;

    void *face_buf;      /**< PSRAM. Owned. */

    int32_t cx, cy;      /**< Dial centre, relative to parent. */
    int32_t radius;      /**< Outer radius of the scale. */
    int32_t needle_len;  /**< Pivot to tip. */

    /* Needle geometry in absolute (screen) coordinates, recomputed on each value change. */
    bool               tri_valid;
    lv_point_precise_t tri[3];
    bool               line_valid;
    lv_point_precise_t line_p1, line_p2;
    int32_t            line_w;
    lv_area_t          prev_bbox;   /**< Area to repaint where the needle used to be. */
    bool               prev_valid;

    float displayed;     /**< Current damped value. */
    bool  valid;
    bool  alert_active;
    bool  alert_phase;   /**< Flash state. */

    lv_timer_t *flash_timer;

    int8_t                  active_alert_idx; /**< Index into cfg.alerts, or -1. */

    /* Peak-hold marker: same stationary-object, tight-invalidation approach as the needle. */
    lv_obj_t          *peak_marker;
    lv_obj_t          *peak_label;
    bool               peak_valid;
    float              peak_value;
    lv_point_precise_t peak_p1, peak_p2;
    lv_area_t          peak_bbox;
    bool               peak_bbox_valid;
    gauge_render_alert_cb_t alert_cb;
    void                   *alert_cb_user;
};

/* ------------------------------------------------------------------- helpers -------- */

static lv_color_t to_lv(gauge_color_t c)
{
    return lv_color_make(c.r, c.g, c.b);
}

/*
 * Fonts are compiled in, so this maps a schema font name onto one of them. An unknown name
 * falls back rather than failing: a typo in a config file must not blank the gauge.
 */
static const lv_font_t *font_by_name(const char *name)
{
    static const struct {
        const char      *name;
        const lv_font_t *font;
    } table[] = {
#if LV_FONT_MONTSERRAT_12
        {"montserrat_12", &lv_font_montserrat_12},
#endif
#if LV_FONT_MONTSERRAT_16
        {"montserrat_16", &lv_font_montserrat_16},
#endif
#if LV_FONT_MONTSERRAT_18
        {"montserrat_18", &lv_font_montserrat_18},
#endif
#if LV_FONT_MONTSERRAT_20
        {"montserrat_20", &lv_font_montserrat_20},
#endif
#if LV_FONT_MONTSERRAT_22
        {"montserrat_22", &lv_font_montserrat_22},
#endif
#if LV_FONT_MONTSERRAT_24
        {"montserrat_24", &lv_font_montserrat_24},
#endif
#if LV_FONT_MONTSERRAT_26
        {"montserrat_26", &lv_font_montserrat_26},
#endif
#if LV_FONT_MONTSERRAT_48
        {"montserrat_48", &lv_font_montserrat_48},
#endif
    };

    if (name != NULL && name[0] != '\0') {
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
            if (strcmp(name, table[i].name) == 0) {
                return table[i].font;
            }
        }
        ESP_LOGW(TAG, "unknown font '%s', using default", name);
    }

    return LV_FONT_DEFAULT;
}

/** Value -> schema angle (degrees clockwise from 12 o'clock). */
static float value_to_deg(const gauge_render_t *g, float value)
{
    const gauge_config_t *c = &g->cfg;

    float span = c->source.max - c->source.min;
    float frac = (span > 0.0f) ? (value - c->source.min) / span : 0.0f;

    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }

    return c->face.start_angle + frac * c->face.sweep;
}

/** Schema angle -> LVGL arc angle (0 at 3 o'clock). */
static int32_t arc_deg(float schema_deg)
{
    return (int32_t)lroundf(schema_deg - 90.0f);
}

static void polar(int32_t cx, int32_t cy, float radius, float schema_deg,
                  int32_t *x, int32_t *y)
{
    float r = DEG2RAD(schema_deg);
    *x = cx + (int32_t)lroundf(radius * sinf(r));
    *y = cy - (int32_t)lroundf(radius * cosf(r));
}

static void draw_text(lv_layer_t *layer, const char *text, const lv_font_t *font,
                      lv_color_t color, int32_t cx, int32_t cy)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text        = text;
    dsc.text_length = strlen(text);
    dsc.text_local  = 1; /* text is on our stack, so LVGL must copy it */
    dsc.font        = font;
    dsc.color       = color;
    dsc.align       = LV_TEXT_ALIGN_CENTER;

    /* Generous box centred on the point, so the centred text lands where we asked. */
    int32_t   h = lv_font_get_line_height(font);
    int32_t   w = h * 8;
    lv_area_t a = {
        .x1 = cx - w / 2, .y1 = cy - h / 2,
        .x2 = cx + w / 2, .y2 = cy + h / 2,
    };

    lv_draw_label(layer, &dsc, &a);
}

/* ------------------------------------------------------------- face pre-render ------ */

static void render_face(gauge_render_t *g)
{
    const gauge_config_t *c  = &g->cfg;
    lv_obj_t             *cv = g->face;

    lv_canvas_fill_bg(cv, to_lv(c->face.background), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);

    int32_t cx = g->cx;
    int32_t cy = g->cy;

    /* --- colour bands --- */
    int32_t widest_band = 0;
    for (uint8_t i = 0; i < c->face.band_count; i++) {
        const gauge_band_t *b = &c->face.bands[i];

        lv_draw_arc_dsc_t dsc;
        lv_draw_arc_dsc_init(&dsc);
        dsc.color       = to_lv(b->color);
        dsc.width       = b->width_px;
        dsc.radius      = (uint16_t)g->radius;
        dsc.center.x    = cx;
        dsc.center.y    = cy;
        dsc.start_angle = arc_deg(value_to_deg(g, b->from));
        dsc.end_angle   = arc_deg(value_to_deg(g, b->to));
        dsc.opa         = LV_OPA_COVER;
        lv_draw_arc(&layer, &dsc);

        if (b->width_px > widest_band) {
            widest_band = b->width_px;
        }
    }

    /* Ticks and labels sit inside the band ring so they never collide with it. */
    int32_t tick_outer = g->radius - widest_band - (widest_band ? 6 : 0);

    /* --- ticks --- */
    if (c->face.ticks.present) {
        float min = c->source.min;
        float max = c->source.max;

        /* Minor ticks first, so major ticks draw over them where they coincide. */
        for (int pass = 0; pass < 2; pass++) {
            float step = (pass == 0) ? c->face.ticks.minor_every
                                     : c->face.ticks.major_every;
            if (step <= 0.0f) {
                continue;
            }

            int32_t len   = (pass == 0) ? c->face.ticks.minor_len_px
                                        : c->face.ticks.major_len_px;
            int32_t width = (pass == 0) ? c->face.ticks.minor_width_px
                                        : c->face.ticks.major_width_px;

            /* Guard against a step so small it would draw thousands of ticks. */
            int32_t count = (int32_t)((max - min) / step);
            if (count > 400) {
                ESP_LOGW(TAG, "tick step %.3f yields %" PRId32 " ticks, skipping",
                         (double)step, count);
                continue;
            }

            for (int32_t i = 0; i <= count; i++) {
                float v   = min + (float)i * step;
                float deg = value_to_deg(g, v);

                int32_t x1, y1, x2, y2;
                polar(cx, cy, (float)tick_outer, deg, &x1, &y1);
                polar(cx, cy, (float)(tick_outer - len), deg, &x2, &y2);

                lv_draw_line_dsc_t dsc;
                lv_draw_line_dsc_init(&dsc);
                dsc.color = to_lv(c->face.ticks.color);
                dsc.width = width;
                dsc.opa   = LV_OPA_COVER;
                dsc.p1.x  = x1;
                dsc.p1.y  = y1;
                dsc.p2.x  = x2;
                dsc.p2.y  = y2;
                lv_draw_line(&layer, &dsc);
            }
        }
    }

    /* --- numeric labels --- */
    if (c->face.labels.present) {
        float step = (c->face.labels.every > 0.0f) ? c->face.labels.every
                                                   : c->face.ticks.major_every;
        if (step > 0.0f) {
            const lv_font_t *font = font_by_name(c->face.labels.font);

            int32_t r = c->face.labels.radius_px;
            if (r <= 0) {
                r = tick_outer - c->face.ticks.major_len_px - lv_font_get_line_height(font);
            }

            int32_t count = (int32_t)((c->source.max - c->source.min) / step);
            if (count > 100) {
                ESP_LOGW(TAG, "label step %.3f yields too many labels, skipping",
                         (double)step);
            } else {
                for (int32_t i = 0; i <= count; i++) {
                    float v = c->source.min + (float)i * step;

                    char text[16];
                    snprintf(text, sizeof(text), c->face.labels.format, (double)v);

                    int32_t x, y;
                    polar(cx, cy, (float)r, value_to_deg(g, v), &x, &y);
                    draw_text(&layer, text, font, to_lv(c->face.labels.color), x, y);
                }
            }
        }
    }

    /* --- static titles (baked in, so they cost nothing per frame) --- */
    for (uint8_t i = 0; i < c->title_count; i++) {
        const gauge_title_t *t = &c->titles[i];
        int32_t x = (t->x == INT16_MIN) ? cx : t->x;
        draw_text(&layer, t->text, font_by_name(t->font), to_lv(t->color), x, t->y);
    }

    lv_canvas_finish_layer(cv, &layer);
}

/* ------------------------------------------------------------------- needle --------- */

static void needle_draw_cb(lv_event_t *e)
{
    gauge_render_t *g     = lv_event_get_user_data(e);
    lv_layer_t     *layer = lv_event_get_layer(e);

    if (layer == NULL) {
        return;
    }

    lv_color_t color = to_lv(g->cfg.needle.color);

    /* The needle flashes in the alert colour along with the readout. */
    if (g->alert_active && g->alert_phase && g->active_alert_idx >= 0) {
        color = to_lv(g->cfg.alerts[g->active_alert_idx].color);
    }

    if (g->line_valid) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color       = color;
        dsc.width       = g->line_w;
        dsc.opa         = LV_OPA_COVER;
        dsc.round_start = 1;
        dsc.round_end   = 1;
        dsc.p1          = g->line_p1;
        dsc.p2          = g->line_p2;
        lv_draw_line(layer, &dsc);
    }

    if (g->tri_valid) {
        lv_draw_triangle_dsc_t dsc;
        lv_draw_triangle_dsc_init(&dsc);
        dsc.color = color;
        dsc.opa   = LV_OPA_COVER;
        dsc.p[0]  = g->tri[0];
        dsc.p[1]  = g->tri[1];
        dsc.p[2]  = g->tri[2];
        lv_draw_triangle(layer, &dsc);
    }
}

/*
 * Recompute the needle polygon for the current angle and resize the object to its exact
 * bounding box. Keeping the object tight is what keeps the dirty region small: LVGL
 * invalidates the old and new areas on a coordinate change, and nothing more.
 */
static void update_needle(gauge_render_t *g)
{
    const gauge_config_t *c = &g->cfg;

    /* Snapshot the current geometry so an update that lands on the same pixels can be skipped. */
    bool               old_tri_valid  = g->tri_valid;
    lv_point_precise_t old_tri[3]     = {g->tri[0], g->tri[1], g->tri[2]};
    bool               old_line_valid = g->line_valid;
    lv_point_precise_t old_p1         = g->line_p1;
    lv_point_precise_t old_p2         = g->line_p2;

    /* Absolute centre: the parent may not be at the screen origin. */
    lv_area_t pa;
    lv_obj_get_coords(g->parent, &pa);
    float acx = (float)(pa.x1 + g->cx);
    float acy = (float)(pa.y1 + g->cy);

    float deg = value_to_deg(g, g->displayed);
    float rad = DEG2RAD(deg);

    /* Unit vector along the needle (towards the tip) and its perpendicular. */
    float dx = sinf(rad),  dy = -cosf(rad);
    float px = cosf(rad),  py =  sinf(rad);

    float len  = (float)g->needle_len;
    float hw   = (float)c->needle.width_px * 0.5f;
    float tail = (float)c->needle.tail_px;

    if (hw < 1.0f) {
        hw = 1.0f;
    }

    float tip_x = acx + dx * len;
    float tip_y = acy + dy * len;

    g->tri_valid  = false;
    g->line_valid = false;

    switch (c->needle.style) {
    case GAUGE_NEEDLE_LINE: {
        g->line_valid = true;
        g->line_w     = c->needle.width_px;
        g->line_p1.x  = (int32_t)lroundf(tip_x);
        g->line_p1.y  = (int32_t)lroundf(tip_y);
        g->line_p2.x  = (int32_t)lroundf(acx - dx * tail);
        g->line_p2.y  = (int32_t)lroundf(acy - dy * tail);
        break;
    }

    case GAUGE_NEEDLE_ARROW: {
        float head = len * 0.25f;

        g->line_valid = true;
        g->line_w     = (c->needle.width_px / 2 > 0) ? c->needle.width_px / 2 : 1;
        g->line_p1.x  = (int32_t)lroundf(acx + dx * (len - head));
        g->line_p1.y  = (int32_t)lroundf(acy + dy * (len - head));
        g->line_p2.x  = (int32_t)lroundf(acx - dx * tail);
        g->line_p2.y  = (int32_t)lroundf(acy - dy * tail);

        g->tri_valid = true;
        g->tri[0].x  = (int32_t)lroundf(tip_x);
        g->tri[0].y  = (int32_t)lroundf(tip_y);
        g->tri[1].x  = (int32_t)lroundf(acx + dx * (len - head) + px * hw * 2.0f);
        g->tri[1].y  = (int32_t)lroundf(acy + dy * (len - head) + py * hw * 2.0f);
        g->tri[2].x  = (int32_t)lroundf(acx + dx * (len - head) - px * hw * 2.0f);
        g->tri[2].y  = (int32_t)lroundf(acy + dy * (len - head) - py * hw * 2.0f);
        break;
    }

    case GAUGE_NEEDLE_TAPER:
    default: {
        g->tri_valid = true;
        g->tri[0].x  = (int32_t)lroundf(tip_x);
        g->tri[0].y  = (int32_t)lroundf(tip_y);
        g->tri[1].x  = (int32_t)lroundf(acx + px * hw);
        g->tri[1].y  = (int32_t)lroundf(acy + py * hw);
        g->tri[2].x  = (int32_t)lroundf(acx - px * hw);
        g->tri[2].y  = (int32_t)lroundf(acy - py * hw);

        if (tail > 0.0f) {
            g->line_valid = true;
            g->line_w     = c->needle.width_px;
            g->line_p1.x  = (int32_t)lroundf(acx);
            g->line_p1.y  = (int32_t)lroundf(acy);
            g->line_p2.x  = (int32_t)lroundf(acx - dx * tail);
            g->line_p2.y  = (int32_t)lroundf(acy - dy * tail);
        }
        break;
    }
    }

    /*
     * Nothing moved on screen, so repaint nothing. Without this every value update invalidated
     * the needle even when damping had settled or the input was constant. Measured with a
     * steady 22.5 psi telemetry feed: ~17 redraws a second of an unchanged needle, for as long as
     * the feed ran. A real sensor at idle would do the same at its full sampling rate.
     */
    if (g->prev_valid && old_tri_valid == g->tri_valid && old_line_valid == g->line_valid &&
        memcmp(old_tri, g->tri, sizeof(old_tri)) == 0 &&
        memcmp(&old_p1, &g->line_p1, sizeof(old_p1)) == 0 &&
        memcmp(&old_p2, &g->line_p2, sizeof(old_p2)) == 0) {
        return;
    }

    /* Bounding box over whatever we are about to draw, plus slack for anti-aliasing. */
    int32_t x1 = INT32_MAX, y1 = INT32_MAX, x2 = INT32_MIN, y2 = INT32_MIN;

#define TRACK(PX, PY)                       \
    do {                                    \
        int32_t vx = (int32_t)(PX);         \
        int32_t vy = (int32_t)(PY);         \
        if (vx < x1) x1 = vx;               \
        if (vx > x2) x2 = vx;               \
        if (vy < y1) y1 = vy;               \
        if (vy > y2) y2 = vy;               \
    } while (0)

    if (g->tri_valid) {
        for (int i = 0; i < 3; i++) {
            TRACK(g->tri[i].x, g->tri[i].y);
        }
    }
    if (g->line_valid) {
        TRACK(g->line_p1.x, g->line_p1.y);
        TRACK(g->line_p2.x, g->line_p2.y);
    }
#undef TRACK

    int32_t slack = (g->line_valid ? g->line_w : 0) / 2 + 3;
    x1 -= slack;
    y1 -= slack;
    x2 += slack;
    y2 += slack;

    /*
     * The object itself stays put and covers the whole sweep, so there is no layout churn.
     * Only these two tight rectangles are repainted: where the needle was, and where it now
     * is. Resizing the object instead made LVGL invalidate far more than the needle occupies.
     */
    lv_area_t bbox = {.x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2};

    if (g->prev_valid) {
        lv_obj_invalidate_area(g->needle, &g->prev_bbox);
    }
    lv_obj_invalidate_area(g->needle, &bbox);

    g->prev_bbox  = bbox;
    g->prev_valid = true;
}

/* --------------------------------------------------------------------- peak --------- */

static void peak_draw_cb(lv_event_t *e)
{
    gauge_render_t *g     = lv_event_get_user_data(e);
    lv_layer_t     *layer = lv_event_get_layer(e);

    if (layer == NULL || !g->peak_valid) {
        return;
    }

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color       = to_lv(g->cfg.peak.color);
    dsc.width       = g->cfg.peak.width_px;
    dsc.opa         = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end   = 1;
    dsc.p1          = g->peak_p1;
    dsc.p2          = g->peak_p2;
    lv_draw_line(layer, &dsc);
}

static void set_peak_label(gauge_render_t *g)
{
    if (g->peak_label == NULL) {
        return;
    }

    char num[32];
    if (g->peak_valid) {
        snprintf(num, sizeof(num), g->cfg.peak.format, (double)g->peak_value);
    } else {
        snprintf(num, sizeof(num), "--");
    }

    char text[80];
    snprintf(text, sizeof(text), "%s%s", g->cfg.peak.prefix, num);
    if (strcmp(lv_label_get_text(g->peak_label), text) != 0) {
        lv_label_set_text(g->peak_label, text);
    }
}

/*
 * Track the highest raw value and move the marker when it rises. The marker only repaints when
 * it lands on different pixels, so a value sitting below its peak costs nothing.
 */
static void update_peak(gauge_render_t *g, float value, bool valid)
{
    if (g->peak_marker == NULL || !valid) {
        return;
    }
    if (g->peak_valid && value <= g->peak_value) {
        return;
    }

    g->peak_value = value;
    g->peak_valid = true;
    set_peak_label(g);

    lv_area_t pa;
    lv_obj_get_coords(g->parent, &pa);
    float acx = (float)(pa.x1 + g->cx);
    float acy = (float)(pa.y1 + g->cy);

    float rad   = DEG2RAD(value_to_deg(g, value));
    float dx    = sinf(rad);
    float dy    = -cosf(rad);
    float outer = (float)g->radius;
    float inner = outer - (float)g->cfg.peak.length_px;

    lv_point_precise_t p1 = {.x = (int32_t)lroundf(acx + dx * outer),
                             .y = (int32_t)lroundf(acy + dy * outer)};
    lv_point_precise_t p2 = {.x = (int32_t)lroundf(acx + dx * inner),
                             .y = (int32_t)lroundf(acy + dy * inner)};

    if (g->peak_bbox_valid && memcmp(&p1, &g->peak_p1, sizeof(p1)) == 0 &&
        memcmp(&p2, &g->peak_p2, sizeof(p2)) == 0) {
        return; /* same pixels: nothing to repaint */
    }

    g->peak_p1 = p1;
    g->peak_p2 = p2;

    int32_t   slack = g->cfg.peak.width_px / 2 + 3;
    lv_area_t bbox  = {
        .x1 = LV_MIN(p1.x, p2.x) - slack, .y1 = LV_MIN(p1.y, p2.y) - slack,
        .x2 = LV_MAX(p1.x, p2.x) + slack, .y2 = LV_MAX(p1.y, p2.y) + slack,
    };

    if (g->peak_bbox_valid) {
        lv_obj_invalidate_area(g->peak_marker, &g->peak_bbox);
    }
    lv_obj_invalidate_area(g->peak_marker, &bbox);
    g->peak_bbox       = bbox;
    g->peak_bbox_valid = true;
}

void gauge_render_reset_peak(gauge_render_t *g)
{
    if (g == NULL || g->peak_marker == NULL) {
        return;
    }
    if (g->peak_bbox_valid) {
        lv_obj_invalidate_area(g->peak_marker, &g->peak_bbox);
    }
    g->peak_valid      = false;
    g->peak_bbox_valid = false;
    set_peak_label(g);
}

/* ------------------------------------------------------------------ readout --------- */

static void update_readout(gauge_render_t *g)
{
    if (g->readout == NULL) {
        return;
    }

    const gauge_config_t *c = &g->cfg;

    if (!g->valid) {
        /* Dashes, not a stale number: admit the sensor is gone. */
        lv_label_set_text(g->readout, "---");
        return;
    }

    char num[32];
    snprintf(num, sizeof(num), c->readout.format, (double)g->displayed);

    char text[96];
    snprintf(text, sizeof(text), "%s%s%s", c->readout.prefix, num, c->readout.suffix);

    /* lv_label_set_text() invalidates even when the text is identical, so compare first. */
    if (strcmp(lv_label_get_text(g->readout), text) != 0) {
        lv_label_set_text(g->readout, text);
    }
}

static void apply_alert_style(gauge_render_t *g)
{
    /*
     * The needle's colour depends on the flash phase, but update_needle() only repaints when the
     * needle moves. A steady value above a threshold would otherwise never show the flash, so
     * repaint the needle's current footprint on every phase change.
     */
    if (g->needle != NULL && g->prev_valid) {
        lv_obj_invalidate_area(g->needle, &g->prev_bbox);
    }

    if (g->readout == NULL) {
        return;
    }

    lv_color_t color = to_lv(g->cfg.readout.color);

    if (g->alert_active && g->alert_phase) {
        /* Use the colour of the alert that actually fired, not a generic one. */
        for (uint8_t i = 0; i < g->cfg.alert_count; i++) {
            const gauge_alert_t *a = &g->cfg.alerts[i];
            bool fired = (a->has_above && g->displayed > a->above) ||
                         (a->has_below && g->displayed < a->below);
            if (fired) {
                color = to_lv(a->color);
                break;
            }
        }
    }

    lv_obj_set_style_text_color(g->readout, color, LV_PART_MAIN);
}

static void flash_timer_cb(lv_timer_t *t)
{
    gauge_render_t *g = lv_timer_get_user_data(t);
    g->alert_phase    = !g->alert_phase;
    apply_alert_style(g);
}

static void evaluate_alerts(gauge_render_t *g)
{
    int   idx      = -1;
    float flash_hz = 0.0f;

    for (uint8_t i = 0; i < g->cfg.alert_count; i++) {
        const gauge_alert_t *a = &g->cfg.alerts[i];
        if ((a->has_above && g->displayed > a->above) ||
            (a->has_below && g->displayed < a->below)) {
            idx      = i;
            flash_hz = a->flash_hz;
            break;
        }
    }

    bool active = (idx >= 0);
    if (active == g->alert_active) {
        return;
    }
    g->alert_active = active;

    /* Report the alert that fired, or on clearing, the one that was active. */
    int reported = active ? idx : g->active_alert_idx;
    g->active_alert_idx = (int8_t)idx;
    if (g->alert_cb != NULL && reported >= 0) {
        g->alert_cb(&g->cfg.alerts[reported], active, g->alert_cb_user);
    }

    if (g->flash_timer != NULL) {
        lv_timer_delete(g->flash_timer);
        g->flash_timer = NULL;
    }

    if (active && flash_hz > 0.0f) {
        uint32_t period = (uint32_t)(500.0f / flash_hz); /* half period per toggle */
        g->flash_timer  = lv_timer_create(flash_timer_cb, period, g);
        g->alert_phase  = true;
    } else {
        /* Steady colour when flash-hz is 0, plain colour when the alert clears. */
        g->alert_phase = active;
    }

    apply_alert_style(g);
}

/* ------------------------------------------------------------------- public --------- */

esp_err_t gauge_render_create(lv_obj_t *parent, const gauge_config_t *cfg,
                              const board_profile_t *board, gauge_render_t **out)
{
    ESP_RETURN_ON_FALSE(parent && cfg && board && out, ESP_ERR_INVALID_ARG, TAG, "bad args");

    gauge_render_t *g = calloc(1, sizeof(gauge_render_t));
    ESP_RETURN_ON_FALSE(g != NULL, ESP_ERR_NO_MEM, TAG, "no memory for gauge");

    g->cfg    = *cfg;
    g->board  = board;
    g->parent = parent;
    g->active_alert_idx = -1;

    int32_t w = board->width_px;
    int32_t h = board->height_px;
    g->cx     = w / 2;
    g->cy     = h / 2;

    g->radius = (cfg->face.radius_px > 0) ? cfg->face.radius_px
                                          : board_profile_max_radius(board);

    esp_err_t ret = ESP_OK; /* ESP_GOTO_ON_FALSE assigns to a variable named `ret` */

    /* --- face canvas, full panel, in PSRAM --- */
    uint32_t face_stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    size_t   face_size   = (size_t)face_stride * h;

    g->face_buf = heap_caps_malloc(face_size, MALLOC_CAP_SPIRAM);
    ESP_GOTO_ON_FALSE(g->face_buf != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "no PSRAM for %u byte face buffer", (unsigned)face_size);

    g->face = lv_canvas_create(parent);
    ESP_GOTO_ON_FALSE(g->face != NULL, ESP_ERR_NO_MEM, fail, TAG, "face canvas failed");
    lv_canvas_set_buffer(g->face, g->face_buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(g->face, 0, 0);

    render_face(g);

    /* --- peak marker: under the needle, over the face --- */
    if (cfg->peak.present) {
        g->peak_marker = lv_obj_create(parent);
        ESP_GOTO_ON_FALSE(g->peak_marker != NULL, ESP_ERR_NO_MEM, fail, TAG, "peak marker failed");
        lv_obj_remove_style_all(g->peak_marker);
        lv_obj_remove_flag(g->peak_marker, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(g->peak_marker, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(g->peak_marker, g->radius * 2 + 16, g->radius * 2 + 16);
        lv_obj_set_pos(g->peak_marker, g->cx - g->radius - 8, g->cy - g->radius - 8);
        lv_obj_add_event_cb(g->peak_marker, peak_draw_cb, LV_EVENT_DRAW_MAIN, g);
    }

    /* --- needle: a bare object sized to the polygon, drawn by needle_draw_cb --- */
    g->needle_len = (cfg->needle.length_px > 0) ? cfg->needle.length_px
                                                : (int32_t)((float)g->radius * 0.72f);

    g->needle = lv_obj_create(parent);
    ESP_GOTO_ON_FALSE(g->needle != NULL, ESP_ERR_NO_MEM, fail, TAG, "needle failed");
    lv_obj_remove_style_all(g->needle);
    lv_obj_remove_flag(g->needle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g->needle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g->needle, needle_draw_cb, LV_EVENT_DRAW_MAIN, g);

    /*
     * Covers the whole area the needle can sweep, and never moves. Drawing is clipped to
     * whatever we invalidate, so a large object costs nothing -- see update_needle().
     */
    {
        int32_t reach = g->needle_len + cfg->needle.tail_px + cfg->needle.width_px + 8;
        lv_obj_set_size(g->needle, reach * 2, reach * 2);
        lv_obj_set_pos(g->needle, g->cx - reach, g->cy - reach);
    }

    /* --- centre hub, above the needle --- */
    if (cfg->needle.pivot_radius_px > 0) {
        int32_t r = cfg->needle.pivot_radius_px;
        g->hub    = lv_obj_create(parent);
        if (g->hub != NULL) {
            lv_obj_remove_style_all(g->hub);
            lv_obj_remove_flag(g->hub, LV_OBJ_FLAG_SCROLLABLE);
            /* Taps on the dial clear the peak; the hub must not swallow them. */
            lv_obj_remove_flag(g->hub, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(g->hub, r * 2, r * 2);
            lv_obj_set_pos(g->hub, g->cx - r, g->cy - r);
            lv_obj_set_style_radius(g->hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(g->hub, to_lv(cfg->needle.color), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(g->hub, LV_OPA_COVER, LV_PART_MAIN);
        }
    }

    /*
     * Readout: sized to its content, not to the panel. A full-width label would dirty the
     * whole width of the screen on every value change. lv_obj_align is retained across size
     * changes, so the text stays centred as its width varies.
     */
    if (cfg->readout.present) {
        g->readout = lv_label_create(parent);
        ESP_GOTO_ON_FALSE(g->readout != NULL, ESP_ERR_NO_MEM, fail, TAG, "readout failed");

        lv_obj_set_style_text_font(g->readout, font_by_name(cfg->readout.font), LV_PART_MAIN);
        lv_obj_set_style_text_color(g->readout, to_lv(cfg->readout.color), LV_PART_MAIN);
        lv_obj_set_width(g->readout, LV_SIZE_CONTENT);

        int32_t rx = (cfg->readout.x == INT16_MIN) ? g->cx : cfg->readout.x;
        lv_obj_align(g->readout, LV_ALIGN_TOP_MID, rx - w / 2, cfg->readout.y);
    }

    /* --- peak readout, below the live readout --- */
    if (cfg->peak.present && cfg->peak.show_value) {
        g->peak_label = lv_label_create(parent);
        ESP_GOTO_ON_FALSE(g->peak_label != NULL, ESP_ERR_NO_MEM, fail, TAG, "peak label failed");

        lv_obj_set_style_text_font(g->peak_label, font_by_name(cfg->peak.font), LV_PART_MAIN);
        lv_obj_set_style_text_color(g->peak_label, to_lv(cfg->peak.color), LV_PART_MAIN);
        lv_obj_set_width(g->peak_label, LV_SIZE_CONTENT);
        lv_label_set_text(g->peak_label, "");

        int32_t py = cfg->peak.value_y;
        if (py == INT16_MIN) {
            py = cfg->readout.present
                     ? cfg->readout.y + lv_font_get_line_height(font_by_name(cfg->readout.font)) + 2
                     : g->cy + 60;
        }
        lv_obj_align(g->peak_label, LV_ALIGN_TOP_MID, 0, py);
    }

    g->valid     = true;
    g->displayed = cfg->source.min;
    update_readout(g);
    update_needle(g);
    set_peak_label(g);

    ESP_LOGI(TAG, "gauge '%s' ready: r=%" PRId32 " needle_len=%" PRId32 " face=%uKB",
             cfg->id, g->radius, g->needle_len, (unsigned)(face_size / 1024));

    *out = g;
    return ESP_OK;

fail:
    gauge_render_destroy(g);
    return ret;
}

void gauge_render_destroy(gauge_render_t *g)
{
    if (g == NULL) {
        return;
    }

    if (g->flash_timer != NULL) {
        lv_timer_delete(g->flash_timer);
    }

    /* Delete the objects before the buffer they point into. */
    if (g->readout != NULL) {
        lv_obj_delete(g->readout);
    }
    if (g->peak_label != NULL) {
        lv_obj_delete(g->peak_label);
    }
    if (g->peak_marker != NULL) {
        lv_obj_delete(g->peak_marker);
    }
    if (g->hub != NULL) {
        lv_obj_delete(g->hub);
    }
    if (g->needle != NULL) {
        lv_obj_delete(g->needle);
    }
    if (g->face != NULL) {
        lv_obj_delete(g->face);
    }

    free(g->face_buf);
    free(g);
}

void gauge_render_set_value(gauge_render_t *g, float value, bool valid)
{
    if (g == NULL) {
        return;
    }

    bool validity_changed = (valid != g->valid);
    g->valid = valid;

    float target = valid ? value : g->cfg.source.min;

    if (target < g->cfg.source.min) {
        target = g->cfg.source.min;
    } else if (target > g->cfg.source.max) {
        target = g->cfg.source.max;
    }

    /*
     * Exponential smoothing. damping is the fraction of the remaining error retained each
     * update, so 0 tracks instantly and larger values lag more. A validity change snaps
     * rather than sweeping, since sweeping would imply a real reading.
     */
    float d = g->cfg.source.damping;
    if (d > 0.0f && !validity_changed) {
        g->displayed += (target - g->displayed) * (1.0f - d);
    } else {
        g->displayed = target;
    }

    update_peak(g, target, valid);
    update_needle(g);
    update_readout(g);
    evaluate_alerts(g);
}

float gauge_render_get_displayed(const gauge_render_t *g)
{
    return (g != NULL) ? g->displayed : 0.0f;
}

void gauge_render_set_alert_cb(gauge_render_t *g, gauge_render_alert_cb_t cb, void *user_data)
{
    if (g == NULL) {
        return;
    }
    g->alert_cb      = cb;
    g->alert_cb_user = user_data;
}
