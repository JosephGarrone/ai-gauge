/*
 * Rear sensor board driver and sampling task. See sensor_hub.h.
 *
 * Register facts are from the TI datasheets, not memory:
 *   ADS1115  SBAS444E  -- pointer 00 conversion / 01 config / 10 Lo_thresh / 11 Hi_thresh;
 *                         config fields and codes in Table 8-3; conversion-ready on ALERT/RDY
 *                         when Hi_thresh MSB = 1, Lo_thresh MSB = 0 and COMP_QUE != 11 (7.3.8).
 *   TMP1075  SBOS854F  -- 00h temperature, 0Fh DIEID = 7500h (not on TMP1075N); continuous
 *                         conversion every 27.5 ms from reset, so it needs no configuration.
 * Addresses and channel wiring are from the fabricated netlist
 * (pcb/map-and-egt-daughterboard/production/netlist.ipc): ADS1115 ADDR to GND = 0x48,
 * TMP1075 A2 A1 A0 = GND GND 3V3 = 0x49; AIN0 MAP/2, AIN1 sensor 5V/2, AIN2 ignition/6,
 * AIN3 thermocouple.
 */

#include "sensor_hub.h"

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "sensor_hub";

/* ---------------------------------------------------------------- devices --------- */

#define ADS1115_ADDR 0x48
#define TMP1075_ADDR 0x49

#define ADS_REG_CONVERSION 0x00
#define ADS_REG_CONFIG     0x01
#define ADS_REG_LO_THRESH  0x02
#define ADS_REG_HI_THRESH  0x03

#define ADS_CFG_OS_START   (1u << 15)
#define ADS_CFG_MUX_AIN(n) ((uint16_t)(0x4u + (n)) << 12) /* AINn to GND */
#define ADS_CFG_PGA(p)     ((uint16_t)(p) << 9)
#define ADS_CFG_SINGLESHOT (1u << 8)
#define ADS_CFG_DR(d)      ((uint16_t)(d) << 5)
#define ADS_CFG_QUE_1      0x0000u /* ALERT/RDY enabled (conversion ready with the thresholds below) */
#define ADS_CFG_QUE_OFF    0x0003u /* ALERT/RDY high impedance */

#define ADS_PGA_4V096 0x1
#define ADS_PGA_0V256 0x5
#define ADS_DR_128    0x4
#define ADS_DR_475    0x6

#define TMP_REG_TEMP  0x00
#define TMP_REG_DIEID 0x0F
#define TMP_DIEID     0x7500

#define I2C_TIMEOUT_MS 10

/* ---------------------------------------------------------------- schedule -------- */

/*
 * One 10 ms tick converts MAP every time (~100 Hz, what a boost needle needs) and slots one slower
 * channel in behind it. The thermocouple gets the slowest data rate, which is also the quietest,
 * because a probe takes about a second to respond anyway.
 */
#define TICK_MS          10
#define SLOT_COUNT       10
#define SLOT_EGT         0 /* 10 Hz */
#define SLOT_SUPPLY      3 /* 10 Hz */
#define SLOT_IGNITION    6 /* 10 Hz */
#define TMP_EVERY_TICKS  50 /* 2 Hz; the cold junction changes slowly */
#define PROBE_EVERY_MS   2000
#define LOST_AFTER_FAILS 5

/* Readings older than this are shown invalid, so a stalled task cannot freeze the needle. */
#define STALE_FAST_US (250 * 1000)
#define STALE_SLOW_US (1500 * 1000)

/* ---------------------------------------------------------------- plausibility ---- */

/* The MAP divider's bottom resistor pulls an unplugged signal to 0V; a live sensor never gets there. */
#define MAP_SIGNAL_MIN_V   0.25f
#define MAP_SIGNAL_MAX_V   4.85f
/* The buck makes 5.02V; much below that the TPS2553 is limiting, which means a harness fault. */
#define SUPPLY_MIN_V       4.5f
#define SUPPLY_MAX_V       5.5f
/* R18 pulls an open probe to 3V3, saturating AIN3. Type K never exceeds ~55 mV. */
#define TC_OPEN_V          0.060f
#define EGT_MIN_C          (-40.0f)
#define EGT_MAX_C          1250.0f
#define CJ_MIN_C           (-55.0f)
#define CJ_MAX_C           125.0f
/* What a zero will accept as atmospheric: ~4000 m altitude to a deep low at sea level. */
#define ATMOSPHERE_MIN_KPA 60.0f
#define ATMOSPHERE_MAX_KPA 110.0f

#define MAP_SLOW_TAU_MS   1000.0f
#define SUPPLY_TAU_MS     200.0f
#define IGNITION_TAU_MS   200.0f
#define AUTO_ZERO_SAMPLES 150 /* 1.5 s of MAP at 100 Hz */

