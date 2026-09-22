/*
 * Raw-reading conversions. See sensor_math.h.
 *
 * No ESP-IDF headers here: this file is compiled into the host tests as well as the firmware.
 */

#include "sensor_math.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

float sensor_math_ads1115_volts(int16_t counts, float fsr_v)
{
    /* Two's complement, full scale = 32768 counts (ADS1115 datasheet, SBAS444E 8.1.2). */
    return (float)counts * fsr_v / 32768.0f;
}

float sensor_math_tmp1075_celsius(uint16_t reg)
{
    /* T[11:0] in bits 15:4, two's complement (TMP1075 datasheet, SBOS854F 7.5.1.1). */
    return (float)((int16_t)reg >> 4) * 0.0625f;
}

/*
 * NIST ITS-90 type K reference function coefficients, E in mV, t in degrees C (NIST Monograph
 * 175 / SRD 60, as transcribed in the `thermocouples_reference` package's source_NIST.py).
 * Lowest order first.
 */
static const double k_below_zero[] = {
     0.000000000000e+00,  0.394501280250e-01,  0.236223735980e-04, -0.328589067840e-06,
    -0.499048287770e-08, -0.675090591730e-10, -0.574103274280e-12, -0.310888728940e-14,
    -0.104516093650e-16, -0.198892668780e-19, -0.163226974860e-22,
};

static const double k_above_zero[] = {
    -0.176004136860e-01,  0.389212049750e-01,  0.185587700320e-04, -0.994575928740e-07,
     0.318409457190e-09, -0.560728448890e-12,  0.560750590590e-15, -0.320207200030e-18,
     0.971511471520e-22, -0.121047212750e-25,
};

/* The exponential term added above 0 C: a0 * exp(a1 * (t - a2)^2). */
static const double k_a0 = 0.118597600000e+00;
static const double k_a1 = -0.118343200000e-03;
static const double k_a2 = 0.126968600000e+03;

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* Evaluates the polynomial and its derivative together (Horner). */
static void poly(const double *c, size_t n, double t, double *value, double *slope)
{
    double v = 0.0;
    double d = 0.0;
    for (size_t i = n; i-- > 0;) {
        d = d * t + v;
        v = v * t + c[i];
    }
    *value = v;
    *slope = d;
}

/* E(t) and dE/dt, in mV and mV/C. */
static void type_k(double t, double *emf, double *slope)
{
    if (t < 0.0) {
        poly(k_below_zero, COUNT_OF(k_below_zero), t, emf, slope);
        return;
    }

    poly(k_above_zero, COUNT_OF(k_above_zero), t, emf, slope);

    double u = t - k_a2;
    double g = k_a0 * exp(k_a1 * u * u);
    *emf   += g;
    *slope += g * 2.0 * k_a1 * u;
}

double sensor_math_type_k_mv(double t_c)
{
    double emf;
    double slope;
    type_k(t_c, &emf, &slope);
    return emf;
}

bool sensor_math_type_k_celsius(double emf_mv, double *t_c)
{
    const double lo_mv = sensor_math_type_k_mv(SENSOR_MATH_TYPE_K_MIN_C);
    const double hi_mv = sensor_math_type_k_mv(SENSOR_MATH_TYPE_K_MAX_C);

    if (emf_mv <= lo_mv) {
        *t_c = SENSOR_MATH_TYPE_K_MIN_C;
        return emf_mv == lo_mv;
    }
    if (emf_mv >= hi_mv) {
        *t_c = SENSOR_MATH_TYPE_K_MAX_C;
        return emf_mv == hi_mv;
    }

    /*
     * Newton's method. E(t) is monotonic across the range, and the Seebeck coefficient never
     * falls below ~0.004 mV/C (at -270 C), so the step is always well defined. A linear guess
     * at ~41 uV/C is within a few tens of degrees everywhere above -100 C.
     */
    double t = emf_mv / 0.041;
    for (int i = 0; i < 30; i++) {
        double emf;
        double slope;
        type_k(t, &emf, &slope);
        if (slope < 1e-4) {
            slope = 1e-4;
        }
        double step = (emf - emf_mv) / slope;
        t -= step;
        if (t < SENSOR_MATH_TYPE_K_MIN_C) {
            t = SENSOR_MATH_TYPE_K_MIN_C;
        } else if (t > SENSOR_MATH_TYPE_K_MAX_C) {
            t = SENSOR_MATH_TYPE_K_MAX_C;
        }
        if (fabs(step) < 1e-4) {
            break;
        }
    }

    *t_c = t;
    return true;
}

bool sensor_math_egt_celsius(float probe_v, float cold_junction_c, float *egt_c)
{
    double emf = (double)probe_v * 1000.0 + sensor_math_type_k_mv((double)cold_junction_c);
    double t;
    bool   ok = sensor_math_type_k_celsius(emf, &t);
    *egt_c    = (float)t;
    return ok;
}

float sensor_math_linear(float v_out, float v_lo, float p_lo, float v_hi, float p_hi)
{
    float span = v_hi - v_lo;
    if (fabsf(span) < 1e-6f) {
        return p_lo;
    }
    return p_lo + (v_out - v_lo) * (p_hi - p_lo) / span;
}

float sensor_math_lowpass(float prev, float in, float dt_ms, float tau_ms)
{
    if (tau_ms <= 0.0f || dt_ms <= 0.0f) {
        return in;
    }
    float alpha = 1.0f - expf(-dt_ms / tau_ms);
    return prev + (in - prev) * alpha;
}

/* Case-insensitive compare, skipping a leading UTF-8 degree sign on the unit. */
static bool unit_is(const char *unit, const char *want)
{
    if ((unsigned char)unit[0] == 0xC2 && (unsigned char)unit[1] == 0xB0) {
        unit += 2;
    }
    for (; *unit != '\0' && *want != '\0'; unit++, want++) {
        if (tolower((unsigned char)*unit) != *want) {
            return false;
        }
    }
    return *unit == '\0' && *want == '\0';
}

bool sensor_math_to_unit(sensor_math_qty_t qty, float native, const char *unit, float *out)
{
    *out = native;
    if (unit == NULL || unit[0] == '\0') {
        return false;
    }

    switch (qty) {
    case SENSOR_MATH_QTY_PRESSURE_KPA:
        if (unit_is(unit, "kpa")) {
            return true;
        }
        if (unit_is(unit, "psi")) {
            *out = native / 6.894757f;
            return true;
        }
        if (unit_is(unit, "bar")) {
            *out = native / 100.0f;
            return true;
        }
        if (unit_is(unit, "inhg")) {
            *out = native / 3.386389f;
            return true;
        }
        return false;

    case SENSOR_MATH_QTY_TEMP_C:
        if (unit_is(unit, "c")) {
            return true;
        }
        if (unit_is(unit, "f")) {
            *out = native * 9.0f / 5.0f + 32.0f;
            return true;
        }
        if (unit_is(unit, "k")) {
            *out = native + 273.15f;
            return true;
        }
        return false;

    case SENSOR_MATH_QTY_VOLTS:
        return unit_is(unit, "v");
    }
    return false;
}
