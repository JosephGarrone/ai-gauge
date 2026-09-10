/**
 * @file gauge_perf.h
 * @brief Frame-rate and dirty-region instrumentation.
 *
 * The 60fps requirement is governed by how much of the screen is redrawn each frame, not by
 * raw CPU speed -- a full-frame blit alone costs ~11ms of the 16.6ms budget. LVGL's built-in
 * FPS overlay reports its own view of the world; this reports the two numbers that actually
 * predict whether a change is affordable:
 *
 *   - dirty area per frame (px, and as a percentage of the panel)
 *   - bytes transferred per frame over QSPI
 *
 * Method and recorded results: docs/performance.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t frames;            /**< Frames that actually redrew something. */
    uint32_t skipped;           /**< Refresh cycles with nothing to draw. */

    float    fps;               /**< Drawn frames per second over the window. */

    uint32_t dirty_px_mean;
    uint32_t dirty_px_max;
    float    dirty_pct_mean;    /**< Mean dirty area as a percentage of the panel. */

    uint32_t bytes_per_frame;   /**< Mean; dirty_px_mean x bytes per pixel. */

    float    render_ms_mean;    /**< Time inside LVGL rendering. */
    float    render_ms_max;

    uint32_t window_ms;         /**< Wall time the window covered. */
} gauge_perf_stats_t;

/**
 * @brief Attach instrumentation to a display.
 *
 * Registers LVGL display event callbacks. Cheap enough to leave enabled -- it does a little
 * arithmetic per invalidated area and two timestamps per refresh.
 *
 * @param disp Display to instrument, or NULL for the default display.
 */
esp_err_t gauge_perf_attach(lv_display_t *disp);

/** @brief Discard accumulated samples and restart the measurement window. */
void gauge_perf_reset(void);

/** @brief Read the statistics accumulated since the last reset. */
void gauge_perf_get(gauge_perf_stats_t *out);

/**
 * @brief Log the current statistics, then reset the window.
 *
 * @param tag   Log tag to attribute the line to.
 * @param label Short description of what was being measured, e.g. "needle sweep".
 */
void gauge_perf_report(const char *tag, const char *label);

/**
 * @brief Start periodic reporting.
 *
 * @param period_ms How often to log a summary line.
 * @param label     Description recorded with each line.
 */
esp_err_t gauge_perf_start_reporting(uint32_t period_ms, const char *label);

#ifdef __cplusplus
}
#endif