/* ---------------------------------------------------------------- calibration ----- */

#define NVS_NAMESPACE "sensor_cal"
#define NVS_KEY_CAL   "cal"
#define NVS_KEY_VER   "ver"
#define CAL_VERSION   1

void sensor_hub_cal_defaults(sensor_hub_cal_t *cal)
{
    *cal = (sensor_hub_cal_t){
        .map_v_lo     = 0.50f,
        .map_v_hi     = 4.50f,
        .map_p_lo_kpa = 0.0f,
        .map_p_hi_kpa = 800.0f,
        .ratiometric  = true,
        .map_div      = 2.0f,
        .supply_div   = 2.0f,
        .ignition_div = 6.0f,
        .baro_kpa     = 101.325f,
        .auto_zero    = true,
        .egt_offset_c = 0.0f,
        .boost_tau_ms = 20.0f,
        .egt_tau_ms   = 300.0f,
    };
}

static float clampf(float v, float lo, float hi)
{
    if (!(v >= lo)) { /* also catches NaN */
        return lo;
    }
    return (v > hi) ? hi : v;
}

static void cal_sanitise(sensor_hub_cal_t *c)
{
    c->map_v_lo     = clampf(c->map_v_lo, 0.0f, 5.0f);
    c->map_v_hi     = clampf(c->map_v_hi, 0.0f, 5.0f);
    if (fabsf(c->map_v_hi - c->map_v_lo) < 0.5f) {
        c->map_v_hi = clampf(c->map_v_lo + 0.5f, 0.0f, 5.0f);
        c->map_v_lo = c->map_v_hi - 0.5f;
    }
    c->map_p_lo_kpa = clampf(c->map_p_lo_kpa, 0.0f, 2000.0f);
    c->map_p_hi_kpa = clampf(c->map_p_hi_kpa, 50.0f, 2000.0f);
    c->map_div      = clampf(c->map_div, 1.0f, 20.0f);
    c->supply_div   = clampf(c->supply_div, 1.0f, 20.0f);
    c->ignition_div = clampf(c->ignition_div, 1.0f, 30.0f);
    c->baro_kpa     = clampf(c->baro_kpa, ATMOSPHERE_MIN_KPA, ATMOSPHERE_MAX_KPA);
    c->egt_offset_c = clampf(c->egt_offset_c, -50.0f, 50.0f);
    c->boost_tau_ms = clampf(c->boost_tau_ms, 0.0f, 2000.0f);
    c->egt_tau_ms   = clampf(c->egt_tau_ms, 0.0f, 5000.0f);
}

/* ---------------------------------------------------------------- state ----------- */

/*
 * Publication slot. The writer only ever fills the slot readers are *not* directed to, then
 * flips `latest`; each slot's sequence number catches the rare reader that was preempted long
 * enough for the writer to lap it. Neither side ever waits on the other.
 */
typedef struct {
    atomic_uint           seq;
    sensor_hub_snapshot_t snap;
} slot_t;

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t ads;
    i2c_master_dev_handle_t tmp;
    int                     alert_gpio;
    SemaphoreHandle_t       rdy;
    bool                    use_alert;
    int                     alert_misses;

    portMUX_TYPE     cal_lock;
    sensor_hub_cal_t cal;        /* guarded by cal_lock */
    float            baro_active; /* guarded by cal_lock */
    bool             auto_zeroed; /* guarded by cal_lock */
    bool             cal_dirty;   /* UI task only */
    bool             cal_loaded;  /* app_main task, before the sensor task exists */

    slot_t      slots[2];
    atomic_uint latest;
    atomic_bool started;
    atomic_bool seen; /* a board has answered since startup */
} s = {
    .cal_lock = portMUX_INITIALIZER_UNLOCKED,
};

/* ---------------------------------------------------------------- I2C helpers ----- */

static esp_err_t reg_write16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)val};
    return i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *val)
{
    uint8_t   buf[2];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *val = (uint16_t)((buf[0] << 8) | buf[1]);
    }
    return err;
}

static void IRAM_ATTR alert_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s.rdy, &woken);
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ---------------------------------------------------------------- ADS1115 --------- */

static esp_err_t ads_init(void)
{
    /* Conversion-ready mode for ALERT/RDY: Hi_thresh MSB 1, Lo_thresh MSB 0 (datasheet 7.3.8). */
    ESP_RETURN_ON_ERROR(reg_write16(s.ads, ADS_REG_HI_THRESH, 0x8000), TAG, "Hi_thresh");
    ESP_RETURN_ON_ERROR(reg_write16(s.ads, ADS_REG_LO_THRESH, 0x0000), TAG, "Lo_thresh");
    s.use_alert    = (s.rdy != NULL);
    s.alert_misses = 0;
    return ESP_OK;
}

