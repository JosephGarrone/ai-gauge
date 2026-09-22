/**
 * @file sensor_hub.h
 * @brief Reads the rear sensor board (ADS1115 + TMP1075) and publishes channel readings.
 *
 * Owns the sensor I2C bus. A task on core 0 samples the ADC, converts and filters, and
 * publishes a snapshot; readers on any task take a consistent copy without blocking it
 * (docs/architecture.md, "The snapshot rule"). Nothing here touches LVGL.
 *
 * Channels, in their native units:
 *
 * | Channel         | Native | Source |
 * |---|---|---|
 * | `boost`         | kPa    | MAP absolute minus the zero reference (gauge pressure) |
 * | `map`           | kPa    | MAP absolute |
 * | `egt`           | C      | AIN3 thermocouple + TMP1075 cold junction |
 * | `ignition`      | V      | AIN2, the fused ignition feed |
 * | `sensor_supply` | V      | AIN1, the current-limited 5V to the MAP sensor |
 * | `cold_junction` | C      | TMP1075 |
 *
 * Circuit, scaling and fault rules: docs/sensor-frontend.md. Design choices:
 * docs/adr/0009-sensor-hub-sampling-and-calibration.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "sensor_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where the sensor board is wired. From board_profile, so another board is a table change. */
typedef struct {
    int     i2c_port;  /**< I2C controller, e.g. 1. */
    int     sda_gpio;
    int     scl_gpio;
    int     alert_gpio; /**< ADS1115 ALERT/RDY, or -1 to poll the conversion status instead. */
    uint32_t scl_hz;
} sensor_hub_bus_t;

/**
 * Calibration and tuning. Everything a different sensor or a DMM measurement would change lives
 * here rather than in a constant (docs/sensor-frontend.md, Scaling).
 */
typedef struct {
    /* MAP sensor transfer function, output volts referred to a 5.00V supply. */
    float map_v_lo;      /**< Output at map_p_lo_kpa. JRP 7-bar: 0.5V (assumed, unverified). */
    float map_v_hi;      /**< Output at map_p_hi_kpa. JRP 7-bar: 4.5V (assumed, unverified). */
    float map_p_lo_kpa;  /**< Absolute pressure at map_v_lo. */
    float map_p_hi_kpa;  /**< Absolute pressure at map_v_hi. JRP 7-bar: 800 kPa (0-8 bar abs). */
    bool  ratiometric;   /**< Scale the MAP output by the measured supply (AIN1) rather than 5.00V. */

    /* Divider ratios, (Rtop + Rbottom) / Rbottom. Nominal from the schematic; trim to a DMM. */
    float map_div;       /**< R5/R6, 10k/10k: 2.0 */
    float supply_div;    /**< R8/R9, 10k/10k: 2.0 */
    float ignition_div;  /**< R11/R12, 100k/20k: 6.0 */

    /* Boost zero: atmospheric pressure, subtracted from MAP to give gauge pressure. */
    float baro_kpa;      /**< Last zero captured by hand (or the default). */
    bool  auto_zero;     /**< Re-zero at startup if MAP then reads plausibly atmospheric. */

    float egt_offset_c;  /**< Added to the computed EGT. Trim against boiling water. */

    /* First-order low-pass time constants, ms. 0 disables. The face's damping still applies. */
    float boost_tau_ms;
    float egt_tau_ms;
} sensor_hub_cal_t;

/** The calibration a fresh board starts with. */
void sensor_hub_cal_defaults(sensor_hub_cal_t *cal);

typedef enum {
    SENSOR_HUB_OFF = 0,  /**< Not started, or the bus could not be created. */
    SENSOR_HUB_SEARCHING, /**< No board has answered since startup. Probing periodically. */
    SENSOR_HUB_ONLINE,   /**< At least one device is answering. */
    SENSOR_HUB_LOST,     /**< A board was seen, then stopped answering. Channels are invalid. */
} sensor_hub_state_t;

typedef enum {
    SENSOR_HUB_CH_BOOST = 0,
    SENSOR_HUB_CH_MAP,
    SENSOR_HUB_CH_EGT,
    SENSOR_HUB_CH_IGNITION,
    SENSOR_HUB_CH_SENSOR_SUPPLY,
    SENSOR_HUB_CH_COLD_JUNCTION,
    SENSOR_HUB_CH_COUNT,
} sensor_hub_channel_t;

