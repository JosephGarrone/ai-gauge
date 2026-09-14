/*
 * Custom shape geometry and rasterisation. See gauge_shape.h.
 *
 * The rasteriser is the signed-area accumulation technique used by font renderers (after
 * font-rs): each edge deposits the area it sweeps into an accumulation buffer, and a running sum
 * along each row turns that into exact coverage. Cost is proportional to the mask area plus the
 * edge length, with no per-pixel inside/outside tests and no seams between sub-regions.
 */

#include "gauge_shape.h"

#include <math.h>
#include <string.h>

#define DEG2RAD(d) ((float)(d) * 3.14159265358979f / 180.0f)

static float units_to_px(int32_t v)
{
    return (float)v / (float)GAUGE_SHAPE_COORD_SCALE;
}

/*
 * Segments for a flattened circle: enough that the chord sits within ~0.1px of the true arc for
 * hub-sized circles, capped so a huge one still fits an outline.
 */
static int circle_segments(float r_px)
{
    int n = (int)ceilf(r_px * 2.5f);
    if (n < 16) {
        n = 16;
    }
    if (n > GAUGE_SHAPE_MAX_OUTLINE) {
        n = GAUGE_SHAPE_MAX_OUTLINE;
    }
    return n;
}

float gauge_shape_extent(const gauge_shape_t *shape)
{
    float ext = 0.0f;

    for (uint8_t i = 0; shape != NULL && i < shape->part_count; i++) {
        const gauge_shape_part_t *p = &shape->parts[i];

        if (p->kind == GAUGE_SHAPE_PART_CIRCLE) {
            float d = hypotf(units_to_px(p->center.x), units_to_px(p->center.y)) +
                      units_to_px(p->radius);
            ext = fmaxf(ext, d);
        } else {
            for (uint8_t k = 0; k < p->count; k++) {
                const gauge_point_t *pt = &shape->points[p->first + k];
                ext = fmaxf(ext, hypotf(units_to_px(pt->x), units_to_px(pt->y)));
            }
        }
    }

    return ext;
}

float gauge_shape_max_y(const gauge_shape_t *shape)
{
    float max_y = 0.0f;
    bool  any   = false;

    for (uint8_t i = 0; shape != NULL && i < shape->part_count; i++) {
        const gauge_shape_part_t *p = &shape->parts[i];

        if (p->kind == GAUGE_SHAPE_PART_CIRCLE) {
            float y = units_to_px(p->center.y) + units_to_px(p->radius);
            max_y   = any ? fmaxf(max_y, y) : y;
            any     = true;
        } else {
            for (uint8_t k = 0; k < p->count; k++) {
                float y = units_to_px(shape->points[p->first + k].y);
                max_y   = any ? fmaxf(max_y, y) : y;
                any     = true;
            }
        }
    }

    return max_y;
}

size_t gauge_shape_outline(const gauge_shape_t *shape, uint8_t part, float deg, float ox,
                           float oy, gauge_shape_vec_t *out)
{
    if (shape == NULL || out == NULL || part >= shape->part_count) {
        return 0;
    }

    const gauge_shape_part_t *p = &shape->parts[part];

    /* Clockwise rotation with y pointing down: (0,-1), 12 o'clock, goes to (1,0) at 90 degrees. */
    float c = cosf(DEG2RAD(deg));
    float s = sinf(DEG2RAD(deg));

    if (p->kind == GAUGE_SHAPE_PART_CIRCLE) {
        float cx = units_to_px(p->center.x);
        float cy = units_to_px(p->center.y);
        float r  = units_to_px(p->radius);
        float px = ox + cx * c - cy * s;
        float py = oy + cx * s + cy * c;
        int   n  = circle_segments(r);

        for (int i = 0; i < n; i++) {
            float t = 2.0f * 3.14159265358979f * (float)i / (float)n;
            out[i].x = px + r * sinf(t);
            out[i].y = py - r * cosf(t);
        }
        return (size_t)n;
    }

    size_t n = p->count;
    if (n > GAUGE_SHAPE_MAX_OUTLINE) {
        n = GAUGE_SHAPE_MAX_OUTLINE;
    }
    for (size_t i = 0; i < n; i++) {
        const gauge_point_t *pt = &shape->points[p->first + i];
        float x = units_to_px(pt->x);
        float y = units_to_px(pt->y);
        out[i].x = ox + x * c - y * s;
        out[i].y = oy + x * s + y * c;
    }
    return n;
}