/*
 * One single-shot conversion. Waits on ALERT/RDY falling when it is working, otherwise sleeps
 * the nominal conversion time and polls the OS bit.
 */
static esp_err_t ads_convert(int ain, int pga, int dr, float sps, int16_t *out)
{
    uint16_t cfg = ADS_CFG_OS_START | ADS_CFG_MUX_AIN(ain) | ADS_CFG_PGA(pga) |
                   ADS_CFG_SINGLESHOT | ADS_CFG_DR(dr) |
                   (s.use_alert ? ADS_CFG_QUE_1 : ADS_CFG_QUE_OFF);

    /* The internal oscillator may run 10% slow (datasheet: data rate variation). */
    uint32_t conv_ms = (uint32_t)ceilf(1000.0f / sps * 1.1f) + 1;

    if (s.use_alert) {
        xSemaphoreTake(s.rdy, 0); /* drop an edge left over from before */
    }

    ESP_RETURN_ON_ERROR(reg_write16(s.ads, ADS_REG_CONFIG, cfg), TAG, "start AIN%d", ain);

    bool ready = false;
    if (s.use_alert) {
        ready = (xSemaphoreTake(s.rdy, pdMS_TO_TICKS(conv_ms + 2)) == pdTRUE);
        if (ready) {
            s.alert_misses = 0;
        } else if (++s.alert_misses >= 3) {
            /* Pull-up missing or the line not wired: carry on without it rather than stop. */
            ESP_LOGW(TAG, "no conversion-ready edges on GPIO%d; polling the ADC instead",
                     s.alert_gpio);
            s.use_alert = false;
        }
    } else {
        vTaskDelay(pdMS_TO_TICKS(conv_ms));
    }

    for (int i = 0; !ready && i < 5; i++) {
        uint16_t status;
        ESP_RETURN_ON_ERROR(reg_read16(s.ads, ADS_REG_CONFIG, &status), TAG, "status");
        ready = (status & ADS_CFG_OS_START) != 0; /* reads 1 when not converting */
        if (!ready) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    ESP_RETURN_ON_FALSE(ready, ESP_ERR_TIMEOUT, TAG, "AIN%d never finished", ain);

    uint16_t raw;
    ESP_RETURN_ON_ERROR(reg_read16(s.ads, ADS_REG_CONVERSION, &raw), TAG, "read AIN%d", ain);
    *out = (int16_t)raw;
    return ESP_OK;
}

/* ---------------------------------------------------------------- publication ----- */

static void publish(const sensor_hub_snapshot_t *snap)
{
    unsigned idx  = atomic_load_explicit(&s.latest, memory_order_relaxed) ^ 1u;
    slot_t  *slot = &s.slots[idx];

    unsigned seq = atomic_load_explicit(&slot->seq, memory_order_relaxed);
    atomic_store_explicit(&slot->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    slot->snap = *snap;
    atomic_store_explicit(&slot->seq, seq + 2, memory_order_release);

    atomic_store_explicit(&s.latest, idx, memory_order_release);
}

void sensor_hub_get_snapshot(sensor_hub_snapshot_t *out)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        unsigned idx  = atomic_load_explicit(&s.latest, memory_order_acquire);
        slot_t  *slot = &s.slots[idx];

        unsigned before = atomic_load_explicit(&slot->seq, memory_order_acquire);
        if (before & 1u) {
            continue;
        }
        *out = slot->snap;
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&slot->seq, memory_order_relaxed) == before) {
            return;
        }
    }
    /* Lapped eight times in a row: report nothing rather than a torn copy. */
    memset(out, 0, sizeof(*out));
}

/* ---------------------------------------------------------------- task ------------ */

typedef struct {
    bool    ads_ok;
    bool    tmp_ok;
    int     ads_fails;
    int     tmp_fails;
    int64_t next_probe_us;

    bool  supply_seen;
    float supply_v;
    bool  ignition_seen;
    float ignition_v;
    bool  map_seen;
    float map_kpa;
    float map_slow_kpa;
    int   map_samples;
    bool  auto_zero_done;
    bool  egt_seen;
    float egt_c;
    bool  cj_valid;
    float cj_c;
    int64_t cj_us;
} task_state_t;

static void note_ads_result(task_state_t *t, sensor_hub_snapshot_t *snap, esp_err_t err)
{
    if (err == ESP_OK) {
        t->ads_fails = 0;
        return;
    }
    snap->i2c_errors++;
    if (++t->ads_fails >= LOST_AFTER_FAILS && t->ads_ok) {
        ESP_LOGW(TAG, "ADS1115 stopped answering (%s)", esp_err_to_name(err));
        t->ads_ok = false;
        i2c_master_bus_reset(s.bus);
    }
}

