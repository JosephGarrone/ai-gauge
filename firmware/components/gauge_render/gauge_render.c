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
 * Custom shapes (docs/adr/0006-custom-shapes-as-polygons.md) keep that property. A shaped
 * needle is rasterised by gauge_shape into A8 coverage masks sized to its current bounding box,
 * and blended untransformed, so the dirty region is still exactly where the needle was and is.
 * Shaped ticks are blended straight into the face canvas, and a shaped hub is rasterised once.
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

#include "gauge_shape.h"

static const char *TAG = "gauge_render";

#define DEG2RAD(d) ((float)(d) * 3.14159265358979f / 180.0f)

/*
 * One part of a custom needle or hub: an A8 coverage mask blended in a single colour. The
 * buffer is sized once for the part's worst-case bounds; the image header tracks the bounds of
 * the current rasterisation.
 */
typedef struct {
    lv_image_dsc_t img;
    uint8_t       *cov;           /**< PSRAM. Owned. NULL for an unused part. */
    lv_area_t      area;          /**< Where img lands, relative to the parent. */
    lv_color_t     color;
    bool           follows_alert; /**< Inherits the needle colour, so flashes with alerts. */
} shape_sprite_t;

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

    /* Custom needle and hub. Unused unless cfg.needle.shape / hub_shape is set. */
    shape_sprite_t    needle_parts[GAUGE_SHAPE_MAX_PARTS];
    shape_sprite_t    hub_parts[GAUGE_SHAPE_MAX_PARTS];
    float            *raster_acc;   /**< PSRAM scratch, sized for the largest part. Owned. */
    gauge_shape_vec_t outline[GAUGE_SHAPE_MAX_OUTLINE]; /**< Here, not on a task stack. */
    float             needle_extent;
    float             shape_deg;    /**< Angle the needle parts were last rasterised at. */
    bool              shape_valid;

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

    /* Session min/max of the raw input, shown on request in place of the readouts. */
    bool        minmax_valid;
    float       min_value, max_value;
    lv_obj_t   *recall_label;
    lv_timer_t *recall_timer;
    gauge_render_alert_cb_t alert_cb;
    void                   *alert_cb_user;

    /* The user's number preferences (gauge_render_set_number_format). */
    char  num_format[GAUGE_CONFIG_MAX_FORMAT_LEN]; /**< "" = the face's own formats. */
    float round_step;                              /**< 0 = no rounding. */
    bool  peak_value_hidden;

    /*
     * Sparkline strip: one column per pixel of history, re-rasterised into two A8 masks whenever a
     * column completes, and repainted only then. See docs/display-pipeline.md.
     */
    lv_obj_t      *trend;
    lv_area_t      trend_area;      /**< The strip itself, parent-relative. */
    int32_t        trend_w, trend_h;
    float         *trend_cols;      /**< PSRAM ring of column means; NAN = no valid reading. */
    uint16_t       trend_next;      /**< Ring slot the next column goes in. */
    uint16_t       trend_count;
    float          trend_sum;
    uint32_t       trend_n;
    uint32_t       trend_col_ms;
    uint32_t       trend_col_start;
    uint8_t       *trend_fill_cov;  /**< PSRAM, trend_w x trend_h. */
    uint8_t       *trend_line_cov;  /**< PSRAM, trend_w x trend_h. */
    lv_image_dsc_t trend_fill_img;
    lv_image_dsc_t trend_line_img;
    lv_color_t     trend_fill_color;
    bool           trend_head_valid;
    lv_point_t     trend_head;      /**< Newest point, strip-relative, for the dot. */
};

LV_FONT_DECLARE(lv_font_montserrat_digits_80)

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
#if LV_FONT_MONTSERRAT_32
        {"montserrat_32", &lv_font_montserrat_32},
#endif
#if LV_FONT_MONTSERRAT_48
        {"montserrat_48", &lv_font_montserrat_48},
#endif
        /* Digits, '.', '-', '+' and space only: tools/fonts/make_digits_font.py. */
        {"montserrat_digits_80", &lv_font_montserrat_digits_80},
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

/* --------------------------------------------------------------- custom shapes ------ */

/*
 * Allocate a sprite per part, each sized for that part's worst case at any angle, and resolve
 * its colour. Reports the largest mask in *need so one scratch buffer serves every part.
 */
static bool init_sprites(shape_sprite_t *sprites, const gauge_shape_t *shape,
                         gauge_color_t inherited, size_t *need)
{
    for (uint8_t i = 0; i < shape->part_count; i++) {
        shape_sprite_t *sp = &sprites[i];
        int32_t         w, h;

        gauge_shape_max_size(shape, i, &w, &h);
        size_t bytes = (size_t)w * (size_t)h;

        sp->cov = heap_caps_malloc(bytes > 0 ? bytes : 1, MALLOC_CAP_SPIRAM);
        if (sp->cov == NULL) {
            return false;
        }

        memset(&sp->img, 0, sizeof(sp->img));
        sp->img.header.magic = LV_IMAGE_HEADER_MAGIC;
        sp->img.header.cf    = LV_COLOR_FORMAT_A8;
        sp->img.data         = sp->cov;
        sp->color            = to_lv(gauge_shape_part_color(shape, i, inherited,
                                                            &sp->follows_alert));

        if (bytes > *need) {
            *need = bytes;
        }
    }
    return true;
}