void gauge_shape_bounds(const gauge_shape_vec_t *v, size_t n, gauge_shape_rect_t *out)
{
    if (n == 0) {
        *out = (gauge_shape_rect_t){0, 0, 0, 0};
        return;
    }

    float minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (size_t i = 1; i < n; i++) {
        minx = fminf(minx, v[i].x);
        maxx = fmaxf(maxx, v[i].x);
        miny = fminf(miny, v[i].y);
        maxy = fmaxf(maxy, v[i].y);
    }

    out->x1 = (int32_t)floorf(minx) - 1;
    out->y1 = (int32_t)floorf(miny) - 1;
    out->x2 = (int32_t)ceilf(maxx) + 1;
    out->y2 = (int32_t)ceilf(maxy) + 1;
}

void gauge_shape_max_size(const gauge_shape_t *shape, uint8_t part, int32_t *w, int32_t *h)
{
    /*
     * Works on the part directly rather than through gauge_shape_outline(), so there is no
     * 512-byte vertex array on the stack: a face can be rebuilt from the HTTP server task, whose
     * internal-RAM stack has little to spare (docs/performance.md).
     */
    *w = 0;
    *h = 0;
    if (shape == NULL || part >= shape->part_count) {
        return;
    }

    const gauge_shape_part_t *p     = &shape->parts[part];
    int32_t                   max_w = 0, max_h = 0;
    float                     reach = 0.0f;

    for (int deg = 0; deg < 360; deg++) {
        float c = cosf(DEG2RAD(deg));
        float s = sinf(DEG2RAD(deg));
        float minx, maxx, miny, maxy;

        if (p->kind == GAUGE_SHAPE_PART_CIRCLE) {
            /* The flattened circle lies inside the true one, so its box is a safe bound. */
            float cx = units_to_px(p->center.x);
            float cy = units_to_px(p->center.y);
            float r  = units_to_px(p->radius);
            float px = cx * c - cy * s;
            float py = cx * s + cy * c;
            minx     = px - r;
            maxx     = px + r;
            miny     = py - r;
            maxy     = py + r;
            reach    = hypotf(cx, cy) + r;
        } else {
            if (p->count == 0) {
                return;
            }
            for (uint8_t k = 0; k < p->count; k++) {
                const gauge_point_t *pt = &shape->points[p->first + k];
                float x  = units_to_px(pt->x);
                float y  = units_to_px(pt->y);
                float rx = x * c - y * s;
                float ry = x * s + y * c;
                if (k == 0) {
                    minx = maxx = rx;
                    miny = maxy = ry;
                } else {
                    minx = fminf(minx, rx);
                    maxx = fmaxf(maxx, rx);
                    miny = fminf(miny, ry);
                    maxy = fmaxf(maxy, ry);
                }
                reach = fmaxf(reach, hypotf(x, y));
            }
        }

        /* Same arithmetic as gauge_shape_bounds(): floor/ceil plus a pixel of margin each side. */
        int32_t bw = (int32_t)ceilf(maxx) - (int32_t)floorf(minx) + 3;
        int32_t bh = (int32_t)ceilf(maxy) - (int32_t)floorf(miny) + 3;
        max_w = (bw > max_w) ? bw : max_w;
        max_h = (bh > max_h) ? bh : max_h;
    }

    /*
     * Between whole-degree samples a vertex moves at most reach * sin(0.5 deg), and a sub-pixel
     * origin can push floor/ceil out by one more pixel.
     */
    int32_t slack = (int32_t)ceilf(reach * 0.00873f) * 2 + 1;
    *w = max_w + slack;
    *h = max_h + slack;
}

/* Deposit into a row, folding anything left of the mask into column 0 so the row sum is kept. */
static inline void deposit(float *row, int32_t w, int32_t x, float v)
{
    if (x < 0) {
        x = 0;
    }
    if (x < w) {
        row[x] += v;
    }
}