static void note_tmp_result(task_state_t *t, sensor_hub_snapshot_t *snap, esp_err_t err)
{
    if (err == ESP_OK) {
        t->tmp_fails = 0;
        return;
    }
    snap->i2c_errors++;
    if (++t->tmp_fails >= LOST_AFTER_FAILS && t->tmp_ok) {
        ESP_LOGW(TAG, "TMP1075 stopped answering (%s)", esp_err_to_name(err));
        t->tmp_ok = false;
    }
}

/* Look for whichever device is missing. Cheap: two address-only transactions. */
static void probe(task_state_t *t)
{
    if (!t->ads_ok && i2c_master_probe(s.bus, ADS1115_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
        if (ads_init() == ESP_OK) {
            t->ads_ok    = true;
            t->ads_fails = 0;
            ESP_LOGI(TAG, "ADS1115 found at 0x%02x", ADS1115_ADDR);
        }
    }

    if (!t->tmp_ok && i2c_master_probe(s.bus, TMP1075_ADDR, I2C_TIMEOUT_MS) == ESP_OK) {
        uint16_t id = 0;
        if (reg_read16(s.tmp, TMP_REG_DIEID, &id) == ESP_OK) {
            if (id != TMP_DIEID) {
                /* A TMP1075N has no ID register; anything else at 0x49 is a wiring surprise. */
                ESP_LOGW(TAG, "device at 0x%02x has ID 0x%04x, expected 0x%04x; using it anyway",
                         TMP1075_ADDR, id, TMP_DIEID);
            }
            t->tmp_ok    = true;
            t->tmp_fails = 0;
            ESP_LOGI(TAG, "TMP1075 found at 0x%02x", TMP1075_ADDR);
        }
    }

    if ((t->ads_ok || t->tmp_ok) && !atomic_load(&s.seen)) {
        atomic_store(&s.seen, true);
    }
}

static void sample_map(task_state_t *t, sensor_hub_snapshot_t *snap, const sensor_hub_cal_t *cal,
                       float baro, int64_t now)
{
    int16_t   counts;
    esp_err_t err = ads_convert(0, ADS_PGA_4V096, ADS_DR_475, 475.0f, &counts);
    note_ads_result(t, snap, err);
    if (err != ESP_OK) {
        return;
    }
    snap->samples++;

    float v_sensor     = sensor_math_ads1115_volts(counts, 4.096f) * cal->map_div;
    snap->map_sensor_v = v_sensor;

    const char *fault = NULL;
    float       v_ref = v_sensor;

    if (cal->ratiometric) {
        if (!t->supply_seen) {
            fault = "Waiting for supply reading";
        } else if (t->supply_v < SUPPLY_MIN_V || t->supply_v > SUPPLY_MAX_V) {
            fault = "Sensor supply out of range";
        } else {
            /* Refer the output to a 5.00V supply: the sensor's output scales with its supply. */
            v_ref = v_sensor * 5.0f / t->supply_v;
        }
    }
    if (v_sensor < MAP_SIGNAL_MIN_V) {
        fault = "No MAP signal";
    } else if (v_sensor > MAP_SIGNAL_MAX_V) {
        fault = "MAP signal too high";
    }

    snap->boost_fault = fault;
    if (fault != NULL) {
        snap->ch[SENSOR_HUB_CH_MAP].valid   = false;
        snap->ch[SENSOR_HUB_CH_BOOST].valid = false;
        snap->ch[SENSOR_HUB_CH_MAP].timestamp_us   = now;
        snap->ch[SENSOR_HUB_CH_BOOST].timestamp_us = now;
        /* Restart the filters too: the auto-zero must not average in a reading from before. */
        t->map_samples = 0;
        t->map_seen    = false;
        return;
    }

    float p_abs = sensor_math_linear(v_ref, cal->map_v_lo, cal->map_p_lo_kpa, cal->map_v_hi,
                                     cal->map_p_hi_kpa);
    if (!t->map_seen) {
        t->map_kpa      = p_abs;
        t->map_slow_kpa = p_abs;
        t->map_seen     = true;
    } else {
        t->map_kpa      = sensor_math_lowpass(t->map_kpa, p_abs, TICK_MS, cal->boost_tau_ms);
        t->map_slow_kpa = sensor_math_lowpass(t->map_slow_kpa, p_abs, TICK_MS, MAP_SLOW_TAU_MS);
    }
    t->map_samples++;
    snap->map_slow_kpa = t->map_slow_kpa;

    /*
     * Key-on zero, as the JRP gauge does it (docs/sensor-frontend.md). Only when the reading looks
     * like atmosphere: a restart under boost keeps the stored zero instead of adopting a bad one.
     */
    if (!t->auto_zero_done && t->map_samples >= AUTO_ZERO_SAMPLES) {
        t->auto_zero_done = true;
        if (cal->auto_zero) {
            if (t->map_slow_kpa >= ATMOSPHERE_MIN_KPA && t->map_slow_kpa <= ATMOSPHERE_MAX_KPA) {
                taskENTER_CRITICAL(&s.cal_lock);
                s.baro_active = t->map_slow_kpa;
                s.auto_zeroed = true;
                taskEXIT_CRITICAL(&s.cal_lock);
                baro = t->map_slow_kpa;
                ESP_LOGI(TAG, "auto-zero: atmosphere %.1f kPa", (double)baro);
            } else {
                ESP_LOGW(TAG, "auto-zero skipped: MAP %.1f kPa is not atmospheric; keeping %.1f kPa",
                         (double)t->map_slow_kpa, (double)baro);
            }
        }
    }

    snap->ch[SENSOR_HUB_CH_MAP]   = (sensor_hub_reading_t){t->map_kpa, true, now};
    snap->ch[SENSOR_HUB_CH_BOOST] = (sensor_hub_reading_t){t->map_kpa - baro, true, now};
}

static void sample_supply(task_state_t *t, sensor_hub_snapshot_t *snap, const sensor_hub_cal_t *cal,
                          int64_t now)
{
    int16_t   counts;
    esp_err_t err = ads_convert(1, ADS_PGA_4V096, ADS_DR_475, 475.0f, &counts);
    note_ads_result(t, snap, err);
    if (err != ESP_OK) {
        return;
    }
    float v = sensor_math_ads1115_volts(counts, 4.096f) * cal->supply_div;
    t->supply_v    = t->supply_seen ? sensor_math_lowpass(t->supply_v, v, TICK_MS * SLOT_COUNT,
                                                          SUPPLY_TAU_MS)
                                    : v;
    t->supply_seen = true;
    snap->ch[SENSOR_HUB_CH_SENSOR_SUPPLY] = (sensor_hub_reading_t){t->supply_v, true, now};
}

static void sample_ignition(task_state_t *t, sensor_hub_snapshot_t *snap,
                            const sensor_hub_cal_t *cal, int64_t now)
{
    int16_t   counts;
    esp_err_t err = ads_convert(2, ADS_PGA_4V096, ADS_DR_475, 475.0f, &counts);
    note_ads_result(t, snap, err);
    if (err != ESP_OK) {
        return;
    }
    float v = sensor_math_ads1115_volts(counts, 4.096f) * cal->ignition_div;
    t->ignition_v    = t->ignition_seen ? sensor_math_lowpass(t->ignition_v, v,
                                                              TICK_MS * SLOT_COUNT, IGNITION_TAU_MS)
                                        : v;
    t->ignition_seen = true;
    snap->ch[SENSOR_HUB_CH_IGNITION] = (sensor_hub_reading_t){t->ignition_v, true, now};
}

static void sample_egt(task_state_t *t, sensor_hub_snapshot_t *snap, const sensor_hub_cal_t *cal,
                       int64_t now)
{
    int16_t   counts;
    esp_err_t err = ads_convert(3, ADS_PGA_0V256, ADS_DR_128, 128.0f, &counts);
    note_ads_result(t, snap, err);
    if (err != ESP_OK) {
        return;
    }

    float tc_v   = sensor_math_ads1115_volts(counts, 0.256f);
    snap->tc_uv  = tc_v * 1e6f;

    const char *fault = NULL;
    float       egt   = 0.0f;

    if (tc_v > TC_OPEN_V) {
        fault = "Thermocouple open";
    } else if (!t->cj_valid || now - t->cj_us > STALE_SLOW_US) {
        fault = "No cold-junction reading";
    } else if (!sensor_math_egt_celsius(tc_v, t->cj_c, &egt)) {
        fault = "EGT out of range";
    } else {
        egt += cal->egt_offset_c;
        if (egt < EGT_MIN_C || egt > EGT_MAX_C) {
            fault = "EGT out of range";
        }
    }

    snap->egt_fault = fault;
    if (fault != NULL) {
        t->egt_seen = false;
        snap->ch[SENSOR_HUB_CH_EGT] = (sensor_hub_reading_t){0.0f, false, now};
        return;
    }

    t->egt_c    = t->egt_seen ? sensor_math_lowpass(t->egt_c, egt, TICK_MS * SLOT_COUNT,
                                                     cal->egt_tau_ms)
                              : egt;
    t->egt_seen = true;
    snap->ch[SENSOR_HUB_CH_EGT] = (sensor_hub_reading_t){t->egt_c, true, now};
}

static void sample_cold_junction(task_state_t *t, sensor_hub_snapshot_t *snap, int64_t now)
{
    uint16_t  raw;
    esp_err_t err = reg_read16(s.tmp, TMP_REG_TEMP, &raw);
    note_tmp_result(t, snap, err);
    if (err != ESP_OK) {
        return;
    }
    float c     = sensor_math_tmp1075_celsius(raw);
    t->cj_valid = (c >= CJ_MIN_C && c <= CJ_MAX_C);
    t->cj_c     = c;
    t->cj_us    = now;
    snap->ch[SENSOR_HUB_CH_COLD_JUNCTION] = (sensor_hub_reading_t){c, t->cj_valid, now};
}

/* Channels a missing device feeds are invalid, stamped now so the reason shows immediately. */
static void invalidate_missing(const task_state_t *t, sensor_hub_snapshot_t *snap, int64_t now)
{
    if (!t->ads_ok) {
        static const sensor_hub_channel_t ads_channels[] = {
            SENSOR_HUB_CH_BOOST, SENSOR_HUB_CH_MAP, SENSOR_HUB_CH_EGT, SENSOR_HUB_CH_IGNITION,
            SENSOR_HUB_CH_SENSOR_SUPPLY,
        };
        for (size_t i = 0; i < sizeof(ads_channels) / sizeof(ads_channels[0]); i++) {
            snap->ch[ads_channels[i]] = (sensor_hub_reading_t){0.0f, false, now};
        }
        snap->boost_fault = "ADC not responding";
        snap->egt_fault   = "ADC not responding";
    }
    if (!t->tmp_ok) {
        snap->ch[SENSOR_HUB_CH_COLD_JUNCTION] = (sensor_hub_reading_t){0.0f, false, now};
        if (t->ads_ok) {
            snap->ch[SENSOR_HUB_CH_EGT] = (sensor_hub_reading_t){0.0f, false, now};
            snap->egt_fault             = "Cold-junction sensor missing";
        }
    }
}

static void sensor_task(void *arg)
{
    (void)arg;

    task_state_t          t    = {0};
    sensor_hub_snapshot_t snap = {0};
    TickType_t            last = xTaskGetTickCount();
    uint32_t              tick = 0;
    bool                  stack_reported = false;

    for (;;) {
        int64_t now = esp_timer_get_time();

        if ((!t.ads_ok || !t.tmp_ok) && now >= t.next_probe_us) {
            probe(&t);
            t.next_probe_us = now + (int64_t)PROBE_EVERY_MS * 1000;
        }

        sensor_hub_cal_t cal;
        float            baro;
        taskENTER_CRITICAL(&s.cal_lock);
        cal  = s.cal;
        baro = s.baro_active;
        taskEXIT_CRITICAL(&s.cal_lock);

        if (t.ads_ok) {
            sample_map(&t, &snap, &cal, baro, now);

            switch (tick % SLOT_COUNT) {
            case SLOT_EGT:      sample_egt(&t, &snap, &cal, now); break;
            case SLOT_SUPPLY:   sample_supply(&t, &snap, &cal, now); break;
            case SLOT_IGNITION: sample_ignition(&t, &snap, &cal, now); break;
            default: break;
            }
        }
        if (t.tmp_ok && tick % TMP_EVERY_TICKS == 8) {
            sample_cold_junction(&t, &snap, now);
        }

        invalidate_missing(&t, &snap, now);

        snap.ads1115_ok = t.ads_ok;
        snap.tmp1075_ok = t.tmp_ok;
        snap.alert_ok   = t.ads_ok && s.use_alert;
        snap.state      = (t.ads_ok || t.tmp_ok) ? SENSOR_HUB_ONLINE
                        : atomic_load(&s.seen)   ? SENSOR_HUB_LOST
                                                 : SENSOR_HUB_SEARCHING;
        taskENTER_CRITICAL(&s.cal_lock);
        snap.baro_kpa    = s.baro_active;
        snap.auto_zeroed = s.auto_zeroed;
        taskEXIT_CRITICAL(&s.cal_lock);

        publish(&snap);

        /* Once, well after startup: the number to size the stack from (docs/performance.md). */
        if (!stack_reported && tick == 30 * 1000 / TICK_MS) {
            stack_reported = true;
            ESP_LOGI(TAG, "stack high-water mark: %u B free",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

        tick++;
        /* If a slow slot overran the tick, carry on from now rather than bursting to catch up. */
        if (xTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS)) == pdFALSE) {
            last = xTaskGetTickCount();
        }
    }
}

/* ---------------------------------------------------------------- NVS ------------- */

/*
 * Loaded on first use rather than in sensor_hub_start(), because the settings page is built, and
 * reads the calibration, before the sensors start.
 */
static void cal_load(void)
{
    if (s.cal_loaded) {
        return;
    }
    s.cal_loaded = true;
    sensor_hub_cal_defaults(&s.cal);
    s.baro_active = s.cal.baro_kpa;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored calibration; using defaults");
        return;
    }

    uint8_t          ver    = 0;
    sensor_hub_cal_t stored;
    size_t           len    = sizeof(stored);
    if (nvs_get_u8(h, NVS_KEY_VER, &ver) == ESP_OK && ver == CAL_VERSION &&
        nvs_get_blob(h, NVS_KEY_CAL, &stored, &len) == ESP_OK && len == sizeof(stored)) {
        cal_sanitise(&stored);
        s.cal         = stored;
        s.baro_active = stored.baro_kpa;
        ESP_LOGI(TAG, "calibration loaded");
    } else {
        ESP_LOGW(TAG, "stored calibration unreadable or from another version; using defaults");
    }
    nvs_close(h);
}

