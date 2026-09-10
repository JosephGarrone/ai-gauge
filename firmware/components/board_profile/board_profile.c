#include "board_profile.h"

#include <stddef.h>

/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.75.
 * Hardware details: docs/hardware-reference.md.
 */
static const board_profile_t s_profile = {
    .name           = "Waveshare ESP32-S3-Touch-AMOLED-1.75",
    .shape          = BOARD_PANEL_ROUND,
    .width_px       = 466,
    .height_px      = 466,
    /* A round panel clips its corners; 8px keeps content clear of the visible edge. */
    .safe_inset_px  = 8,
    .refresh_hz     = 60,
    .has_touch      = true,
    .has_audio      = true,
    .has_sdcard     = true,
    .has_imu        = true,
    .has_rtc        = true,
    .has_battery    = true,
};

const board_profile_t *board_profile_get(void)
{
    return &s_profile;
}

uint16_t board_profile_max_radius(const board_profile_t *profile)
{
    if (profile == NULL) {
        return 0;
    }

    uint16_t shorter = (profile->width_px < profile->height_px) ? profile->width_px
                                                                : profile->height_px;

    /* The inset applies at both edges, so it costs twice its value across the diameter. */
    uint16_t inset_diameter = (uint16_t)(profile->safe_inset_px * 2);
    if (inset_diameter >= shorter) {
        return 0;
    }

    return (uint16_t)((shorter - inset_diameter) / 2);
}