static void free_sprites(shape_sprite_t *sprites)
{
    for (int i = 0; i < GAUGE_SHAPE_MAX_PARTS; i++) {
        free(sprites[i].cov);
        sprites[i].cov = NULL;
    }
}

/* Rasterise one part into its sprite, with the shape's origin at (ox, oy) in parent space. */
static void raster_sprite(gauge_render_t *g, shape_sprite_t *sp, const gauge_shape_t *shape,
                          uint8_t part, float deg, float ox, float oy)
{
    gauge_shape_rect_t r;
    size_t             n = gauge_shape_outline(shape, part, deg, ox, oy, g->outline);

    gauge_shape_bounds(g->outline, n, &r);
    gauge_shape_rasterize(g->outline, n, &r, g->raster_acc, sp->cov);

    int32_t w = gauge_shape_rect_w(&r);
    int32_t h = gauge_shape_rect_h(&r);
    sp->img.header.w      = (uint32_t)w;
    sp->img.header.h      = (uint32_t)h;
    sp->img.header.stride = (uint32_t)w;
    sp->img.data_size     = (uint32_t)(w * h);
    sp->area              = (lv_area_t){.x1 = r.x1, .y1 = r.y1, .x2 = r.x2, .y2 = r.y2};
}

/* Draw sprites whose areas are parent-relative. @p alert recolours parts that follow it. */
static void draw_sprites(gauge_render_t *g, lv_layer_t *layer, const shape_sprite_t *sprites,
                         uint8_t count, const lv_color_t *alert)
{
    lv_area_t pa;
    lv_obj_get_coords(g->parent, &pa);

    for (uint8_t i = 0; i < count; i++) {
        const shape_sprite_t *sp = &sprites[i];

        lv_draw_image_dsc_t dsc;
        lv_draw_image_dsc_init(&dsc);
        dsc.src         = &sp->img;
        dsc.recolor     = (alert != NULL && sp->follows_alert) ? *alert : sp->color;
        dsc.recolor_opa = LV_OPA_COVER;

        lv_area_t a = sp->area;
        lv_area_move(&a, pa.x1, pa.y1);
        lv_draw_image(layer, &dsc, &a);
    }
}

/*
 * Get the custom needle and hub ready: buffers, colours, and the one-off hub rasterisation.
 * If PSRAM runs out, fall back to the built-in drawing -- a plainer needle beats no gauge.
 */
static void prepare_shapes(gauge_render_t *g)
{
    gauge_config_t *c    = &g->cfg;
    bool            ok   = true;
    size_t          need = 0;

    if (gauge_shape_is_set(&c->needle.shape)) {
        ok = init_sprites(g->needle_parts, &c->needle.shape, c->needle.color, &need);
        g->needle_extent = gauge_shape_extent(&c->needle.shape);
    }
    if (ok && gauge_shape_is_set(&c->needle.hub_shape)) {
        ok = init_sprites(g->hub_parts, &c->needle.hub_shape, c->needle.color, &need);
    }
    if (ok && need > 0) {
        g->raster_acc = heap_caps_malloc(need * sizeof(float), MALLOC_CAP_SPIRAM);
        ok            = (g->raster_acc != NULL);
    }

    if (!ok) {
        ESP_LOGW(TAG, "no PSRAM for the custom needle; using the built-in one");
        free_sprites(g->needle_parts);
        free_sprites(g->hub_parts);
        free(g->raster_acc);
        g->raster_acc                 = NULL;
        c->needle.shape.part_count     = 0;
        c->needle.hub_shape.part_count = 0;
        return;
    }

    for (uint8_t i = 0; i < c->needle.hub_shape.part_count; i++) {
        raster_sprite(g, &g->hub_parts[i], &c->needle.hub_shape, i, 0.0f, (float)g->cx,
                      (float)g->cy);
    }
}

/*
 * Blend a coverage mask straight into the RGB565 face buffer. The face is drawn once, so this
 * favours simplicity over speed. It is needed because LVGL has no seam-free polygon fill, and
 * because queuing images on the canvas layer would defer the draw past the reuse of the mask.
 */
static void blend_mask(lv_draw_buf_t *dst, const gauge_shape_rect_t *r, const uint8_t *cov,
                       lv_color_t color)
{
    int32_t  dw     = (int32_t)dst->header.w;
    int32_t  dh     = (int32_t)dst->header.h;
    size_t   stride = dst->header.stride;
    int32_t  mw     = gauge_shape_rect_w(r);
    uint16_t fg     = lv_color_to_u16(color);
    uint32_t fr = fg >> 11, fgr = (fg >> 5) & 0x3f, fb = fg & 0x1f;

    for (int32_t y = LV_MAX(r->y1, 0); y <= LV_MIN(r->y2, dh - 1); y++) {
        uint8_t       *row  = dst->data + (size_t)y * stride;
        const uint8_t *mask = cov + (size_t)(y - r->y1) * (size_t)mw;

        for (int32_t x = LV_MAX(r->x1, 0); x <= LV_MIN(r->x2, dw - 1); x++) {
            uint32_t a = mask[x - r->x1];
            if (a == 0) {
                continue;
            }

            uint16_t px;
            memcpy(&px, row + (size_t)x * 2, sizeof(px));
            if (a == 255) {
                px = fg;
            } else {
                uint32_t ia = 255 - a;
                uint32_t br = px >> 11, bgr = (px >> 5) & 0x3f, bb = px & 0x1f;
                px = (uint16_t)((((fr * a + br * ia + 127) / 255) << 11) |
                                (((fgr * a + bgr * ia + 127) / 255) << 5) |
                                ((fb * a + bb * ia + 127) / 255));
            }
            memcpy(row + (size_t)x * 2, &px, sizeof(px));
        }
    }
}

