/*
 * Host tests for the sensor conversions.
 *
 * What matters on the gauge: the thermocouple maths agrees with the NIST type K table (a wrong
 * coefficient would read hundreds of degrees off and nothing on the bench would notice until a
 * probe is hot), the inverse really inverts, cold-junction compensation adds rather than
 * subtracts, and register decoding handles negative values.
 */

#include "sensor_math.h"

#include <math.h>
#include <stdio.h>
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

#define NEAR(a, b, tol) (fabs((double)(a) - (double)(b)) <= (tol))

/*
 * NIST ITS-90 type K table values, mV, reference junction at 0 C. The table is published to
 * 0.001 mV, so agreement within half a digit is exact agreement.
 */
static const struct {
    double t_c;
    double mv;
} k_table[] = {
    {-200.0, -5.891}, {-40.0, -1.527}, {-10.0, -0.392}, {0.0, 0.000},    {25.0, 1.000},
    {100.0, 4.096},   {250.0, 10.153}, {500.0, 20.644}, {750.0, 31.213}, {900.0, 37.326},
    {1000.0, 41.276}, {1200.0, 48.838}, {1372.0, 54.886},
};

#define TABLE_LEN (sizeof(k_table) / sizeof(k_table[0]))

static void test_type_k_forward_matches_nist(void)
{
    for (size_t i = 0; i < TABLE_LEN; i++) {
        double mv = sensor_math_type_k_mv(k_table[i].t_c);
        CHECK(NEAR(mv, k_table[i].mv, 0.0006), "E(%.0f) = %.4f, NIST %.3f", k_table[i].t_c, mv,
              k_table[i].mv);
    }
}

static void test_type_k_inverse_matches_nist(void)
{
    /* 0.001 mV is ~0.025 C at the steepest, so the table only pins the inverse to ~0.03 C. */
    for (size_t i = 0; i < TABLE_LEN; i++) {
        double t;
        bool   ok = sensor_math_type_k_celsius(k_table[i].mv, &t);
        CHECK(ok || k_table[i].t_c == 1372.0, "%.3f mV reported out of range", k_table[i].mv);
        CHECK(NEAR(t, k_table[i].t_c, 0.05), "T(%.3f mV) = %.3f, NIST %.0f", k_table[i].mv, t,
              k_table[i].t_c);
    }
}

static void test_type_k_round_trip(void)
{
    for (double t = -250.0; t <= 1370.0; t += 7.3) {
        double back;
        sensor_math_type_k_celsius(sensor_math_type_k_mv(t), &back);
        CHECK(NEAR(back, t, 0.001), "round trip %.2f -> %.4f", t, back);
    }
}

static void test_type_k_out_of_range(void)
{
    double t;
    CHECK(!sensor_math_type_k_celsius(60.0, &t), "60 mV accepted");
    CHECK(t == SENSOR_MATH_TYPE_K_MAX_C, "60 mV clamped to %.1f", t);
    CHECK(!sensor_math_type_k_celsius(-7.0, &t), "-7 mV accepted");
    CHECK(t == SENSOR_MATH_TYPE_K_MIN_C, "-7 mV clamped to %.1f", t);
}

static void test_cold_junction_compensation(void)
{
    /* Probe at 500 C, connector at 25 C: the probe sees E(500) - E(25). */
    float probe_v = (float)((20.644 - 1.000) / 1000.0);
    float egt;
    CHECK(sensor_math_egt_celsius(probe_v, 25.0f, &egt), "in range");
    CHECK(NEAR(egt, 500.0, 0.1), "EGT %.2f, expected 500", egt);

    /* Probe and connector at the same temperature: no voltage, and the reading is ambient. */
    CHECK(sensor_math_egt_celsius(0.0f, 31.5f, &egt), "in range");
    CHECK(NEAR(egt, 31.5, 0.01), "shorted probe reads %.3f, expected the cold junction", egt);

    /* A freezing morning: a cold junction below 0 C uses the other polynomial. */
    probe_v = (float)((20.644 - -0.392) / 1000.0);
    CHECK(sensor_math_egt_celsius(probe_v, -10.0f, &egt), "in range");
    CHECK(NEAR(egt, 500.0, 0.1), "EGT %.2f at -10 C cold junction, expected 500", egt);
}

static void test_ads1115_scaling(void)
{
    CHECK(NEAR(sensor_math_ads1115_volts(32767, 4.096f), 4.095875, 1e-5), "full scale");
    CHECK(NEAR(sensor_math_ads1115_volts(8000, 4.096f), 1.0, 1e-6), "1 V");
    CHECK(NEAR(sensor_math_ads1115_volts(-32768, 4.096f), -4.096, 1e-6), "negative full scale");
    /* 0.256 V range: 7.8125 uV per count. */
    CHECK(NEAR(sensor_math_ads1115_volts(128, 0.256f), 0.001, 1e-7), "1 mV at 0.256 V FSR");
}

