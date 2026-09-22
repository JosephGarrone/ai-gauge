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

    /*
     * The 8-pin header's free pins (docs/hardware-reference.md): SDA pin 6, SCL pin 7, ADS1115
     * ALERT/RDY pin 8. 100 kHz because the bus leaves the board through a connector.
     *
     * I2C_NUM_0, because the BSP's own bus (touch, codec, PMIC) is on I2C_NUM_1: that is the
     * BSP's CONFIG_BSP_I2C_NUM default. Measured on hardware: asking for port 1 here failed with
     * "bus already acquired", and the failed attempt broke the codec's bus on the way out.
     */
    .sensor_i2c_port   = 0,
    .sensor_sda_gpio   = 16,
    .sensor_scl_gpio   = 17,
    .sensor_alert_gpio = 18,
    .sensor_i2c_hz     = 100000,
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
