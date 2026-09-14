/**
 * @file gauge_render.h
 * @brief Renders a dial gauge from a parsed configuration.
 *
 * The face -- bands, ticks, labels, titles -- is rasterised once into a PSRAM canvas and
 * becomes the background. Only a small needle sprite and the readout label move, which is
 * what keeps the per-frame dirty region small enough to hold 60fps.
 *
 * See docs/adr/0003-static-background-plus-needle-sprite.md and docs/display-pipeline.md.
 *
 * All functions must be called with the LVGL lock held (bsp_display_lock).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#include "board_profile.h"
#include "gauge_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gauge_render_t gauge_render_t;

/**
 * @brief Build a gauge on @p parent.
 *
 * Pre-renders the face, which is the expensive part -- expect tens of milliseconds and a
 * full-frame allocation in PSRAM. Do it at startup or on config change, never per frame.
 *
 * @param parent Parent object, typically a screen or a tileview tile.
 * @param cfg    Parsed configuration. Copied, so the caller may free it afterwards.
 * @param board  Panel geometry. Must outlive the gauge.
 * @param[out] out Created gauge.
 */
esp_err_t gauge_render_create(lv_obj_t *parent, const gauge_config_t *cfg,
                              const board_profile_t *board, gauge_render_t **out);

/** @brief Destroy a gauge and release its buffers. */
void gauge_render_destroy(gauge_render_t *g);

/**
 * @brief Set the displayed value.
 *
 * Applies the configured damping, moves the needle and updates the readout. Values outside
 * the configured range clamp to the ends of the scale rather than swinging past them.
 *
 * @param value Value in the channel's native unit.
 * @param valid False if the sensor is absent, faulted or stale -- the needle parks at
 *              minimum and the readout shows dashes. A gauge that displays a stale number is
 *              worse than one that admits it lost the sensor.
 */
void gauge_render_set_value(gauge_render_t *g, float value, bool valid);

/** @brief The value currently displayed, after damping. */
float gauge_render_get_displayed(const gauge_render_t *g);

/**
 * @brief Called when the gauge enters or leaves an alert.
 *
 * Fires on the transition only, not on every update, and runs on the LVGL task.
 *
 * @param alert     The alert that fired (or that just cleared).
 * @param active    True on entry, false when it clears.
 * @param user_data As passed to gauge_render_set_alert_cb().
 */
typedef void (*gauge_render_alert_cb_t)(const gauge_alert_t *alert, bool active, void *user_data);

/** @brief Register an alert transition callback, or NULL to remove it. */
void gauge_render_set_alert_cb(gauge_render_t *g, gauge_render_alert_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
