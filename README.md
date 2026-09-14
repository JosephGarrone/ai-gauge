# AI-Gauge

Customisable automotive dial gauge firmware for the Waveshare
**ESP32-S3-Touch-AMOLED-1.75** (466×466 round AMOLED).

Replaces physical boost and EGT gauges, re-using the vehicle's existing sensors through an
external signal-conditioning front-end. The gauge face is defined in XML, so it can be
restyled without rebuilding firmware.

## Goals

- **60fps+**, always. A laggy needle is worse than the analogue gauge it replaces.
- **XML-defined faces** — restyle over WiFi, no rebuild. Eventually authored in a web app.
- **Portable** to square and other round panels via a board profile.
- **Swipe up** from the dial for settings.
- **WiFi** for config upload, OTA, and telemetry feeds pushed from other devices.

## Getting started

```bash
. ~/esp/v5.5.1/esp-idf/export.sh
idf.py -C firmware set-target esp32s3
idf.py -C firmware -p <PORT> flash monitor
```

Full instructions: [docs/build-and-flash.md](docs/build-and-flash.md).

Not developing the firmware? Every tagged release publishes a browser-based installer — flash
from Chrome or Edge with no toolchain. See [docs/ci-release.md](docs/ci-release.md).

## Documentation

Start with **[AGENTS.md](AGENTS.md)** for the design decisions and the conventions this
project follows, then [docs/](docs/) for detail:

| Document | Contents |
|---|---|
| [architecture.md](docs/architecture.md) | Components, task model, data flow |
| [display-pipeline.md](docs/display-pipeline.md) | Frame budget and how 60fps is achieved |
| [gauge-config-schema.md](docs/gauge-config-schema.md) | The gauge XML format |
| [hardware-reference.md](docs/hardware-reference.md) | GPIO map, I2C addresses, warnings |
| [sensor-frontend.md](docs/sensor-frontend.md) | Boost and EGT signal conditioning |
| [networking.md](docs/networking.md) | Provisioning, HTTP API, telemetry, OTA |
| [performance.md](docs/performance.md) | Measurement method and results |
| [roadmap.md](docs/roadmap.md) | Milestones and what is planned next |
| [adr/](docs/adr/) | Why things are the way they are |

## Status

Running on hardware, driven by a **simulated** value source — no sensor is being read yet, and
the sensor board does not exist.

**Working today:** the dial renders from an XML configuration held on flash, with colour
bands, ticks, labels and a damped needle. Crossing an alert threshold flashes the needle in the
alert colour. A peak-hold marker records the highest reading on the dial; tap the dial to reset
it. Swipe up for settings — pick a different gauge, adjust brightness, toggle an FPS badge or the
alert sound, and read live frame statistics and firmware information. Choices persist across
reboots.

The alert chime is implemented and plays through the audio codec without errors, but nothing is
audible on the development board, which appears to have no speaker fitted.

Over WiFi, the gauge is set up from a phone and then reachable at `ai-gauge-XXXX.local`. Its HTTP
API uploads new gauge faces and applies them live, accepts telemetry from other devices, and
installs firmware updates, rolling back automatically if an update fails to start. See
[docs/networking.md](docs/networking.md); note the API is unauthenticated, so use it only on a
network you trust.

A missing or malformed configuration falls back to the next available one, and then to a
compiled-in face, always saying on the settings page what went wrong. A bad config file must
never leave a driver looking at a blank screen.

**Measured on hardware** (ESP32-S3 rev v0.2), needle sweeping continuously with WiFi connected,
peak-hold and audio running:

| | Result |
|---|---|
| Frame rate | **66.7 fps** (the LVGL refresh ceiling, not a rendering limit) |
| Screen redrawn per frame | ~13% |
| Render time | ~7.7 ms of each 15 ms period |
| Swipe transition | ~33 fps — a documented, accepted trade |
| Internal RAM free after startup | ~8.6 KB — the board's scarcest resource |

CPU is not the constraint on this board; **internal RAM is**. The display's flush buffers, WiFi's
receive buffers and every task stack all compete for the ESP32-S3's ~512 KB of internal SRAM, while
8 MB of PSRAM sits largely idle. Anything that adds a task, a stack or a small allocation has to
be measured. The rules are in [AGENTS.md](AGENTS.md).

Method, the renderer designs that failed, and how each memory limit was found and fixed, are in
[docs/performance.md](docs/performance.md).

## Roadmap

Detail and exit criteria in [docs/roadmap.md](docs/roadmap.md).

| | Milestone | State |
|---|---|---|
| M1 | Foundation: docs, build, XML parser, CI, display bring-up | ✅ |
| M2 | Renderer and screens; the 60fps gate | ✅ |
| M3 | Load configurations from flash; switch gauges at runtime | ✅ |
| M4 | WiFi: provisioning, config upload, telemetry feeds, OTA | ✅ |
| M5 | Sensors: boost via ADS1115, EGT via MCP9600 | blocked on hardware |
| **M6** | **Alerts, peak-hold, min/max recall, datalogging** | **🚧 in progress** |
| M7 | Portability: square and other round panels | |
| M8 | Web app for designing gauge faces | |

M6 so far: needle alert flash and peak-hold are verified on hardware; the chime is done but
silent (no speaker). Min/max recall is next, and SD datalogging waits for a card.

Software comes before sensors deliberately. The sensor board still has to be built, and the
firmware treats a simulated source, a network feed and a real sensor identically — so the
whole UI and network stack can be finished and verified before any wiring exists.