typedef struct {
    float   value;        /**< Native unit (see the table above). */
    bool    valid;        /**< False if absent, faulted or implausible. */
    int64_t timestamp_us; /**< esp_timer_get_time() at the sample. 0 if never sampled. */
} sensor_hub_reading_t;

/** Everything the task publishes. Raw values are for the settings page's diagnostics. */
typedef struct {
    sensor_hub_state_t   state;
    bool                 ads1115_ok;
    bool                 tmp1075_ok;
    bool                 alert_ok;    /**< ALERT/RDY is signalling; false means status polling. */
    sensor_hub_reading_t ch[SENSOR_HUB_CH_COUNT];

    float    map_sensor_v; /**< MAP output at the connector, volts (after undoing the divider). */
    float    tc_uv;        /**< Voltage across the thermocouple, microvolts. */
    float    map_slow_kpa; /**< MAP absolute, heavily filtered: what a zero captures. */
    float    baro_kpa;     /**< Zero reference in use. */
    bool     auto_zeroed;  /**< baro_kpa came from this startup's auto-zero. */
    const char *boost_fault; /**< Why boost is invalid, or NULL. Static string. */
    const char *egt_fault;   /**< Why EGT is invalid, or NULL. Static string. */
    uint32_t i2c_errors;   /**< Failed transactions since startup. */
    uint32_t samples;      /**< MAP conversions since startup. */
} sensor_hub_snapshot_t;

/**
 * @brief Load calibration from NVS, create the bus and start the sampling task.
 *
 * Never fails because the board is missing: the task keeps probing, so a board plugged in later
 * is picked up. Fails only if the bus or task cannot be created at all.
 */
esp_err_t sensor_hub_start(const sensor_hub_bus_t *bus);

/** @brief A consistent copy of the latest snapshot. Never blocks. Safe from any task. */
void sensor_hub_get_snapshot(sensor_hub_snapshot_t *out);

/** @brief Channel index for a gauge's channel name, or SENSOR_HUB_CH_COUNT if not ours. */
sensor_hub_channel_t sensor_hub_channel_from_name(const char *name);

/** @brief The quantity a channel's native unit belongs to, for sensor_math_to_unit(). */
sensor_math_qty_t sensor_hub_channel_qty(sensor_hub_channel_t ch);

/**
 * @brief Read one channel, converted to @p unit.
 *
 * A reading older than a few sample periods is reported invalid: a stalled task must not leave a
 * frozen number on the dial.
 *
 * @return false if the hub has no say over this channel: it is not one of ours, or no board has
 *         been seen since startup. The caller may then use another source. Once a board has been
 *         seen this returns true even while it is lost, with @p valid false, so a failing sensor
 *         is shown as failed rather than silently replaced.
 */
bool sensor_hub_read(const char *channel, const char *unit, float *value, bool *valid);

/** @brief The calibration in use. */
void sensor_hub_get_cal(sensor_hub_cal_t *out);

/**
 * @brief Apply new calibration immediately. Values are clamped to sane ranges.
 *
 * Not persisted until sensor_hub_save_cal(), so a stepper can apply every tap and save once on
 * release.
 */
void sensor_hub_set_cal(const sensor_hub_cal_t *cal);

/**
 * @brief Write the calibration to NVS if it has changed.
 *
 * Call from a task with an internal-RAM stack (the UI task is fine); flash writes are not
 * allowed from the sensor task's PSRAM stack.
 */
esp_err_t sensor_hub_save_cal(void);

/**
 * @brief Take the current MAP reading as atmospheric: boost reads zero from here on.
 *
 * Refused unless the board is online, MAP is valid and the reading is plausibly atmospheric (so a
 * tap while under boost cannot set a wildly wrong zero). Saves on success.
 *
 * @param msg Filled with a one-line result for the user.
 */
esp_err_t sensor_hub_zero_boost(char *msg, size_t msg_len);

/** @brief Short user-facing word for a state, e.g. "online". */
const char *sensor_hub_state_str(sensor_hub_state_t state);

#ifdef __cplusplus
}
#endif
