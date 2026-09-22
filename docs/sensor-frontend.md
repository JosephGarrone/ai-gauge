# Sensor Front-End

Re-uses the vehicle's existing boost and EGT sensors. Because no internal ADC is usable (see
[hardware-reference.md](hardware-reference.md) and
[ADR 0001](adr/0001-external-i2c-sensor-frontend.md)), both sensors are read over a
**second I2C bus** brought out on the 8-pin expansion header.

> **Status:** the board has been designed and sent for fabrication
> (`pcb/map-and-egt-daughterboard/`). The firmware (`sensor_hub`, see *Firmware* below) is built
> and running on the display board, but **has never talked to a sensor board**. Record measured
> results in the *Verification* section as they happen, and do not describe an unbuilt circuit as
> working.

## Bus

| Signal | GPIO | Notes |
|---|---|---|
| SDA | 16 | Header pin 6 |
| SCL | 17 | Header pin 7 |
| ALERT / DRDY | 18 | Header pin 8, open-drain, ADS1115 only, pulled up |
| 3V3 | — | Header pin 3, logic supply |
| 5V | — | Header pin 1 (VBUS) — **see the power warning below** |
| GND | — | Header pin 2 |

`I2C_NUM_0` at **100kHz** (the BSP's own bus is `I2C_NUM_1`). Both devices support 400kHz, but the bus runs off-board through the
header and the traffic is tiny, so the slower, more forgiving speed is kept. Separate from the
system bus (GPIO14/15) so sensor polling never contends with touch, and a wiring fault on the
harness cannot take down the touchscreen.

| Address | Device | Channel |
|---|---|---|
| 0x48 | ADS1115 | `boost`, and the raw thermocouple voltage for `egt` (AIN3) |
| 0x49 | TMP1075 | Cold-junction temperature for `egt` |

Both are address-strappable if they clash with something added later.

## Boost — 0–60 PSI MAP sensor via ADS1115

**Sensor:** the **JRP 7-bar, 0-5V, 3-wire filtered MAP sensor** supplied with the
[JRP GearX 52mm 0-60 PSI diesel boost gauge kit](https://www.justraceparts.com.au/digital-diesel-boost-gauge-kit-0-60-psi-52mm)
(product code `GearX-52mm-Diesel-Boost-60`). Connectors are Just Race Parts, matching the existing
gauges.

What the listing confirms: 0-5V output, three wires (5V supply, ground, signal), sold as a 7-bar
sensor for a gauge that displays 0-60 PSI, and the JRP gauge offers a **boost sensor calibration**
feature. A manual exists (`JRP-GearX-Oil-Pressure-Volts-Boost(30INHG-30PSI)-(0-30PSI)-(0-60PSI).pdf`,
linked from the product page) but could not be machine-read here.

> **Open items — verify before wiring anything:**
> - Pinout and wire colours (in the manual).
> - The transfer function. "0-5V" in a listing often means the 0.5-4.5V ratiometric convention;
>   the scaling below assumes that, and it must be confirmed by measurement.
> - Absolute versus gauge pressure: see below.

**ADC:** ADS1115 — 16-bit, differential, programmable gain, internal reference, 250 SPS.

### The divider is mandatory

The sensor output can reach **5V**. ADS1115 inputs must not exceed `VDD + 0.3V`, and on a 3.3V
rail that is 3.6V. Connecting the sensor directly will damage the ADC.

A 2:1 divider brings a 5V full scale to 2.5V, comfortably inside the ±4.096V PGA range:

```
MAP out ──┬── R1 (10k, 1%) ──┬── ADS1115 AIN0
          │                  │
          │                 R2 (10k, 1%)
          │                  │
         GND ────────────────┴── ADS1115 GND  (star ground at the sensor board)
```

Use 1% or better resistors — divider tolerance is a direct gain error on the reading. The
resulting source impedance (5k) is well within what the ADS1115 sampling capacitor tolerates
at 250 SPS.

### Scaling

With `PGA = ±4.096V`, the ADS1115 gives `4.096V / 32768 = 125µV` per count.

```
V_adc    = counts × 4.096 / 32768
V_sensor = V_adc × (R1 + R2) / R2          = V_adc × 2.0
P_abs    = (V_sensor - 0.5) / 4.0 × 8.0    # bar absolute; assumed 0.5V = 0, 4.5V = 8 bar abs
PSI      = (P_abs - P_zero) × 14.5038         # P_zero captured at key-on, engine off
```

**The sensor spans 0—8 bar absolute** (−1 to +7 bar gauge, about −14.5 to +101.5 PSI). 0—60 PSI is
only the display range of the gauge face. The 0.5—4.5V endpoints are the usual convention for this
class of sensor but are still an assumption until measured.

Both the divider ratio and the sensor transfer function are **configuration, not constants** —
they live in `sensor_hub` calibration settings so a different sensor is a settings change, not
a recompile.

### Gauge vs absolute pressure

A boost gauge conventionally reads **gauge pressure** (0 at atmospheric), while a MAP sensor
measures **absolute**. If the sensor is absolute, subtract barometric pressure:

```
PSI_gauge = PSI_abs - P_baro          # ~14.7 PSI at sea level
```

Store `P_baro` as a calibration value, ideally captured at key-on with the engine off, so the
gauge zeroes correctly at altitude.

**For this sensor it is settled: it is absolute.** JRP's own page for the sensor
([JRP Boost Sensor v2 7-Bar](https://www.justraceparts.com.au/boost-map-sensor-7-bar-barb), product
code `7-BAR-MAP-BARB`) gives its calibrated range as **0—8 bar absolute**, which is −1 to
+7 bar gauge. The "7-bar" name counts only the positive boost range. At normal atmospheric
pressure it is already reading about 1 bar.

That is why the JRP gauge reads 0 at rest: its "zeroing", done with the engine off and no
pressure in the system, subtracts atmospheric pressure. The firmware does the same, capturing
`P_zero` at key-on with the engine off. That also corrects for altitude and weather, which a fixed
14.7 PSI offset would not.

Expected signal at rest, if the 0.5—4.5V convention holds: `0.5 + 4.0 × (1/8) — about 1.0V`. A
measurement confirms the transfer function.

Note that JRP's 7-bar and 3-bar sensors use different calibrations and the connectors look alike;
the firmware's scaling constants must match the sensor actually fitted.

### Sampling

250 SPS continuous-conversion mode, with DRDY on GPIO18 or simple periodic polling. A boost
gauge needs to feel instantaneous, so oversample lightly and let the needle damping in
`<source damping="...">` do the visual smoothing rather than over-filtering the raw signal.

## EGT — K-type thermocouple via ADS1115 AIN3 + TMP1075

A thermocouple produces roughly **41µV/°C** and needs cold-junction compensation (CJC). The
ADS1115's ±0.256V range resolves that directly, so the thermocouple uses the ADC's fourth
input. A TMP1075 beside the connector measures the cold junction. The firmware does the
linearisation and CJC.

This replaced an MCP9600, which did all of that in one part but cost about $10 per board.
MAX31855/MAX31856 are ruled out because they use SPI — see
[ADR 0001](adr/0001-external-i2c-sensor-frontend.md).

```
J4 T+ ──┬── R18 1M ── 3V3        (open-probe pull-up)
        ├── C14 10nF ── T−
        └── R17 1k ──┬── ADS1115 AIN3
                     └── C17 10nF ── GND
J4 T− ── GND
```

| Setting | Value |
|---|---|
| Thermocouple type | K |
| ADC input | AIN3 single-ended, PGA ±0.256V → 7.8125µV/count ≈ 0.19°C |
| Range | 0.256V is far beyond K-type's 54.9mV at 1372°C, so any real EGT fits |
| Cold junction | TMP1075 at 0x49 (A0 high): 12-bit, 0.0625°C/LSB, ±1°C max |
| Open probe | R18 pulls AIN3 to 3V3 → the reading saturates at 0x7FFF |

No clamp diode on AIN3. The other inputs have BAV99 clamps, but the upper diode's leakage into
R17 would add an error of several degrees when hot. The thermocouple can only produce millivolts,
and R17 limits the current if the lead ever touches 12V.

**Conversion** (NIST ITS-90 K-type polynomials, from
[srdata.nist.gov/its90](https://srdata.nist.gov/its90/main/)):

```
V_tc  = counts × 0.256 / 32768                  # volts at AIN3
V_cj  = E_K(T_cj)                               # reference function, T_cj from the TMP1075
T_hot = E_K⁻¹(V_tc + V_cj)                      # reference function inverted by Newton's method
```

The firmware inverts the reference function numerically instead of using NIST's inverse
polynomials ([ADR 0009](adr/0009-sensor-hub-sampling-and-calibration.md)). Host tests hold both
directions to the NIST table (`tools/host-tests/test_sensor_math.c`).

**Error budget** (roughly ±3°C in total, about what the MCP9600 offered):
- The TMP1075 is ±1°C.
- ADS1115 gain error is ±0.15% max, about 1.3°C at 900°C.
- R18's 3.3µA flows through the probe loop: about 0.4°C with a 5Ω probe.
- The ADS1115 input current through R17 adds about 0.2°C.

**Sampling:** AIN3 shares the ADS1115 with the boost channels, so conversions are multiplexed.
A thermocouple probe responds in about a second, so one AIN3 conversion every ~100ms is plenty.
Interleave it with the boost conversions. Switch the PGA to ±0.256V for AIN3 and back to
±4.096V for the others.

The single-ended input cannot read below 0V, so an exhaust colder than the connector reads as
the cold-junction temperature. That doesn't matter for EGT.

**Thermocouple wiring:** use K-type extension wire and connectors for the whole run. Any
junction with copper creates a parasitic thermocouple, and an error that CJC cannot correct.
Keep the wiring away from ignition leads.

## Power and automotive protection

> **Not yet designed.** This is a hardware requirement, recorded so it is not forgotten. The
> board that will implement it is being planned in [rear-pcb.md](rear-pcb.md).

Running from vehicle 12V is the hostile part of this project. The board's USB VBUS rail is
**not** an appropriate source for the sensor 5V supply once installed in a vehicle.

Required:

- **Reverse-polarity protection** — series P-channel MOSFET or a Schottky.
- **Load-dump and transient suppression** — automotive-rated TVS. Alternator load dump can
  reach 60V+ and will destroy an unprotected regulator.
- **12V → 5V buck** for the MAP sensor, sized for the sensor plus margin.
- **5V → 3.3V** for the ADS1115/TMP1075 logic, or run their logic from the board's 3V3.
- **Fusing** on the 12V feed.
- **Ignition-switched supply**, or a low-quiescent-current path, so the gauge cannot flatten
  the battery.
- **Star grounding** at the sensor board, with the ADS1115 ground referenced to the MAP
  sensor's ground rather than picked up elsewhere in the vehicle. Ground offsets across a
  chassis are a real and common source of gauge error.

## Fault handling

| Condition | Detection | Behaviour |
|---|---|---|
| Sensor board absent | ADS1115 and TMP1075 both NACK (re-probed every 2s) | Simulated sweep while no board has ever answered (if built in), else dashes. Settings shows "Board not detected" |
| Board lost after being seen | 5 consecutive failed transactions | All its channels invalid; bus reset; re-probed every 2s. Never falls back to the simulator |
| MAP unplugged or shorted | Sensor output below 0.25V or above 4.85V | `boost` invalid, reason on settings |
| Sensor supply collapsed | AIN1 × 2 outside 4.5–5.5V (TPS2553 limiting) | `boost` invalid when ratiometric |
| Thermocouple open circuit | AIN3 above 60mV (R18 pulls it to saturation) | Channel invalid; readout shows dashes |
| Cold-junction sensor missing | TMP1075 does not ACK at 0x49 | `egt` invalid; `boost` unaffected |
| Reading out of plausible range | Range check against the channel limits | Channel invalid; logged |
| I2C bus error | Transaction returns an error | Retry with backoff; mark invalid after N failures |

A channel marked invalid parks its needle at minimum and shows dashes. Displaying a stale or
implausible value on a gauge someone is relying on is worse than admitting the sensor is gone.

## Firmware

`firmware/components/sensor_hub`. Design choices are in
[ADR 0009](adr/0009-sensor-hub-sampling-and-calibration.md).

**Schedule.** A 10ms tick on core 0: AIN0 (MAP) every tick at 475 SPS, then one of AIN3
(thermocouple, ±0.256V, 128 SPS), AIN1 (supply) or AIN2 (ignition), each at 10 Hz. TMP1075 at
2 Hz. Each conversion is single-shot, and ALERT/RDY (GPIO18) signals when it is done. If three
edges are missed, the driver polls the OS bit instead.

**Channels** (gauge `<source channel>` names, native units; the face's `unit` converts them:
`psi`, `bar`, `kpa`, `inhg`, `C`, `F`, `K`):

| Channel | Unit | |
|---|---|---|
| `boost` | kPa | MAP − zero reference |
| `map` | kPa abs | |
| `egt` | °C | |
| `ignition` | V | AIN2 × 6 |
| `sensor_supply` | V | AIN1 × 2 |
| `cold_junction` | °C | TMP1075 |

**Calibration** is stored in NVS namespace `sensor_cal`, and is set on the settings page under
*Calibration*: pick a parameter, then −/+. A tap moves one step, holding repeats, and the value
is saved on release.

| Parameter | Default | Use |
|---|---|---|
| Auto-zero at start | on | Zero boost 1.5s after the MAP reading appears, if it reads 60–110 kPa |
| Stored zero | 101.3 kPa | Used when auto-zero is off or refuses; set by *Zero boost now* |
| Sensor range | 8.0 bar abs | Pressure at *Output at max* |
| Output at 0 bar / at max | 0.50 / 4.50 V | Sensor transfer function, referred to 5.00V |
| Ratiometric | on | Scale the output by the measured supply |
| Boost / EGT smoothing | 20 / 300 ms | First-order filter time constant, before the face's own damping |
| EGT offset | 0 °C | Trim against boiling water |
| MAP / supply / ignition divider | 2.000 / 2.000 / 6.000 | Set to the DMM-measured ratio |

The *Sensors* block above it shows live readings in native units (MAP kPa and connector volts,
EGT and probe µV, cold junction, supply, ignition, the zero in use), plus any fault, so each
value can be checked against a meter.

## Verification

Record measured results here as the hardware is built.

- [x] Identify the sensor: JRP 7-bar, 0-5V, 3-wire MAP sensor (from the kit listing)
- [ ] Confirm connector pinout and wire colours from the manual
- [ ] Measure the output voltage at rest to establish the transfer function
- [x] Absolute or gauge referenced: **absolute**, 0—8 bar (JRP sensor specification)
- [ ] Measure the actual divider ratio with a DMM and store it as the calibration value
- [ ] Verify ADS1115 → PSI against a known pressure reference at several points
- [ ] Verify AIN3 + TMP1075 → °C against ambient and boiling water
- [ ] Confirm an unplugged probe reads 0x7FFF and a shorted J4 reads the cold-junction temperature
- [ ] Confirm readings are stable with the engine running (electrical noise, ground offset)
- [ ] Both devices answer at 0x48 / 0x49 and ALERT/RDY edges arrive (settings must not say "ALERT line silent")
- [x] Firmware on a display with no sensor board: bus comes up on I2C0, probes quietly, the
      simulated sweep continues, WiFi still starts (2026-09-22)