static void accumulate_edge(float *acc, int32_t w, int32_t h, float x0, float y0, float x1,
                            float y1)
{
    if (fabsf(y1 - y0) < 1e-6f) {
        return; /* horizontal edges sweep no area */
    }

    float dir = 1.0f;
    if (y0 > y1) {
        float t = x0;
        x0      = x1;
        x1      = t;
        t       = y0;
        y0      = y1;
        y1      = t;
        dir     = -1.0f;
    }

    float dxdy = (x1 - x0) / (y1 - y0);
    float x    = x0;
    if (y0 < 0.0f) {
        x -= y0 * dxdy; /* where the edge enters the top of the mask */
    }

    int32_t ystart = (y0 < 0.0f) ? 0 : (int32_t)y0;
    int32_t yend   = (int32_t)ceilf(y1);
    if (yend > h) {
        yend = h;
    }

    for (int32_t y = ystart; y < yend; y++) {
        float *row   = acc + (size_t)y * (size_t)w;
        float  dy    = fminf((float)(y + 1), y1) - fmaxf((float)y, y0);
        float  xnext = x + dxdy * dy;
        float  d     = dy * dir;

        float xa = fminf(x, xnext);
        float xb = fmaxf(x, xnext);

        float   xa_floor = floorf(xa);
        int32_t xai      = (int32_t)xa_floor;
        float   xb_ceil  = ceilf(xb);
        int32_t xbi      = (int32_t)xb_ceil;

        if (xbi <= xai + 1) {
            /* The edge stays within one pixel column on this row. */
            float xmf = 0.5f * (x + xnext) - xa_floor;
            deposit(row, w, xai, d - d * xmf);
            deposit(row, w, xai + 1, d * xmf);
        } else {
            float s   = 1.0f / (xb - xa);
            float xaf = xa - xa_floor;
            float a0  = 0.5f * s * (1.0f - xaf) * (1.0f - xaf);
            float xbf = xb - xb_ceil + 1.0f;
            float am  = 0.5f * s * xbf * xbf;

            deposit(row, w, xai, d * a0);
            if (xbi == xai + 2) {
                deposit(row, w, xai + 1, d * (1.0f - a0 - am));
            } else {
                float a1 = s * (1.5f - xaf);
                deposit(row, w, xai + 1, d * (a1 - a0));
                for (int32_t xi = xai + 2; xi < xbi - 1; xi++) {
                    deposit(row, w, xi, d * s);
                }
                float a2 = a1 + (float)(xbi - xai - 3) * s;
                deposit(row, w, xbi - 1, d * (1.0f - a2 - am));
            }
            deposit(row, w, xbi, d * am);
        }

        x = xnext;
    }
}

void gauge_shape_rasterize(const gauge_shape_vec_t *v, size_t n, const gauge_shape_rect_t *rect,
                           float *acc, uint8_t *cov)
{
    int32_t w = gauge_shape_rect_w(rect);
    int32_t h = gauge_shape_rect_h(rect);
    if (w <= 0 || h <= 0) {
        return;
    }

    memset(acc, 0, (size_t)w * (size_t)h * sizeof(float));

    float ox = (float)rect->x1;
    float oy = (float)rect->y1;
    for (size_t i = 0; i < n; i++) {
        const gauge_shape_vec_t *a = &v[i];
        const gauge_shape_vec_t *b = &v[(i + 1) % n];
        accumulate_edge(acc, w, h, a->x - ox, a->y - oy, b->x - ox, b->y - oy);
    }

    for (int32_t y = 0; y < h; y++) {
        const float *row = acc + (size_t)y * (size_t)w;
        uint8_t     *out = cov + (size_t)y * (size_t)w;
        float        sum = 0.0f;

        for (int32_t x = 0; x < w; x++) {
            sum += row[x];
            float a = fabsf(sum);
            out[x] = (a >= 1.0f) ? 255 : (uint8_t)(a * 255.0f + 0.5f);
        }
    }
}

gauge_color_t gauge_shape_part_color(const gauge_shape_t *shape, uint8_t part,
                                     gauge_color_t inherited, bool *inherits)
{
    bool          inh = false;
    gauge_color_t c   = inherited;

    if (shape != NULL && part < shape->part_count && shape->parts[part].has_color) {
        c = shape->parts[part].color;
    } else if (shape != NULL && shape->has_color) {
        c = shape->color;
    } else {
        inh = true;
    }

    if (inherits != NULL) {
        *inherits = inh;
    }
    return c;
}
