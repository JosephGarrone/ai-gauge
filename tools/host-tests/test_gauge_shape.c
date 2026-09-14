/*
 * Host tests for the custom-shape rasteriser.
 *
 * The properties that matter on the dial: coverage is exact (so edges are smooth and sizes are
 * true), there are no seams inside a shape, rotation follows the schema's angle convention, and
 * the worst-case buffer size really is worst-case -- an undersized buffer would corrupt PSRAM.
 */

#include "gauge_shape.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

#define BUF_DIM 512

static float   s_acc[BUF_DIM * BUF_DIM];
static uint8_t s_cov[BUF_DIM * BUF_DIM];

static gauge_shape_t polygon(const float *xy, int n)
{
    gauge_shape_t s;
    memset(&s, 0, sizeof(s));
    s.part_count     = 1;
    s.point_count    = (uint8_t)n;
    s.parts[0].kind  = GAUGE_SHAPE_PART_POLYGON;
    s.parts[0].first = 0;
    s.parts[0].count = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        s.points[i].x = (int16_t)lroundf(xy[2 * i] * GAUGE_SHAPE_COORD_SCALE);
        s.points[i].y = (int16_t)lroundf(xy[2 * i + 1] * GAUGE_SHAPE_COORD_SCALE);
    }
    return s;
}

static gauge_shape_t circle(float cx, float cy, float r)
{
    gauge_shape_t s;
    memset(&s, 0, sizeof(s));
    s.part_count      = 1;
    s.parts[0].kind   = GAUGE_SHAPE_PART_CIRCLE;
    s.parts[0].center = (gauge_point_t){(int16_t)lroundf(cx * 16), (int16_t)lroundf(cy * 16)};
    s.parts[0].radius = (int16_t)lroundf(r * 16);
    return s;
}

/* Rasterise part 0 into s_cov; returns total coverage in square pixels. */
static float raster(const gauge_shape_t *s, float deg, float ox, float oy, gauge_shape_rect_t *r)
{
    gauge_shape_vec_t v[GAUGE_SHAPE_MAX_OUTLINE];
    size_t            n = gauge_shape_outline(s, 0, deg, ox, oy, v);
    gauge_shape_bounds(v, n, r);

    if (gauge_shape_rect_w(r) > BUF_DIM || gauge_shape_rect_h(r) > BUF_DIM) {
        printf("  test shape too large for buffer\n");
        exit(2);
    }

    gauge_shape_rasterize(v, n, r, s_acc, s_cov);

    long total = 0;
    for (int32_t i = 0; i < gauge_shape_rect_w(r) * gauge_shape_rect_h(r); i++) {
        total += s_cov[i];
    }
    return (float)total / 255.0f;
}

/* Coverage of absolute pixel (x, y), or 0 outside the mask. */
static int px(const gauge_shape_rect_t *r, int32_t x, int32_t y)
{
    if (x < r->x1 || x > r->x2 || y < r->y1 || y > r->y2) {
        return 0;
    }
    return s_cov[(y - r->y1) * gauge_shape_rect_w(r) + (x - r->x1)];
}

/* ------------------------------------------------------------------------------------ */

static void test_aligned_square(void)
{
    static const float SQ[] = {2, 2, 6, 2, 6, 6, 2, 6};
    gauge_shape_t      s    = polygon(SQ, 4);
    gauge_shape_rect_t r;

    float area = raster(&s, 0, 0, 0, &r);
    CHECK(fabsf(area - 16.0f) < 0.05f, "area %.3f, expected 16", area);
    CHECK(px(&r, 2, 2) == 255 && px(&r, 5, 5) == 255, "inside pixels fully covered");
    CHECK(px(&r, 6, 3) == 0 && px(&r, 1, 3) == 0 && px(&r, 3, 6) == 0, "outside pixels empty");
}

static void test_half_pixel_edges(void)
{
    static const float SQ[] = {2.5f, 2.5f, 6.5f, 2.5f, 6.5f, 6.5f, 2.5f, 6.5f};
    gauge_shape_t      s    = polygon(SQ, 4);
    gauge_shape_rect_t r;

    float area = raster(&s, 0, 0, 0, &r);
    CHECK(fabsf(area - 16.0f) < 0.05f, "area %.3f, expected 16", area);
    CHECK(abs(px(&r, 2, 4) - 128) <= 1, "edge pixel half covered: %d", px(&r, 2, 4));
    CHECK(abs(px(&r, 2, 2) - 64) <= 1, "corner pixel quarter covered: %d", px(&r, 2, 2));
    CHECK(px(&r, 4, 4) == 255, "interior full");
}

