# Hardware Reference

Board: **Waveshare ESP32-S3-Touch-AMOLED-1.75**

Upstream sources (read these before changing anything here):
- <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75> — `HARDWARE_REFERENCE.md`, `Schematic/`
- <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75>

> Pin assignments and I2C addresses in this file are transcribed from the upstream hardware
> reference. Do not edit them from memory or inference — re-read the source. A wrong pin
> number here can destroy hardware.

## Core

| Item | Value |
|---|---|
| MCU | ESP32-S3R8, Xtensa LX7 dual-core @240MHz |
| SRAM | 512KB internal + 384KB ROM |
| PSRAM | 8MB **octal**, 80MHz |
| Flash | 16MB, QIO |
| Wireless | 2.4GHz WiFi b/g/n, Bluetooth 5 (LE) |

## Display — CO5300 QSPI AMOLED, 466×466

| Signal | GPIO |
|---|---|
| CS | 12 |
| CLK | 38 |
| D0–D3 (SIO0–SIO3) | 4, 5, 6, 7 |
| RST | 39 |
| **TE** (tearing effect) | **13** |

## Touch — CST9217

| Signal | GPIO / Address |
|---|---|
| I2C address | 0x5A |
| RST | 40 |
| Bus | shared system I2C (GPIO14/15) |
| Points | 2-point multitouch |

## System I2C bus — SCL=GPIO14, SDA=GPIO15, 400kHz

| Address | Device | Function |
|---|---|---|
| 0x18 | ES8311 | Playback codec |
| 0x20 | TCA9554 | I/O expander |
| 0x34 | AXP2101 | Power management / battery telemetry |
| 0x40 | ES7210 | Audio ADC (dual mic) |
| 0x51 | PCF85063 | Real-time clock (battery backed) |
| 0x5A | CST9217 | Touch controller |
| 0x6B | QMI8658 | 6-axis IMU |
| 0x50 / 0x54 | LC76G | GNSS (`-G` variant only) |

### TCA9554 I/O expander (0x20)

| Pin | Function |
|---|---|
| P0–P2 | EXIO0–EXIO2, application-defined |
| P3 | RTC_INT (from PCF85063) |
| P4 | SYS_OUT (conditioned power-button state) |
| P5 | AXP_IRQ (from AXP2101) |
| P6 | QMI_INT1 (from IMU) |
| P7 | GPS_RST (`-G` variant) |

## microSD — 1-bit SDMMC

| Signal | GPIO |
|---|---|
| CMD | 1 |
| CLK | 2 |
| D0 | 3 |
| D3/CS | 41 (wired, unused in 1-bit mode) |

Upstream warning: *"Do not repurpose GPIO1, GPIO2, or GPIO3 while the card is mounted."*

## Audio — I2S

| Signal | GPIO | Direction |
|---|---|---|
| MCLK | 42 | Out |
| BCLK | 9 | Out |
| LRCK/WS | 45 | Out |
| DOUT → ES8311 | 8 | Out |
| DIN ← ES7210 | 10 | In |
| PA enable | 46 | Out |

MCLK **must** be GPIO42.

**Speaker:** the ES8311 drives an **NS4150B** class-D amplifier (enabled by GPIO46), powered
from **VCC3V3**, with a **bridged** output on a 2-pin **MX1.25** connector (H3) — upstream
schematic and `HARDWARE_REFERENCE.md`. On the unit used for development nothing was audible even
at full volume, with the codec reporting no errors, so no speaker is fitted. A speaker plugged
into H3 needs no extra amplifier. Never ground either speaker terminal. See
[rear-pcb.md](rear-pcb.md#speaker).

## 8-pin expansion header — the only solder-free I/O

| Pin | Signal | Notes |
|---|---|---|
| 1 | VBUS | USB 5V supply |
| 2 | GND | |
| 3 | 3V3 | Board rail |
| 4 | GPIO44 / U0RXD | UART0 RX |
| 5 | GPIO43 / U0TXD | UART0 TX |
| 6 | **GPIO17** | Free (LC76G RX on `-G`) |
| 7 | **GPIO18** | Free (LC76G TX on `-G`) |
| 8 | **GPIO16** | Free |

> **Upstream sources disagree on pins 4–8.** The table above follows upstream
> `HARDWARE_REFERENCE.md`. The upstream schematic's header symbol (H2) reads pin 4 U0TXD,
> 5 U0RXD, 6 GPIO16, 7 GPIO17, 8 GPIO18. Confirm on the board with a meter before wiring
> anything to pins 6–8 — see [rear-pcb.md](rear-pcb.md#the-header--source-conflict-resolve-before-layout).
> Header VBUS is the same net as USB-C VBUS with no diode between them, so an external 5V
> supply on pin 1 must block reverse current.

This project's allocation — see [sensor-frontend.md](sensor-frontend.md):

| GPIO | Use |
|---|---|
| 17 | `I2C_NUM_1` SDA (sensor bus) |
| 18 | `I2C_NUM_1` SCL (sensor bus) |
| 16 | Shared open-drain ALERT / DRDY input |

## Critical warnings

- **All GPIO is 3.3V and is not 5V tolerant.** The 0–5V MAP sensor output must be divided
  before it reaches anything on this board or on the ADS1115.
- **Do not drive GPIO0 or GPIO43/44 externally** — they are boot-strapping and UART recovery.
- **Do not repurpose GPIO1/2/3** while the SD card is mounted.

## Why the internal ADC is unusable

ESP32-S3 splits its ADCs as **ADC1 = GPIO1–10** and **ADC2 = GPIO11–20**.

- Every ADC1 pin is taken: GPIO1/2/3 (microSD), GPIO4–7 (display data lanes), GPIO8/9/10
  (audio I2S).
- The free header pins GPIO16/17/18 are all ADC2. **ADC2 is shared with the WiFi radio on
  ESP32-S3 and cannot be read reliably while WiFi is active** — and continuous WiFi is a
  product requirement.

Reclaiming ADC1 pins by dropping SD and audio was evaluated and rejected — see
[ADR 0004](adr/0004-retain-sd-and-audio.md). The short version: those pins are not brought
out to any connector, so using them means board rework, and it would not remove the need for
an I2C thermocouple amplifier anyway.

Reference: <https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/adc.html>