esp_err_t sensor_hub_save_cal(void)
{
    if (!s.cal_dirty) {
        return ESP_OK;
    }

    sensor_hub_cal_t cal;
    sensor_hub_get_cal(&cal);

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");

    err = nvs_set_u8(h, NVS_KEY_VER, CAL_VERSION);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY_CAL, &cal, sizeof(cal));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        s.cal_dirty = false;
        ESP_LOGI(TAG, "calibration saved");
    } else {
        ESP_LOGE(TAG, "calibration save failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---------------------------------------------------------------- public ---------- */

esp_err_t sensor_hub_start(const sensor_hub_bus_t *bus)
{
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_ARG, TAG, "no bus");
    ESP_RETURN_ON_FALSE(!atomic_load(&s.started), ESP_ERR_INVALID_STATE, TAG, "already started");

    cal_load();

    sensor_hub_snapshot_t empty = {.state = SENSOR_HUB_SEARCHING};
    publish(&empty);

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = bus->i2c_port,
        .sda_io_num        = bus->sda_gpio,
        .scl_io_num        = bus->scl_gpio,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /*
         * The board has 4.7k pull-ups (R14, R15). The internal ones are for when it is unplugged:
         * they hold the idle bus high, so a probe gets a clean NACK instead of floating lines.
         */
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s.bus), TAG, "sensor bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADS1115_ADDR,
        .scl_speed_hz    = bus->scl_hz,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s.bus, &dev_cfg, &s.ads), TAG, "ADS1115");
    dev_cfg.device_address = TMP1075_ADDR;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s.bus, &dev_cfg, &s.tmp), TAG, "TMP1075");

    s.alert_gpio = bus->alert_gpio;
    if (bus->alert_gpio >= 0) {
        s.rdy = xSemaphoreCreateBinary();
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << bus->alert_gpio,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE, /* R16 on the board; this covers it unplugged */
            .intr_type    = GPIO_INTR_NEGEDGE,  /* COMP_POL = 0: RDY asserts low */
        };
        esp_err_t err = (s.rdy != NULL) ? gpio_config(&io) : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            /*
             * The touch driver normally installed the ISR service already. Install it only if
             * adding the handler says it is missing: installing twice logs an error.
             */
            err = gpio_isr_handler_add(bus->alert_gpio, alert_isr, NULL);
            if (err == ESP_ERR_INVALID_STATE) {
                err = gpio_install_isr_service(0);
                if (err == ESP_OK) {
                    err = gpio_isr_handler_add(bus->alert_gpio, alert_isr, NULL);
                }
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ALERT/RDY interrupt unavailable (%s); polling instead",
                     esp_err_to_name(err));
            if (s.rdy != NULL) {
                vSemaphoreDelete(s.rdy);
                s.rdy = NULL;
            }
        }
    }

    /*
     * Stack in PSRAM: internal RAM is the binding constraint once WiFi runs
     * (docs/performance.md). The consequence is that this task must never write flash, which is
     * why calibration is saved from the caller's task.
     */
    TaskHandle_t task = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(sensor_task, "sensor_hub", 4096, NULL, 5, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the sensor task");
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s.started, true);
    ESP_LOGI(TAG, "sensor bus on I2C%d: SDA GPIO%d, SCL GPIO%d, ALERT GPIO%d, %" PRIu32 " Hz",
             bus->i2c_port, bus->sda_gpio, bus->scl_gpio, bus->alert_gpio, bus->scl_hz);
    return ESP_OK;
}