static void test_winding_independent(void)
{
    static const float CW[]  = {10.3f, 4, 30, 12.7f, 18, 28.1f, 6, 20};
    static const float CCW[] = {6, 20, 18, 28.1f, 30, 12.7f, 10.3f, 4};

    gauge_shape_t      a = polygon(CW, 4), b = polygon(CCW, 4);
    gauge_shape_rect_t ra, rb;
    static uint8_t     first[BUF_DIM * BUF_DIM];

    raster(&a, 0, 0, 0, &ra);
    memcpy(first, s_cov, sizeof(first));
    raster(&b, 0, 0, 0, &rb);

    size_t len = (size_t)(gauge_shape_rect_w(&ra) * gauge_shape_rect_h(&ra));
    CHECK(memcmp(&ra, &rb, sizeof(ra)) == 0, "same bounds");
    CHECK(memcmp(first, s_cov, len) == 0, "clockwise and anticlockwise must match");
}

static void test_concave(void)
{
    /* An L: 6x2 across the top plus 2x4 down the left, offset by (2, 2). Area 20. */
    static const float L[] = {2, 2, 8, 2, 8, 4, 4, 4, 4, 8, 2, 8};
    gauge_shape_t      s   = polygon(L, 6);
    gauge_shape_rect_t r;

    float area = raster(&s, 0, 0, 0, &r);
    CHECK(fabsf(area - 20.0f) < 0.05f, "area %.3f, expected 20", area);
    CHECK(px(&r, 6, 6) == 0, "the notch stays empty");
    CHECK(px(&r, 3, 7) == 255 && px(&r, 7, 3) == 255, "both arms filled");
}

static void test_no_internal_seams(void)
{
    /*
     * The reason this rasteriser exists: LVGL triangles would leave a faint line along every
     * internal diagonal. Collinear extra vertices and a concave star must still fill solid.
     */
    static const float RECT[] = {10, 10, 20, 10, 30, 10, 30, 20, 20, 20, 10, 20};
    gauge_shape_t      s      = polygon(RECT, 6);
    gauge_shape_rect_t r;

    raster(&s, 0, 0, 0, &r);
    int holes = 0;
    for (int y = 10; y < 20; y++) {
        for (int x = 10; x < 30; x++) {
            holes += px(&r, x, y) != 255;
        }
    }
    CHECK(holes == 0, "%d interior pixels not fully covered", holes);

    float star[24];
    for (int i = 0; i < 12; i++) {
        float a = (float)i * 3.14159265f / 6.0f;
        float d = (i % 2) ? 12.0f : 40.0f;
        star[2 * i]     = 50.0f + d * sinf(a);
        star[2 * i + 1] = 50.0f - d * cosf(a);
    }
    s = polygon(star, 12);
    raster(&s, 0, 0, 0, &r);
    holes = 0;
    for (int y = 42; y < 58; y++) {
        for (int x = 42; x < 58; x++) {
            holes += px(&r, x, y) != 255;
        }
    }
    CHECK(holes == 0, "star core: %d pixels not fully covered", holes);
}

static void test_circle(void)
{
    gauge_shape_t      s = circle(0, 0, 10);
    gauge_shape_rect_t r;

    float area = raster(&s, 0, 40, 40, &r);
    float want = 3.14159265f * 100.0f;
    CHECK(fabsf(area - want) / want < 0.015f, "circle area %.2f, expected ~%.2f", area, want);
    CHECK(px(&r, 40, 40) == 255 && px(&r, 39, 39) == 255, "centre covered");
    CHECK(px(&r, 40, 29) == 0 && px(&r, 51, 40) == 0, "outside the radius empty");

    /* An off-centre circle orbits the origin as the shape rotates. */
    s = circle(0, -20, 4);
    raster(&s, 90, 100, 100, &r);
    CHECK(px(&r, 120, 100) == 255, "centre rotated to 3 o'clock");
}

static void test_rotation_convention(void)
{
    static const float NEEDLE[] = {0, -100, 5, 0, -5, 0};
    gauge_shape_t      s        = polygon(NEEDLE, 3);
    gauge_shape_vec_t  v[GAUGE_SHAPE_MAX_OUTLINE];

    gauge_shape_outline(&s, 0, 0, 200, 200, v);
    CHECK(fabsf(v[0].x - 200) < 1e-3f && fabsf(v[0].y - 100) < 1e-3f, "0 deg points up");

    gauge_shape_outline(&s, 0, 90, 200, 200, v);
    CHECK(fabsf(v[0].x - 300) < 1e-3f && fabsf(v[0].y - 200) < 1e-3f,
          "90 deg is 3 o'clock: (%.2f, %.2f)", v[0].x, v[0].y);

    gauge_shape_outline(&s, 0, 225, 200, 200, v);
    CHECK(v[0].x < 200 && v[0].y > 200, "225 deg is lower-left: (%.2f, %.2f)", v[0].x, v[0].y);

    CHECK(gauge_shape_outline(&s, 1, 0, 0, 0, v) == 0, "missing part yields nothing");
}

