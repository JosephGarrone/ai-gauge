# Sensor Front-End

Re-uses the vehicle's existing boost and EGT sensors. Because no internal ADC is usable (see
[hardware-reference.md](hardware-reference.md) and
[ADR 0001](adr/0001-external-i2c-sensor-frontend.md)), both sensors are read over a
**second I2C bus** brought out on the 8-pin expansion header.

> **Status:** this describes the intended circuit. Nothing here has been built or verified on
> hardware yet. Record measured results in the *Verification* section as they happen, and do
> not describe an unbuilt circuit as working.

## Bus

| Signal | GPIO | Notes |
|---|---|---|
| SDA | 17 | Header pin 6 |
| SCL | 18 | Header pin 7 |
| ALERT / DRDY | 16 | Header pin 8, open-drain, shared, pulled up |
| 3V3 | — | Header pin 3, logic supply |
| 5V | — | Header pin 1 (VBUS) — **see the power warning below** |
| GND | — | Header pin 2 |

`I2C_NUM_1` at **100kHz**: the MCP9600 datasheet limits its I2C interface to 10–100kHz. Separate from the system bus (GPIO14/15) so sensor polling never
contends with touch, and a wiring fault on the harness cannot take down the touchscreen.

| Address | Device | Channel |
|---|---|---|
| 0x48 | ADS1115 | `boost` |
| 0x67 | MCP9600 | `egt` |

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

250 SPS continuous-conversion mode, with DRDY on GPIO16 or simple periodic polling. A boost
gauge needs to feel instantaneous, so oversample lightly and let the needle damping in
`<source damping="...">` do the visual smoothing rather than over-filtering the raw signal.

## EGT — K-type thermocouple via MCP9600

A thermocouple produces roughly **41µV/°C** and needs cold-junction compensation. It cannot be
fed to a general ADC directly. The **MCP9600** does the amplification, linearisation and CJC
in one I2C part, which is why the boost and EGT paths use different chips.

MAX31855/MAX31856 are the common alternatives but are SPI, and no SPI pins remain free — see
[ADR 0001](adr/0001-external-i2c-sensor-frontend.md).

| Setting | Value |
|---|---|
| Thermocouple type | K |
| Resolution | 0.25°C (adequate; EGT is displayed in whole degrees) |
| ADC resolution | 18-bit |
| Filter | Mid-range MCP9600 internal filter |
| Range | Up to ~1372°C for K-type — beyond any realistic EGT |

Reads `T_hot` directly in °C over I2C; no scaling maths needed. Its alert outputs are **push-pull**
(datasheet section 3), so they must not join the shared open-drain ALERT line on GPIO16. They
are left unconnected; thermocouple faults are read by polling. See
[rear-pcb-parts.md](rear-pcb-parts.md).

**Thermocouple wiring:** use K-type extension wire and connectors for the whole run. Any
junction with copper creates a parasitic thermocouple and an error the MCP9600's CJC cannot
correct for. Keep the wiring away from ignition leads.

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
- **5V → 3.3V** for the ADS1115/MCP9600 logic, or run their logic from the board's 3V3.
- **Fusing** on the 12V feed.
- **Ignition-switched supply**, or a low-quiescent-current path, so the gauge cannot flatten
  the battery.
- **Star grounding** at the sensor board, with the ADS1115 ground referenced to the MAP
  sensor's ground rather than picked up elsewhere in the vehicle. Ground offsets across a
  chassis are a real and common source of gauge error.

## Fault handling

| Condition | Detection | Behaviour |
|---|---|---|
| Sensor board absent | I2C probe fails at startup | Fall back to the simulated source; warning on settings screen |
| Thermocouple open circuit | MCP9600 fault bits | Channel invalid; readout shows dashes |
| Reading out of plausible range | Range check against the channel limits | Channel invalid; logged |
| I2C bus error | Transaction returns an error | Retry with backoff; mark invalid after N failures |

A channel marked invalid parks its needle at minimum and shows dashes. Displaying a stale or
implausible value on a gauge someone is relying on is worse than admitting the sensor is gone.

## Verification

Record measured results here as the hardware is built.

- [x] Identify the sensor: JRP 7-bar, 0-5V, 3-wire MAP sensor (from the kit listing)
- [ ] Confirm connector pinout and wire colours from the manual
- [ ] Measure the output voltage at rest to establish the transfer function
- [x] Absolute or gauge referenced: **absolute**, 0—8 bar (JRP sensor specification)
- [ ] Measure the actual divider ratio with a DMM and store it as the calibration value
- [ ] Verify ADS1115 → PSI against a known pressure reference at several points
- [ ] Verify MCP9600 → °C against ambient and boiling water
- [ ] Confirm readings are stable with the engine running (electrical noise, ground offset)