static const char *const k_channel_names[SENSOR_HUB_CH_COUNT] = {
    [SENSOR_HUB_CH_BOOST]         = "boost",
    [SENSOR_HUB_CH_MAP]           = "map",
    [SENSOR_HUB_CH_EGT]           = "egt",
    [SENSOR_HUB_CH_IGNITION]      = "ignition",
    [SENSOR_HUB_CH_SENSOR_SUPPLY] = "sensor_supply",
    [SENSOR_HUB_CH_COLD_JUNCTION] = "cold_junction",
};

sensor_hub_channel_t sensor_hub_channel_from_name(const char *name)
{
    if (name != NULL) {
        for (int i = 0; i < SENSOR_HUB_CH_COUNT; i++) {
            if (strcmp(name, k_channel_names[i]) == 0) {
                return (sensor_hub_channel_t)i;
            }
        }
    }
    return SENSOR_HUB_CH_COUNT;
}

sensor_math_qty_t sensor_hub_channel_qty(sensor_hub_channel_t ch)
{
    switch (ch) {
    case SENSOR_HUB_CH_BOOST:
    case SENSOR_HUB_CH_MAP:
        return SENSOR_MATH_QTY_PRESSURE_KPA;
    case SENSOR_HUB_CH_EGT:
    case SENSOR_HUB_CH_COLD_JUNCTION:
        return SENSOR_MATH_QTY_TEMP_C;
    default:
        return SENSOR_MATH_QTY_VOLTS;
    }
}

