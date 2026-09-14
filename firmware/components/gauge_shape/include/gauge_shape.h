/**
 * @file gauge_shape.h
 * @brief Geometry and anti-aliased rasterisation for custom gauge shapes.
 *
 * Turns a gauge_shape_t from the config into A8 coverage masks that the renderer blends. Like
 * gauge_config, this depends on neither ESP-IDF nor LVGL, so it builds and is tested on a host.
 *
 * Why a rasteriser of our own rather than LVGL's triangles: LVGL anti-aliases every triangle's
 * edges independently, so a polygon split into triangles shows faint seams along the internal
 * edges. This computes exact per-pixel area coverage of the whole outline in one pass, so there
 * are no internal edges to show. See docs/adr/0006-custom-shapes-as-polygons.md.
 *
 * Coordinate frame: shapes are authored pointing at 12 o'clock, x to the right and y downwards
 * (the SVG convention). Rotating by a schema angle -- degrees clockwise from 12 o'clock --
 * swings a shape into place. Pixel (i, j) covers the continuous square [i, i+1) x [j, j+1).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gauge_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Vertices in one part's outline: a polygon's points, or a flattened circle. */
#define GAUGE_SHAPE_MAX_OUTLINE GAUGE_SHAPE_MAX_POINTS

typedef struct {
    float x, y;
} gauge_shape_vec_t;

/** Pixel rectangle, inclusive on all four sides. */
typedef struct {
    int32_t x1, y1, x2, y2;
} gauge_shape_rect_t;

static inline int32_t gauge_shape_rect_w(const gauge_shape_rect_t *r)
{
    return r->x2 - r->x1 + 1;
}

static inline int32_t gauge_shape_rect_h(const gauge_shape_rect_t *r)
{
    return r->y2 - r->y1 + 1;
}

/** @brief True if the config specified this shape, so it replaces the built-in drawing. */
static inline bool gauge_shape_is_set(const gauge_shape_t *shape)
{
    return shape != NULL && shape->part_count > 0;
}

/** @brief Furthest distance from the shape's origin to anything it covers, in px. */
float gauge_shape_extent(const gauge_shape_t *shape);

/**
 * @brief Largest y any part reaches, in px.
 *
 * For a tick, this is how far it extends from the scale towards the dial centre.
 */
float gauge_shape_max_y(const gauge_shape_t *shape);

/**
 * @brief Outline of one part, rotated and positioned.
 *
 * @param deg     Rotation, degrees clockwise, about the shape's origin.
 * @param ox, oy  Where the shape's origin lands, in output coordinates.
 * @param[out] out Must hold GAUGE_SHAPE_MAX_OUTLINE vertices.
 * @return Vertex count; 0 for a part index that does not exist.
 */
size_t gauge_shape_outline(const gauge_shape_t *shape, uint8_t part, float deg, float ox,
                           float oy, gauge_shape_vec_t *out);

/** @brief Pixel bounds of an outline, with a one-pixel margin for anti-aliasing. */
void gauge_shape_bounds(const gauge_shape_vec_t *v, size_t n, gauge_shape_rect_t *out);

/**
 * @brief Largest bounds one part can need at any rotation and sub-pixel position.
 *
 * Lets a renderer size a part's buffer once, at startup, instead of on every frame.
 */
void gauge_shape_max_size(const gauge_shape_t *shape, uint8_t part, int32_t *w, int32_t *h);

/**
 * @brief Rasterise an outline into an A8 coverage mask with exact-area anti-aliasing.
 *
 * Coverage is independent of winding direction. Parts should be simple polygons; where a
 * self-intersecting one overlaps itself, coverage saturates rather than cancelling.
 *
 * @param rect Area the mask covers. Must contain the outline -- use gauge_shape_bounds().
 * @param acc  Scratch: width x height floats.
 * @param[out] cov Width x height bytes, row-major, 255 = fully covered.
 */
void gauge_shape_rasterize(const gauge_shape_vec_t *v, size_t n, const gauge_shape_rect_t *rect,
                           float *acc, uint8_t *cov);

/**
 * @brief Resolve a part's colour: the part's own, then the shape's, then @p inherited.
 *
 * @param[out] inherits Optional. True when the result is @p inherited -- used to decide which
 *                      needle parts take the alert colour.
 */
gauge_color_t gauge_shape_part_color(const gauge_shape_t *shape, uint8_t part,
                                     gauge_color_t inherited, bool *inherits);

#ifdef __cplusplus
}
#endif
