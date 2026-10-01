# ADR 0009 — sensor_hub: sampling, conversion and calibration

**Status:** Accepted. Built and running on the display board. **Not yet run against the sensor
board**, which is at fabrication.

## Context

The rear sensor board ([rear-pcb.md](../rear-pcb.md), fabricated from
`pcb/map-and-egt-daughterboard/`) puts one ADS1115 (0x48) and one TMP1075 (0x49 by design, 0x4F as assembled) on the header's
I2C pins. The ADC's four inputs carry MAP ÷ 2, the sensor's 5V supply ÷ 2, ignition ÷ 6 and the
raw thermocouple. The firmware has to turn that into `boost` and `egt` channels without disturbing
a render loop that already has almost no internal RAM to spare.

## Decisions

1. **I2C port 0, not 1.** The BSP's own bus (touch, codec, PMIC) is on `I2C_NUM_1`, the BSP's
   `CONFIG_BSP_I2C_NUM` default. The docs had assumed the opposite. Measured: asking for port 1
   failed with "bus already acquired", and the failed attempt broke the codec's bus as it cleaned
   up. The two controllers are identical, so the sensor bus moved rather than the BSP.
2. **Single-shot conversions, multiplexed on a 10 ms tick.** MAP converts every tick (~100 Hz) at
   475 SPS. One slower input follows it in rotation: thermocouple, supply and ignition each at
   10 Hz. The thermocouple converts at 128 SPS on the ±0.256V range, the quietest setting, because
   a probe takes about a second to respond anyway. Continuous mode was rejected because every
   change of input needs a new configuration anyway.
3. **ALERT/RDY as a conversion-ready interrupt, with polling as the fallback.** It is wired, so it
   is used: no bus traffic while waiting. After three missed edges the driver polls the OS bit
   instead and says so on the settings page, so a bad pull-up degrades rather than stops.
4. **Boost is ratiometric by default.** The MAP output is referred to the measured AIN1 supply,
   which cancels buck tolerance and current-limiter drop. It can be switched off in settings.
5. **Type K inversion is numerical.** The EGT is the NIST ITS-90 *reference* function (both
   ranges, including the exponential term), inverted by Newton's method. That avoids a second set
   of inverse-polynomial coefficients, which could not be checked against a primary source here.
   The result is as accurate as the reference function, and host tests hold it to the NIST table.
6. **The zero is captured at key-on only if MAP reads as atmosphere** (60–110 kPa). A restart
   under boost keeps the stored zero rather than adopting a wrong one. A manual "Zero boost now"
   applies the same check and is saved.
7. **Calibration belongs to `sensor_hub`, in its own NVS namespace** (`sensor_cal`, one versioned
   blob). `sensor_hub` stays independent of `app_settings`, and a new sensor is a settings change,
   not a recompile.
8. **The settings page edits calibration through one picker and one −/+ pair**, not a row per
   parameter. A row per parameter was built first and measured at **24.4 KB of internal RAM**
   (LVGL widgets are small `malloc`s, and anything under 4 KB goes to internal RAM). That stopped
   WiFi starting. The single editor costs 4.3 KB.
9. **The task stack is in PSRAM**, so the task must never write flash. Calibration is saved from
   the UI task.
10. **The simulated sweep runs only until a board has answered.** After that a fault shows as
    dashes until restart and is never covered by the simulator. Telemetry, while it keeps
    arriving, still takes priority over both.

## Consequences

- Startup internal-RAM cost: 1.5 KB for the driver and 4.3 KB for the settings section. It fits
  only because audio was removed in the same change ([ADR 0004](0004-retain-sd-and-audio.md)).
  About 3.5 KB of internal RAM is left after WiFi starts.
- Removing audio also removed the ~900 ms that audio setup had spent before WiFi started. WiFi
  then failed intermittently while the first frames were still rendering. It now waits 1 s after
  the UI comes up; 6 of 6 boots got all 6 RX buffers.
- The thresholds in `sensor_hub.c` (open probe above 60 mV, MAP signal outside 0.25–4.85 V,
  supply outside 4.5–5.5 V) are engineering choices. They are unverified until the board exists.