bool sensor_hub_read(const char *channel, const char *unit, float *value, bool *valid)
{
    sensor_hub_channel_t ch = sensor_hub_channel_from_name(channel);
    if (ch == SENSOR_HUB_CH_COUNT || !atomic_load(&s.started) || !atomic_load(&s.seen)) {
        return false;
    }

    /* A whole-snapshot copy is a couple of hundred bytes: cheap enough every UI tick. */
    sensor_hub_snapshot_t snap;
    sensor_hub_get_snapshot(&snap);
    const sensor_hub_reading_t *r = &snap.ch[ch];

    int64_t limit = (ch == SENSOR_HUB_CH_BOOST || ch == SENSOR_HUB_CH_MAP) ? STALE_FAST_US
                                                                           : STALE_SLOW_US;
    bool fresh = r->timestamp_us != 0 && esp_timer_get_time() - r->timestamp_us <= limit;

    *valid = r->valid && fresh && snap.state == SENSOR_HUB_ONLINE;
    sensor_math_to_unit(sensor_hub_channel_qty(ch), r->value, unit, value);
    return true;
}

void sensor_hub_get_cal(sensor_hub_cal_t *out)
{
    cal_load();
    taskENTER_CRITICAL(&s.cal_lock);
    *out = s.cal;
    taskEXIT_CRITICAL(&s.cal_lock);
}

