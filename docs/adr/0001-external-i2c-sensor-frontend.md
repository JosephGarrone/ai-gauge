# ADR 0001 — External I2C sensor front-end

**Status:** Accepted

## Context

The gauge must read a boost pressure sensor and an EGT thermocouple while WiFi is
continuously available for telemetry, config upload and OTA.

The ESP32-S3 splits its ADCs as **ADC1 = GPIO1–10** and **ADC2 = GPIO11–20**. On this board:

- GPIO1/2/3 — microSD (1-bit SDMMC)
- GPIO4–7 — QSPI display data lanes
- GPIO8/9/10 — audio I2S

Every ADC1 pin is occupied. The only pins brought out to the 8-pin expansion header are
GPIO16/17/18 — all **ADC2**, which on ESP32-S3 is shared with the WiFi radio and cannot be
read reliably while WiFi is active. Espressif's guidance is unambiguous: use ADC1 when WiFi is
in use.

So the internal ADC is unavailable, and continuous WiFi is a product requirement.

Separately, a K-type thermocouple outputs ~41µV/°C and requires cold-junction compensation. It
cannot be connected to a general-purpose ADC at all — it needs a dedicated amplifier
regardless of which ADC is available.

## Decision

Read both sensors over a **second I2C bus** (`I2C_NUM_1`) on the expansion header:

| GPIO | Role |
|---|---|
| 17 | SDA |
| 18 | SCL |
| 16 | Shared open-drain ALERT / DRDY |

| Device | Address | Channel |
|---|---|---|
| ADS1115 (16-bit ADC) | 0x48 | `boost` |
| MCP9600 (K-type + CJC) | 0x67 | `egt` |

Wiring, scaling maths and protection requirements are in
[../sensor-frontend.md](../sensor-frontend.md).

## Alternatives considered

**Internal ADC2 with WiFi disabled during reads.** Rejected. It would make WiFi mutually
exclusive with sampling, defeating the remote-telemetry and remote-config goals that are core
to the product.

**Drop the microSD and audio to reclaim ADC1 pins.** Rejected — see
[ADR 0004](0004-retain-sd-and-audio.md). Those pins are not brought out to any connector
(GPIO1/2/3 land only on the microSD socket pads; GPIO8/9/10 run to the codecs, and the ES7210
actively drives GPIO10), so it means board rework, and it would not avoid needing an I2C
thermocouple amplifier anyway.

**MAX31855 / MAX31856 for the thermocouple.** Rejected: both are SPI, and no SPI pins remain
free. The MCP9600 is the I2C equivalent.

**UART companion MCU doing all analogue conditioning.** Rejected as unnecessary complexity for
two channels. It remains the right answer if the channel count grows substantially or if
hard-real-time signal processing (knock detection, high-rate pulse counting) is ever needed.

**Solder to the microSD pads for ADC1.** Rejected. It requires rework, still needs the I2C
bus for the MCP9600, and the ESP32-S3's internal ADC is noticeably noisier and less linear
than an ADS1115 — a real disadvantage in the electrically hostile environment of a running
engine bay.

## Consequences

**Good:**
- WiFi and sensing coexist with no compromise.
- 16-bit differential conversion with a stable internal reference, far better than the
  internal ADC in an automotive noise environment.
- No board rework; everything connects to the existing header.
- The same wiring is portable to any future board, since it depends only on two I2C pins.
- Sensor polling cannot contend with the touch controller, which is on the system bus.

**Bad:**
- Two additional components and a small custom sensor board to design and build.
- ADS1115 tops out at 860 SPS. Ample for a boost gauge, but it forecloses high-rate sampling
  should a future channel need it.
- A 5V rail is needed for the MAP sensor, plus the automotive protection circuitry described
  in [../sensor-frontend.md](../sensor-frontend.md).

**Neutral:**
- GPIO16 is committed to the shared ALERT/DRDY line, spending the last free header pin.
