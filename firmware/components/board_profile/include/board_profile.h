/**
 * @file board_profile.h
 * @brief Panel geometry and board capabilities.
 *
 * The portability seam. Everything that would otherwise hard-code 466x466 reads from here
 * instead, so supporting a square or differently-sized panel is a new profile plus a BSP
 * swap rather than a hunt through the rendering code.
 *
 * See docs/architecture.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Physical shape of the panel. Round panels clip their corners. */
typedef enum {
    BOARD_PANEL_ROUND,
    BOARD_PANEL_SQUARE,
} board_panel_shape_t;

typedef struct {
    /** Human-readable board name, for logs and the settings screen. */
    const char *name;

    board_panel_shape_t shape;
    uint16_t width_px;
    uint16_t height_px;

    /**
     * Inset from the panel edge that is reliably visible.
     *
     * Round panels lose their corners, and bezels eat a few pixels on any panel. UI that
     * must be seen stays inside this margin.
     */
    uint16_t safe_inset_px;

    /** Panel refresh rate, and therefore the frame rate the renderer targets. */
    uint8_t refresh_hz;

    /* Capabilities. Features degrade gracefully when these are false. */
    bool has_touch;
    bool has_audio;   /**< Audible alerts (docs/adr/0004-retain-sd-and-audio.md). */
    bool has_sdcard;
    bool has_imu;
    bool has_rtc;
    bool has_battery; /**< Battery telemetry via the PMIC. */
} board_profile_t;

/**
 * @brief The profile for the board this firmware was built for.
 * @return Never NULL.
 */
const board_profile_t *board_profile_get(void);

/**
 * @brief Radius of the largest circle fitting inside the safe area.
 *
 * The natural outer radius for a dial face when the gauge config does not specify one.
 */
uint16_t board_profile_max_radius(const board_profile_t *profile);

#ifdef __cplusplus
}
#endif