void sensor_hub_set_cal(const sensor_hub_cal_t *cal)
{
    cal_load();
    sensor_hub_cal_t c = *cal;
    cal_sanitise(&c);

    taskENTER_CRITICAL(&s.cal_lock);
    bool changed = memcmp(&c, &s.cal, sizeof(c)) != 0;
    /* A zero typed in by hand takes over from this startup's auto-zero. */
    if (c.baro_kpa != s.cal.baro_kpa) {
        s.baro_active = c.baro_kpa;
        s.auto_zeroed = false;
    }
    s.cal = c;
    taskEXIT_CRITICAL(&s.cal_lock);

    if (changed) {
        s.cal_dirty = true;
    }
}

esp_err_t sensor_hub_zero_boost(char *msg, size_t msg_len)
{
    sensor_hub_snapshot_t snap;
    sensor_hub_get_snapshot(&snap);

    const sensor_hub_reading_t *map = &snap.ch[SENSOR_HUB_CH_MAP];
    bool fresh = map->timestamp_us != 0 && esp_timer_get_time() - map->timestamp_us <= STALE_FAST_US;

    if (snap.state != SENSOR_HUB_ONLINE || !map->valid || !fresh) {
        snprintf(msg, msg_len, "Not zeroed: %s", snap.boost_fault ? snap.boost_fault
                                                                  : "no MAP reading");
        return ESP_ERR_INVALID_STATE;
    }

    float kpa = snap.map_slow_kpa;
    if (kpa < ATMOSPHERE_MIN_KPA || kpa > ATMOSPHERE_MAX_KPA) {
        snprintf(msg, msg_len, "Not zeroed: %.0f kPa is not atmospheric. Engine off?", (double)kpa);
        return ESP_ERR_INVALID_STATE;
    }

    sensor_hub_cal_t cal;
    sensor_hub_get_cal(&cal);
    cal.baro_kpa = kpa;
    sensor_hub_set_cal(&cal);

    /* set_cal only hands over when the value changed; make sure this zero is the one in use. */
    taskENTER_CRITICAL(&s.cal_lock);
    s.baro_active = s.cal.baro_kpa;
    s.auto_zeroed = false;
    taskEXIT_CRITICAL(&s.cal_lock);

    esp_err_t err = sensor_hub_save_cal();
    snprintf(msg, msg_len, "Zeroed at %.1f kPa%s", (double)kpa,
             err == ESP_OK ? "" : " (not saved)");
    return ESP_OK;
}

const char *sensor_hub_state_str(sensor_hub_state_t state)
{
    switch (state) {
    case SENSOR_HUB_SEARCHING: return "not detected";
    case SENSOR_HUB_ONLINE:    return "connected";
    case SENSOR_HUB_LOST:      return "LOST";
    case SENSOR_HUB_OFF:
    default:                   return "off";
    }
}