/* Ticks from a custom shape, each with its origin on the tick circle and pointing outwards. */
static void draw_shaped_ticks(gauge_render_t *g, const gauge_shape_t *shape, float step,
                              int32_t count, int32_t tick_outer)
{
    const gauge_config_t *c    = &g->cfg;
    size_t                need = 0;

    for (uint8_t i = 0; i < shape->part_count; i++) {
        int32_t w, h;
        gauge_shape_max_size(shape, i, &w, &h);
        if ((size_t)w * (size_t)h > need) {
            need = (size_t)w * (size_t)h;
        }
    }

    /* One mask and scratch pair, reused for every tick. Freed once the face is drawn. */
    float   *acc = heap_caps_malloc(need * sizeof(float), MALLOC_CAP_SPIRAM);
    uint8_t *cov = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (acc == NULL || cov == NULL) {
        ESP_LOGW(TAG, "no PSRAM to draw custom ticks; skipping them");
        free(acc);
        free(cov);
        return;
    }

    lv_draw_buf_t *dst = lv_canvas_get_draw_buf(g->face);

    for (int32_t t = 0; t <= count; t++) {
        float         v     = c->source.min + (float)t * step;
        float         deg   = value_to_deg(g, v);
        gauge_color_t color = gauge_config_tick_color(c, v);
        float rad = DEG2RAD(deg);
        float ox  = (float)g->cx + (float)tick_outer * sinf(rad);
        float oy  = (float)g->cy - (float)tick_outer * cosf(rad);

        for (uint8_t i = 0; i < shape->part_count; i++) {
            gauge_shape_rect_t r;
            size_t             n = gauge_shape_outline(shape, i, deg, ox, oy, g->outline);

            gauge_shape_bounds(g->outline, n, &r);
            gauge_shape_rasterize(g->outline, n, &r, acc, cov);
            blend_mask(dst, &r, cov, to_lv(gauge_shape_part_color(shape, i, color, NULL)));
        }
    }

    free(acc);
    free(cov);
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

    /*
     * Finish the bands before any ticks. Shaped ticks are blended straight into the canvas
     * buffer, so everything that must sit beneath them has to be on the canvas already.
     */
    lv_canvas_finish_layer(cv, &layer);

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

            const gauge_shape_t *shape = (pass == 0) ? &c->face.ticks.minor_shape
                                                     : &c->face.ticks.major_shape;
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

            if (gauge_shape_is_set(shape)) {
                draw_shaped_ticks(g, shape, step, count, tick_outer);
                continue;
            }

            lv_canvas_init_layer(cv, &layer);
            for (int32_t i = 0; i <= count; i++) {
                float v   = min + (float)i * step;
                float deg = value_to_deg(g, v);

                int32_t x1, y1, x2, y2;
                polar(cx, cy, (float)tick_outer, deg, &x1, &y1);
                polar(cx, cy, (float)(tick_outer - len), deg, &x2, &y2);

                lv_draw_line_dsc_t dsc;
                lv_draw_line_dsc_init(&dsc);
                dsc.color = to_lv(gauge_config_tick_color(c, v));
                dsc.width = width;
                dsc.opa   = LV_OPA_COVER;
                dsc.p1.x  = x1;
                dsc.p1.y  = y1;
                dsc.p2.x  = x2;
                dsc.p2.y  = y2;
                lv_draw_line(&layer, &dsc);
            }
            lv_canvas_finish_layer(cv, &layer);
        }
    }

    lv_canvas_init_layer(cv, &layer);

    /* --- numeric labels --- */
    if (c->face.labels.present) {
        float step = (c->face.labels.every > 0.0f) ? c->face.labels.every
                                                   : c->face.ticks.major_every;
        if (step > 0.0f) {
            const lv_font_t *font = font_by_name(c->face.labels.font);

            int32_t r = c->face.labels.radius_px;
            if (r <= 0) {
                /* A shaped major tick reaches as far inward as its largest y. */
                int32_t major_reach = gauge_shape_is_set(&c->face.ticks.major_shape)
                    ? (int32_t)ceilf(gauge_shape_max_y(&c->face.ticks.major_shape))
                    : c->face.ticks.major_len_px;
                r = tick_outer - major_reach - lv_font_get_line_height(font);
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

    lv_color_t color    = to_lv(g->cfg.needle.color);
    bool       flashing = g->alert_active && g->alert_phase && g->active_alert_idx >= 0;

    /* The needle flashes in the alert colour along with the readout. */
    if (flashing) {
        color = to_lv(g->cfg.alerts[g->active_alert_idx].color);
    }

    if (gauge_shape_is_set(&g->cfg.needle.shape)) {
        if (g->shape_valid) {
            draw_sprites(g, layer, g->needle_parts, g->cfg.needle.shape.part_count,
                         flashing ? &color : NULL);
        }
        return;
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

static void hub_draw_cb(lv_event_t *e)
{
    gauge_render_t *g     = lv_event_get_user_data(e);
    lv_layer_t     *layer = lv_event_get_layer(e);

    if (layer != NULL) {
        draw_sprites(g, layer, g->hub_parts, g->cfg.needle.hub_shape.part_count, NULL);
    }
}

/*
 * Custom-shape counterpart of update_needle(): rasterise every part at the new angle and
 * invalidate the union of their bounds, plus where they were before.
 */
static void update_needle_shape(gauge_render_t *g)
{
    const gauge_shape_t *shape = &g->cfg.needle.shape;
    float                deg   = value_to_deg(g, g->displayed);

    /*
     * Skip a change that moves the tip by under an eighth of a pixel. It is measured against
     * the angle last drawn, not the previous update, so a slow creep still adds up to a redraw.
     * This is the shaped needle's equivalent of the same-pixels check in update_needle().
     */
    if (g->shape_valid && fabsf(deg - g->shape_deg) * DEG2RAD(1.0f) * g->needle_extent < 0.125f) {
        return;
    }

    lv_area_t bbox = {0};
    for (uint8_t i = 0; i < shape->part_count; i++) {
        shape_sprite_t *sp = &g->needle_parts[i];
        raster_sprite(g, sp, shape, i, deg, (float)g->cx, (float)g->cy);

        if (i == 0) {
            bbox = sp->area;
        } else {
            bbox.x1 = LV_MIN(bbox.x1, sp->area.x1);
            bbox.y1 = LV_MIN(bbox.y1, sp->area.y1);
            bbox.x2 = LV_MAX(bbox.x2, sp->area.x2);
            bbox.y2 = LV_MAX(bbox.y2, sp->area.y2);
        }
    }
    g->shape_deg   = deg;
    g->shape_valid = true;

    /* Sprite areas are parent-relative; invalidation wants screen coordinates. */
    lv_area_t pa;
    lv_obj_get_coords(g->parent, &pa);
    lv_area_move(&bbox, pa.x1, pa.y1);

    if (g->prev_valid) {
        lv_obj_invalidate_area(g->needle, &g->prev_bbox);
    }
    lv_obj_invalidate_area(g->needle, &bbox);

    g->prev_bbox  = bbox;
    g->prev_valid = true;
}

/*
 * Recompute the needle polygon for the current angle and resize the object to its exact
 * bounding box. Keeping the object tight is what keeps the dirty region small: LVGL
 * invalidates the old and new areas on a coordinate change, and nothing more.
 */
static void update_needle(gauge_render_t *g)
{
    const gauge_config_t *c = &g->cfg;

    if (gauge_shape_is_set(&c->needle.shape)) {
        update_needle_shape(g);
        return;
    }

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

/* ------------------------------------------------------------------ numbers -------- */

/*
 * Format a displayed number: in the user's format if one is set, otherwise the element's own, and
 * rounded to the user's step first (EGT to the nearest 10 degrees).
 */
static void format_number(const gauge_render_t *g, const char *face_fmt, float v, char *out,
                          size_t len)
{
    if (g->round_step > 0.0f) {
        v = roundf(v / g->round_step) * g->round_step;
    }
    const char *fmt = (g->num_format[0] != '\0') ? g->num_format : face_fmt;
    snprintf(out, len, fmt, (double)v);
    /* "-0" reads as a fault at a glance; a value that rounds to zero is zero. */
    if (strcmp(out, "-0") == 0 || strcmp(out, "-0.0") == 0 || strcmp(out, "-0.00") == 0) {
        memmove(out, out + 1, strlen(out));
    }
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
        format_number(g, g->cfg.peak.format, g->peak_value, num, sizeof(num));
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
static void place_peak_marker(gauge_render_t *g);

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
    place_peak_marker(g);
}

/* Move the marker to peak_value, repainting only if it lands on different pixels. */
static void place_peak_marker(gauge_render_t *g)
{
    float value = g->peak_value;

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

/* ------------------------------------------------------------------ min/max --------- */

static void format_extreme(const gauge_render_t *g, char *out, size_t len, const char *tag,
                           float value)
{
    const gauge_config_t *c = &g->cfg;
    char                  num[32];

    if (g->minmax_valid) {
        format_number(g, c->readout.format, value, num, sizeof(num));
    } else {
        snprintf(num, sizeof(num), "---");
    }
    snprintf(out, len, "%s %s%s", tag, num, g->minmax_valid ? c->readout.suffix : "");
}

static void set_recall_label(gauge_render_t *g)
{
    if (g->recall_label == NULL || g->recall_timer == NULL) {
        return;
    }

    char hi[96], lo[96], text[200];
    format_extreme(g, hi, sizeof(hi), "MAX", g->max_value);
    format_extreme(g, lo, sizeof(lo), "MIN", g->min_value);
    snprintf(text, sizeof(text), "%s\n%s", hi, lo);

    if (strcmp(lv_label_get_text(g->recall_label), text) != 0) {
        lv_label_set_text(g->recall_label, text);
    }
}

/*
 * Tracks the raw value, before clamping or damping. Unlike the peak marker, which has to sit
 * on the scale, the recalled numbers can say what the needle could not: 34 psi on a 30 psi dial.
 */
static void update_minmax(gauge_render_t *g, float value, bool valid)
{
    if (!valid || !isfinite(value)) {
        return;
    }
    if (!g->minmax_valid) {
        g->min_value    = value;
        g->max_value    = value;
        g->minmax_valid = true;
    } else if (value < g->min_value) {
        g->min_value = value;
    } else if (value > g->max_value) {
        g->max_value = value;
    } else {
        return;
    }
    set_recall_label(g);
}

static void set_readouts_hidden(gauge_render_t *g, bool hidden)
{
    lv_obj_t *objs[] = {g->readout, g->peak_label};
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (objs[i] == NULL) {
            continue;
        }
        if (hidden || (objs[i] == g->peak_label && g->peak_value_hidden)) {
            lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void end_recall(gauge_render_t *g)
{
    if (g->recall_timer != NULL) {
        lv_timer_delete(g->recall_timer);
        g->recall_timer = NULL;
    }
    if (g->recall_label != NULL) {
        lv_obj_add_flag(g->recall_label, LV_OBJ_FLAG_HIDDEN);
    }
    set_readouts_hidden(g, false);
}

static void recall_timer_cb(lv_timer_t *t)
{
    gauge_render_t *g = lv_timer_get_user_data(t);
    g->recall_timer   = NULL; /* one-shot: LVGL deletes it after this callback */
    end_recall(g);
}

void gauge_render_recall_minmax(gauge_render_t *g, uint32_t hold_ms)
{
    if (g == NULL || g->recall_label == NULL) {
        return;
    }

    if (g->recall_timer != NULL) {
        lv_timer_reset(g->recall_timer);
    } else {
        g->recall_timer = lv_timer_create(recall_timer_cb, hold_ms, g);
        if (g->recall_timer == NULL) {
            return;
        }
        lv_timer_set_repeat_count(g->recall_timer, 1);
    }

    set_recall_label(g);
    set_readouts_hidden(g, true);
    lv_obj_remove_flag(g->recall_label, LV_OBJ_FLAG_HIDDEN);
}

bool gauge_render_minmax_recalled(const gauge_render_t *g)
{
    return g != NULL && g->recall_timer != NULL;
}

void gauge_render_reset_minmax(gauge_render_t *g)
{
    if (g == NULL) {
        return;
    }
    g->minmax_valid = false;
    set_recall_label(g);
}

/* -------------------------------------------------------------------- trend -------- */

static lv_color_t mix_black(gauge_color_t c, uint8_t pct)
{
    return lv_color_make((uint8_t)(c.r * pct / 100), (uint8_t)(c.g * pct / 100),
                         (uint8_t)(c.b * pct / 100));
}

/* The strip's y for a value, as a float so the trace keeps sub-pixel position. */
static float trend_y(const gauge_render_t *g, float v)
{
    float lw  = (float)g->cfg.trend.line_width_px;
    float top = lw * 0.5f;
    float bot = (float)g->trend_h - 1.0f - lw * 0.5f;
    float t   = (v - g->cfg.source.min) / (g->cfg.source.max - g->cfg.source.min);
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    return bot - t * (bot - top);
}

/* Cover rows [y0, y1) of column x, with fractional coverage at both ends. */
static void cov_span(uint8_t *cov, int32_t w, int32_t h, int32_t x, float y0, float y1)
{
    if (y0 < 0.0f) {
        y0 = 0.0f;
    }
    if (y1 > (float)h) {
        y1 = (float)h;
    }
    for (int32_t y = (int32_t)floorf(y0); y < h && (float)y < y1; y++) {
        float a = fmaxf(y0, (float)y);
        float b = fminf(y1, (float)(y + 1));
        if (b <= a) {
            continue;
        }
        uint32_t v = cov[y * w + x] + (uint32_t)lroundf((b - a) * 255.0f);
        cov[y * w + x] = (uint8_t)(v > 255 ? 255 : v);
    }
}

/*
 * Rebuild both masks from the ring. Newest column at the right edge. ~10 KB of writes per mask,
 * at most a few times a second: far cheaper than drawing 200 primitives per repaint.
 */
static void trend_rasterise(gauge_render_t *g)
{
    int32_t w  = g->trend_w;
    int32_t h  = g->trend_h;
    float   hw = (float)g->cfg.trend.line_width_px * 0.5f;

    memset(g->trend_fill_cov, 0, (size_t)w * h);
    memset(g->trend_line_cov, 0, (size_t)w * h);
    g->trend_head_valid = false;

    bool  have_prev = false;
    float prev_y    = 0.0f;
    for (uint16_t i = 0; i < g->trend_count; i++) {
        uint16_t slot = (uint16_t)((g->trend_next + w - g->trend_count + i) % w);
        int32_t  x    = w - g->trend_count + i;
        float    v    = g->trend_cols[slot];
        if (isnan(v)) {
            have_prev = false;
            continue;
        }
        float y = trend_y(g, v);
        cov_span(g->trend_fill_cov, w, h, x, y, (float)h);
        float lo = have_prev ? fminf(prev_y, y) : y;
        float hi = have_prev ? fmaxf(prev_y, y) : y;
        cov_span(g->trend_line_cov, w, h, x, lo - hw, hi + hw);
        prev_y    = y;
        have_prev = true;

        g->trend_head       = (lv_point_t){.x = x, .y = (int32_t)lroundf(y)};
        g->trend_head_valid = true;
    }
    if (!have_prev) {
        g->trend_head_valid = false; /* the newest column is a gap: no dot */
    }
}

static void trend_draw_cb(lv_event_t *e)
{
    gauge_render_t *g     = lv_event_get_user_data(e);
    lv_layer_t     *layer = lv_event_get_layer(e);
    const gauge_config_t *c = &g->cfg;
    if (layer == NULL) {
        return;
    }

    lv_area_t pa;
    lv_obj_get_coords(g->parent, &pa);
    lv_area_t a = g->trend_area;
    lv_area_move(&a, pa.x1, pa.y1);

    /* Baseline, so an empty strip still reads as a chart. */
    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = mix_black(c->trend.color, 30);
    ln.width = 2;
    ln.p1    = (lv_point_precise_t){.x = a.x1, .y = a.y2};
    ln.p2    = (lv_point_precise_t){.x = a.x2, .y = a.y2};
    lv_draw_line(layer, &ln);

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src         = &g->trend_fill_img;
    img.recolor     = g->trend_fill_color;
    img.recolor_opa = LV_OPA_COVER;
    lv_draw_image(layer, &img, &a);

    /* The alert thresholds, dashed, over the fill and under the trace. */
    if (c->trend.alert_marks) {
        for (uint8_t i = 0; i < c->alert_count; i++) {
            const gauge_alert_t *al = &c->alerts[i];
            if (!al->has_above || al->above <= c->source.min || al->above >= c->source.max) {
                continue;
            }
            int32_t y = a.y1 + (int32_t)lroundf(trend_y(g, al->above));
            lv_draw_line_dsc_init(&ln);
            ln.color      = to_lv(al->color);
            ln.width      = 2;
            ln.dash_width = 6;
            ln.dash_gap   = 6;
            ln.p1         = (lv_point_precise_t){.x = a.x1, .y = y};
            ln.p2         = (lv_point_precise_t){.x = a.x2, .y = y};
            lv_draw_line(layer, &ln);
        }
    }

    img.src     = &g->trend_line_img;
    img.recolor = to_lv(c->trend.color);
    lv_draw_image(layer, &img, &a);

    /* The newest reading, as a dot the eye can find at a glance. */
    if (g->trend_head_valid) {
        int32_t r = c->trend.line_width_px + 2;
        lv_area_t d = {
            .x1 = a.x1 + g->trend_head.x - r, .y1 = a.y1 + g->trend_head.y - r,
            .x2 = a.x1 + g->trend_head.x + r, .y2 = a.y1 + g->trend_head.y + r,
        };
        lv_draw_rect_dsc_t rd;
        lv_draw_rect_dsc_init(&rd);
        rd.bg_color = c->readout.present ? to_lv(c->readout.color) : lv_color_white();
        rd.bg_opa   = LV_OPA_COVER;
        rd.radius   = LV_RADIUS_CIRCLE;
        lv_draw_rect(layer, &rd, &d);
    }
}

static void trend_reset(gauge_render_t *g)
{
    g->trend_count     = 0;
    g->trend_next      = 0;
    g->trend_sum       = 0.0f;
    g->trend_n         = 0;
    g->trend_col_start = lv_tick_get();
    trend_rasterise(g);
    lv_obj_invalidate(g->trend);
}

/* Add a sample to the current column; close the column, and repaint, once its time is up. */
static void trend_sample(gauge_render_t *g, float value, bool valid)
{
    if (g->trend == NULL) {
        return;
    }
    if (valid) {
        g->trend_sum += value;
        g->trend_n++;
    }

    uint32_t elapsed = lv_tick_elaps(g->trend_col_start);
    if (elapsed < g->trend_col_ms) {
        return;
    }
    /* Keep to the column grid, but don't try to catch up after a long stall. */
    g->trend_col_start = (elapsed < 2 * g->trend_col_ms) ? g->trend_col_start + g->trend_col_ms
                                                         : lv_tick_get();

    g->trend_cols[g->trend_next] = (g->trend_n > 0) ? g->trend_sum / (float)g->trend_n : NAN;
    g->trend_next = (uint16_t)((g->trend_next + 1) % g->trend_w);
    if (g->trend_count < g->trend_w) {
        g->trend_count++;
    }
    g->trend_sum = 0.0f;
    g->trend_n   = 0;

    trend_rasterise(g);
    lv_obj_invalidate(g->trend);
}

static void trend_init_img(lv_image_dsc_t *img, uint8_t *cov, int32_t w, int32_t h)
{
    memset(img, 0, sizeof(*img));
    img->header.magic  = LV_IMAGE_HEADER_MAGIC;
    img->header.cf     = LV_COLOR_FORMAT_A8;
    img->header.w      = (uint32_t)w;
    img->header.h      = (uint32_t)h;
    img->header.stride = (uint32_t)w;
    img->data_size     = (uint32_t)(w * h);
    img->data          = cov;
}

/* Builds the strip. A PSRAM shortfall leaves the gauge without it rather than failing the gauge. */
static void trend_create(gauge_render_t *g, lv_obj_t *parent)
{
    const gauge_config_t *c = &g->cfg;
    int32_t w = c->trend.width_px;
    int32_t h = c->trend.height_px;
    if (w > g->board->width_px) {
        w = g->board->width_px;
    }

    g->trend_cols     = heap_caps_malloc(sizeof(float) * (size_t)w, MALLOC_CAP_SPIRAM);
    g->trend_fill_cov = heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
    g->trend_line_cov = heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
    if (g->trend_cols == NULL || g->trend_fill_cov == NULL || g->trend_line_cov == NULL) {
        ESP_LOGW(TAG, "no PSRAM for the trend strip; leaving it out");
        return;
    }

    g->trend_w = w;
    g->trend_h = h;
    int32_t x  = (c->trend.x == INT16_MIN) ? g->cx - w / 2 : c->trend.x;
    /* Auto: in the gap under a 270-degree dial, 0.6 of the way from centre to the panel edge. */
    int32_t y  = (c->trend.y == INT16_MIN) ? g->cy + (int32_t)lroundf(0.6f * (float)g->cy)
                                           : c->trend.y;
    g->trend_area = (lv_area_t){.x1 = x, .y1 = y, .x2 = x + w - 1, .y2 = y + h - 1};

    g->trend_fill_color = c->trend.has_fill ? to_lv(c->trend.fill) : mix_black(c->trend.color, 30);
    trend_init_img(&g->trend_fill_img, g->trend_fill_cov, w, h);
    trend_init_img(&g->trend_line_img, g->trend_line_cov, w, h);

    g->trend = lv_obj_create(parent);
    if (g->trend == NULL) {
        return;
    }
    lv_obj_remove_style_all(g->trend);
    lv_obj_remove_flag(g->trend, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(g->trend, LV_OBJ_FLAG_CLICKABLE);
    /* Room for the dot, which may sit on the strip's edge. */
    int32_t slack = c->trend.line_width_px + 4;
    lv_obj_set_size(g->trend, w + 2 * slack, h + 2 * slack);
    lv_obj_set_pos(g->trend, x - slack, y - slack);
    lv_obj_add_event_cb(g->trend, trend_draw_cb, LV_EVENT_DRAW_MAIN, g);

    gauge_render_set_trend_window(g, 60);
}

void gauge_render_set_trend_window(gauge_render_t *g, uint32_t seconds)
{
    if (g == NULL || g->trend == NULL || seconds == 0) {
        return;
    }
    uint32_t ms = seconds * 1000u / (uint32_t)g->trend_w;
    if (ms == g->trend_col_ms && g->trend_count > 0) {
        return;
    }
    g->trend_col_ms = ms > 0 ? ms : 1;
    trend_reset(g);
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
    format_number(g, c->readout.format, g->displayed, num, sizeof(num));

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

    /*
     * PSRAM, explicitly. With its embedded config this struct is a few KB, and plain calloc()
     * places anything under 4KB in internal RAM, which WiFi needs (docs/performance.md).
     */
    gauge_render_t *g = heap_caps_calloc(1, sizeof(gauge_render_t), MALLOC_CAP_SPIRAM);
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

    /* May clear the custom needle and hub from g->cfg if PSRAM runs short; read g->cfg below. */
    prepare_shapes(g);

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

    /* --- sparkline strip: over the face, under the needle --- */
    if (cfg->trend.present) {
        trend_create(g, parent);
    }

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
        int32_t reach = gauge_shape_is_set(&g->cfg.needle.shape)
            ? (int32_t)ceilf(g->needle_extent) + 8
            : g->needle_len + cfg->needle.tail_px + cfg->needle.width_px + 8;
        lv_obj_set_size(g->needle, reach * 2, reach * 2);
        lv_obj_set_pos(g->needle, g->cx - reach, g->cy - reach);
    }

    /* --- centre hub, above the needle --- */
    if (gauge_shape_is_set(&g->cfg.needle.hub_shape)) {
        g->hub = lv_obj_create(parent);
        if (g->hub != NULL) {
            lv_obj_remove_style_all(g->hub);
            lv_obj_remove_flag(g->hub, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(g->hub, LV_OBJ_FLAG_CLICKABLE);

            /* Static: cover the union of the parts, rasterised once in prepare_shapes(). */
            lv_area_t box = g->hub_parts[0].area;
            for (uint8_t i = 1; i < g->cfg.needle.hub_shape.part_count; i++) {
                box.x1 = LV_MIN(box.x1, g->hub_parts[i].area.x1);
                box.y1 = LV_MIN(box.y1, g->hub_parts[i].area.y1);
                box.x2 = LV_MAX(box.x2, g->hub_parts[i].area.x2);
                box.y2 = LV_MAX(box.y2, g->hub_parts[i].area.y2);
            }
            lv_obj_set_pos(g->hub, box.x1, box.y1);
            lv_obj_set_size(g->hub, lv_area_get_width(&box), lv_area_get_height(&box));
            lv_obj_add_event_cb(g->hub, hub_draw_cb, LV_EVENT_DRAW_MAIN, g);
        }
    } else if (cfg->needle.pivot_radius_px > 0) {
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

    /*
     * Min/max recall: hidden until asked for, then shown in place of the live and peak
     * readouts. A fixed mid-size font, because two lines of a 48px readout font would not fit
     * the narrowing lower half of a round panel.
     */
    g->recall_label = lv_label_create(parent);
    ESP_GOTO_ON_FALSE(g->recall_label != NULL, ESP_ERR_NO_MEM, fail, TAG, "recall label failed");
    lv_obj_set_style_text_font(g->recall_label, font_by_name("montserrat_26"), LV_PART_MAIN);
    lv_obj_set_style_text_color(g->recall_label, to_lv(cfg->readout.color), LV_PART_MAIN);
    lv_obj_set_style_text_align(g->recall_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(g->recall_label, LV_SIZE_CONTENT);
    lv_label_set_text(g->recall_label, "");
    lv_obj_add_flag(g->recall_label, LV_OBJ_FLAG_HIDDEN);
    {
        int32_t rx = (cfg->readout.present && cfg->readout.x != INT16_MIN) ? cfg->readout.x : g->cx;
        int32_t ry = cfg->readout.present ? cfg->readout.y : g->cy + 60;
        lv_obj_align(g->recall_label, LV_ALIGN_TOP_MID, rx - w / 2, ry);
    }

    g->valid     = true;
    g->displayed = cfg->source.min;
    update_readout(g);
    update_needle(g);
    set_peak_label(g);

    ESP_LOGI(TAG, "gauge '%s' ready: r=%" PRId32 " needle_len=%" PRId32 " face=%uKB%s",
             cfg->id, g->radius, g->needle_len, (unsigned)(face_size / 1024),
             gauge_shape_is_set(&g->cfg.needle.shape) ? " (custom needle)" : "");

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
    if (g->recall_timer != NULL) {
        lv_timer_delete(g->recall_timer);
    }

    /* Delete the objects before the buffers they point into. */
    if (g->readout != NULL) {
        lv_obj_delete(g->readout);
    }
    if (g->peak_label != NULL) {
        lv_obj_delete(g->peak_label);
    }
    if (g->recall_label != NULL) {
        lv_obj_delete(g->recall_label);
    }
    if (g->peak_marker != NULL) {
        lv_obj_delete(g->peak_marker);
    }
    if (g->trend != NULL) {
        lv_obj_delete(g->trend);
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

    free_sprites(g->needle_parts);
    free_sprites(g->hub_parts);
    free(g->raster_acc);
    free(g->trend_cols);
    free(g->trend_fill_cov);
    free(g->trend_line_cov);
    free(g->face_buf);
    free(g);
}

void gauge_render_set_value(gauge_render_t *g, float value, bool valid)
{
    if (g == NULL) {
        return;
    }

    /*
     * NaN passes both clamps below, and once it is in the damping filter nothing brings it back
     * out: every later update is NaN too. A non-finite reading is no reading.
     */
    if (!isfinite(value)) {
        valid = false;
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

    update_minmax(g, value, valid);
    update_peak(g, target, valid);
    trend_sample(g, target, valid);
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

void gauge_render_set_number_format(gauge_render_t *g, int decimals, float round_step)
{
    if (g == NULL) {
        return;
    }
    if (decimals >= 0 && decimals <= 3) {
        snprintf(g->num_format, sizeof(g->num_format), "%%.%df", decimals);
    } else {
        g->num_format[0] = '\0';
    }
    g->round_step = (round_step > 0.0f) ? round_step : 0.0f;

    update_readout(g);
    set_peak_label(g);
    set_recall_label(g);
}

void gauge_render_show_peak_value(gauge_render_t *g, bool show)
{
    if (g == NULL) {
        return;
    }
    g->peak_value_hidden = !show;
    if (g->peak_label == NULL) {
        return;
    }
    if (show && !gauge_render_minmax_recalled(g)) {
        lv_obj_remove_flag(g->peak_label, LV_OBJ_FLAG_HIDDEN);
    } else if (!show) {
        lv_obj_add_flag(g->peak_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void gauge_render_parent_moved(gauge_render_t *g)
{
    if (g == NULL) {
        return;
    }
    /* The needle and peak marker cache screen coordinates; recompute them where they now are. */
    g->prev_valid  = false;
    g->shape_valid = false;
    update_needle(g);
    if (g->peak_marker != NULL && g->peak_valid) {
        g->peak_bbox_valid = false;
        place_peak_marker(g);
    }
}
