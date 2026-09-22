/**
 * @file sensor_math.h
 * @brief Conversions from raw front-end readings to engineering units.
 *
 * Pure functions with no ESP-IDF dependency, so they are tested on the host
 * (tools/host-tests/test_sensor_math.c). The procedure and its sources are in
 * docs/sensor-frontend.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Hottest temperature the NIST type K reference function covers, degrees C. */
#define SENSOR_MATH_TYPE_K_MAX_C 1372.0

/** Coldest temperature the NIST type K reference function covers, degrees C. */
#define SENSOR_MATH_TYPE_K_MIN_C (-270.0)

/** Volts at the ADS1115 input for a conversion result at the given full-scale range. */
float sensor_math_ads1115_volts(int16_t counts, float fsr_v);

/** Degrees C from the TMP1075 temperature register (12-bit, left-justified, 0.0625 C/LSB). */
float sensor_math_tmp1075_celsius(uint16_t reg);

/**
 * @brief Type K thermoelectric voltage, in millivolts, at @p t_c with the reference junction at 0 C.
 *
 * The NIST ITS-90 reference function, including the exponential term above 0 C. Valid from
 * -270 to 1372 C; outside that range the polynomial is evaluated anyway and means nothing.
 */
double sensor_math_type_k_mv(double t_c);

/**
 * @brief Temperature, in degrees C, that produces @p emf_mv on a type K thermocouple.
 *
 * Inverts sensor_math_type_k_mv() numerically rather than using NIST's inverse polynomials, so
 * the result is exactly as accurate as the reference function. Converges in a handful of Newton
 * steps from a linear first guess.
 *
 * @return false if @p emf_mv lies outside the type K range; @p t_c is then clamped to the end.
 */
bool sensor_math_type_k_celsius(double emf_mv, double *t_c);

/**
 * @brief Hot-junction temperature from the voltage across the probe and the cold-junction temperature.
 *
 * Cold-junction compensation: the probe sees only the difference between the two junctions, so
 * the cold junction's own EMF is added back before inverting.
 *
 * @return false if the result is outside the type K range.
 */
bool sensor_math_egt_celsius(float probe_v, float cold_junction_c, float *egt_c);

/**
 * @brief Linear sensor transfer function: the pressure for an output voltage.
 *
 * @param v_out   Sensor output, volts, referred to a 5.00V supply.
 * @param v_lo    Output at @p p_lo.
 * @param v_hi    Output at @p p_hi. Must differ from @p v_lo.
 */
float sensor_math_linear(float v_out, float v_lo, float p_lo, float v_hi, float p_hi);

/**
 * @brief One step of a first-order low-pass filter with time constant @p tau_ms.
 *
 * @p tau_ms of 0 passes the input straight through. @p dt_ms is the time since the last sample.
 */
float sensor_math_lowpass(float prev, float in, float dt_ms, float tau_ms);

/** The native unit a quantity is published in. */
typedef enum {
    SENSOR_MATH_QTY_PRESSURE_KPA, /**< Native kPa; converts to psi, bar, kpa, inhg. */
    SENSOR_MATH_QTY_TEMP_C,       /**< Native degrees C; converts to c, f, k. */
    SENSOR_MATH_QTY_VOLTS,        /**< Native volts; no conversion. */
} sensor_math_qty_t;

/**
 * @brief Convert a native value into the unit a gauge face asks for.
 *
 * Matching ignores case and a leading degree sign, so "psi", "PSI", "C" and "°C" all work. An
 * empty or unknown unit leaves the value in its native unit.
 *
 * @return false if @p unit was not recognised (the value is then native).
 */
bool sensor_math_to_unit(sensor_math_qty_t qty, float native, const char *unit, float *out);

#ifdef __cplusplus
}
#endif
