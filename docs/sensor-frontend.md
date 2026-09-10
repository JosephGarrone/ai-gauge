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

`I2C_NUM_1` at 400kHz. Separate from the system bus (GPIO14/15) so sensor polling never
contends with touch, and a wiring fault on the harness cannot take down the touchscreen.

| Address | Device | Channel |
|---|---|---|
| 0x48 | ADS1115 | `boost` |
| 0x67 | MCP9600 | `egt` |

Both are address-strappable if they clash with something added later.

## Boost — 0–60 PSI MAP sensor via ADS1115

**Sensor:** ratiometric 0–60 PSI pressure sensor with a 0.5–4.5V output on a 5V supply.
Connectors are Just Race Parts, matching the existing gauges.

> **Open item:** the exact part number, connector pinout and transfer function still need to
> be confirmed against the physical sensor. The 0.5–4.5V ratiometric assumption below is the
> industry norm for this class of sensor but **must be verified before wiring anything.**

**ADC:** ADS1115 — 16-bit, differential, programmable gain, internal reference, 250 SPS.

### The divider is mandatory

The sensor swings to **4.5V**. ADS1115 inputs must not exceed `VDD + 0.3V`, and on a 3.3V
rail that is 3.6V. Connecting the sensor directly will damage the ADC.

A 2:1 divider brings full scale to 2.25V, comfortably inside the ±4.096V PGA range:

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
PSI_abs  = (V_sensor - 0.5) / 4.0 × 60     # 0.5V = 0 PSI, 4.5V = 60 PSI
```

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
gauge zeroes correctly at altitude. Confirm which type this sensor is when confirming the part
number — getting it wrong offsets every reading by about 14.7 PSI.

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

Reads `T_hot` directly in °C over I2C; no scaling maths needed. The alert output can drive
GPIO16 for a hardware over-temperature signal independent of firmware polling.

**Thermocouple wiring:** use K-type extension wire and connectors for the whole run. Any
junction with copper creates a parasitic thermocouple and an error the MCP9600's CJC cannot
correct for. Keep the wiring away from ignition leads.

## Power and automotive protection

> **Not yet designed.** This is a hardware requirement, recorded so it is not forgotten.

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

- [ ] Confirm MAP sensor part number, connector pinout and transfer function
- [ ] Confirm whether the MAP sensor is absolute or gauge referenced
- [ ] Measure the actual divider ratio with a DMM and store it as the calibration value
- [ ] Verify ADS1115 → PSI against a known pressure reference at several points
- [ ] Verify MCP9600 → °C against ambient and boiling water
- [ ] Confirm readings are stable with the engine running (electrical noise, ground offset)