static void test_extent_and_reach(void)
{
    static const float NEEDLE[] = {0, -170, 9, 0, 0, 30, -9, 0};
    gauge_shape_t      s        = polygon(NEEDLE, 4);

    CHECK(fabsf(gauge_shape_extent(&s) - 170.0f) < 1e-3f, "extent %.3f", gauge_shape_extent(&s));
    CHECK(fabsf(gauge_shape_max_y(&s) - 30.0f) < 1e-3f, "max y %.3f", gauge_shape_max_y(&s));

    s = circle(0, 5, 3);
    CHECK(fabsf(gauge_shape_extent(&s) - 8.0f) < 1e-3f, "circle extent");
    CHECK(fabsf(gauge_shape_max_y(&s) - 8.0f) < 1e-3f, "circle max y");

    gauge_shape_t empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(gauge_shape_extent(&empty) == 0.0f && !gauge_shape_is_set(&empty), "empty shape");
}

static void test_max_size_is_worst_case(void)
{
    /*
     * The renderer sizes each needle part's buffer from gauge_shape_max_size() once, then
     * rasterises into it every frame. If any angle or sub-pixel position produced larger bounds,
     * that would be a buffer overrun in PSRAM -- so try a great many.
     */
    static const float NEEDLE[] = {0, -171.3f, 4.2f, -20, 8.9f, 0, 3.1f, 33, -3.1f, 33, -8.9f, 0};
    gauge_shape_t      shapes[] = {polygon(NEEDLE, 6), circle(2.5f, -7.25f, 13.4f)};

    uint32_t seed = 12345;
    for (size_t k = 0; k < sizeof(shapes) / sizeof(shapes[0]); k++) {
        int32_t mw, mh;
        gauge_shape_max_size(&shapes[k], 0, &mw, &mh);

        int over = 0;
        for (int i = 0; i < 20000; i++) {
            seed      = seed * 1664525u + 1013904223u;
            float deg = (float)(seed % 36000u) / 100.0f;
            seed      = seed * 1664525u + 1013904223u;
            float ox  = 233.0f + (float)(seed % 1000u) / 1000.0f;
            seed      = seed * 1664525u + 1013904223u;
            float oy  = 233.0f + (float)(seed % 1000u) / 1000.0f;

            gauge_shape_vec_t  v[GAUGE_SHAPE_MAX_OUTLINE];
            gauge_shape_rect_t r;
            size_t n = gauge_shape_outline(&shapes[k], 0, deg, ox, oy, v);
            gauge_shape_bounds(v, n, &r);
            over += gauge_shape_rect_w(&r) > mw || gauge_shape_rect_h(&r) > mh;
        }
        CHECK(over == 0, "shape %zu: %d placements exceeded max size %dx%d", k, over, (int)mw,
              (int)mh);
    }
}

static void test_part_colour(void)
{
    gauge_shape_t s        = circle(0, 0, 5);
    gauge_color_t inherit  = {1, 2, 3};
    bool          inherits = false;

    gauge_color_t c = gauge_shape_part_color(&s, 0, inherit, &inherits);
    CHECK(c.r == 1 && inherits, "no colours set: inherit");

    s.has_color = true;
    s.color     = (gauge_color_t){9, 9, 9};
    c           = gauge_shape_part_color(&s, 0, inherit, &inherits);
    CHECK(c.r == 9 && !inherits, "shape colour beats inherited");

    s.parts[0].has_color = true;
    s.parts[0].color     = (gauge_color_t){7, 7, 7};
    c                    = gauge_shape_part_color(&s, 0, inherit, NULL);
    CHECK(c.r == 7, "part colour beats shape colour");
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"aligned_square",          test_aligned_square},
        {"half_pixel_edges",        test_half_pixel_edges},
        {"winding_independent",     test_winding_independent},
        {"concave",                 test_concave},
        {"no_internal_seams",       test_no_internal_seams},
        {"circle",                  test_circle},
        {"rotation_convention",     test_rotation_convention},
        {"extent_and_reach",        test_extent_and_reach},
        {"max_size_is_worst_case",  test_max_size_is_worst_case},
        {"part_colour",             test_part_colour},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failures;
        printf("%s\n", tests[i].name);
        tests[i].fn();
        if (g_failures == before) {
            printf("  ok\n");
        }
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