static void test_tmp1075_decoding(void)
{
    CHECK(sensor_math_tmp1075_celsius(0x0000) == 0.0f, "0 C");
    CHECK(sensor_math_tmp1075_celsius(0x1900) == 25.0f, "25 C");
    CHECK(sensor_math_tmp1075_celsius(0x0010) == 0.0625f, "one LSB");
    CHECK(sensor_math_tmp1075_celsius(0xFFF0) == -0.0625f, "minus one LSB");
    CHECK(sensor_math_tmp1075_celsius(0xE700) == -25.0f, "-25 C");
    CHECK(sensor_math_tmp1075_celsius(0x7FF0) == 127.9375f, "maximum");
}

static void test_map_transfer(void)
{
    /* JRP 7-bar as documented: 0.5 V = 0 kPa abs, 4.5 V = 800 kPa abs. */
    CHECK(NEAR(sensor_math_linear(0.5f, 0.5f, 0.0f, 4.5f, 800.0f), 0.0, 1e-4), "bottom");
    CHECK(NEAR(sensor_math_linear(4.5f, 0.5f, 0.0f, 4.5f, 800.0f), 800.0, 1e-3), "top");
    CHECK(NEAR(sensor_math_linear(1.0f, 0.5f, 0.0f, 4.5f, 800.0f), 100.0, 1e-3),
          "1.0 V is about atmospheric");
    /* Degenerate span must not divide by zero. */
    CHECK(sensor_math_linear(1.0f, 2.0f, 5.0f, 2.0f, 9.0f) == 5.0f, "zero span");
}

static void test_lowpass(void)
{
    CHECK(sensor_math_lowpass(0.0f, 10.0f, 10.0f, 0.0f) == 10.0f, "tau 0 passes through");
    /* After one time constant a step has covered 1 - 1/e of the way. */
    float y = 0.0f;
    for (int i = 0; i < 100; i++) {
        y = sensor_math_lowpass(y, 1.0f, 1.0f, 100.0f);
    }
    CHECK(NEAR(y, 1.0 - exp(-1.0), 1e-3), "one tau: %.4f", y);
}

static void test_units(void)
{
    float v;
    CHECK(sensor_math_to_unit(SENSOR_MATH_QTY_PRESSURE_KPA, 100.0f, "psi", &v) &&
              NEAR(v, 14.5038, 1e-3),
          "psi %.4f", v);
    CHECK(sensor_math_to_unit(SENSOR_MATH_QTY_PRESSURE_KPA, 100.0f, "PSI", &v), "case");
    CHECK(sensor_math_to_unit(SENSOR_MATH_QTY_PRESSURE_KPA, 150.0f, "bar", &v) && NEAR(v, 1.5, 1e-6),
          "bar");
    CHECK(sensor_math_to_unit(SENSOR_MATH_QTY_PRESSURE_KPA, 7.0f, "kPa", &v) && v == 7.0f, "kPa");
    CHECK(sensor_math_to_unit(SENSOR_MATH_QTY_TEMP_C, 100.0f, "F", &v) && NEAR(v, 212.0, 1e-4),
          "F");
    CHECK(sensor_math_to_unit(SENSOR_MATH_QTY_TEMP_C, 100.0f, "\xC2\xB0" "C", &v) && v == 100.0f,
          "degree sign");
    CHECK(!sensor_math_to_unit(SENSOR_MATH_QTY_TEMP_C, 42.0f, "furlongs", &v) && v == 42.0f,
          "unknown unit stays native");
    CHECK(!sensor_math_to_unit(SENSOR_MATH_QTY_TEMP_C, 42.0f, "", &v) && v == 42.0f, "empty");
    CHECK(!sensor_math_to_unit(SENSOR_MATH_QTY_PRESSURE_KPA, 1.0f, "psia", &v), "prefix only");
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"type_k_forward_matches_nist", test_type_k_forward_matches_nist},
        {"type_k_inverse_matches_nist", test_type_k_inverse_matches_nist},
        {"type_k_round_trip",           test_type_k_round_trip},
        {"type_k_out_of_range",         test_type_k_out_of_range},
        {"cold_junction_compensation",  test_cold_junction_compensation},
        {"ads1115_scaling",             test_ads1115_scaling},
        {"tmp1075_decoding",            test_tmp1075_decoding},
        {"map_transfer",                test_map_transfer},
        {"lowpass",                     test_lowpass},
        {"units",                       test_units},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", tests[i].name);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
