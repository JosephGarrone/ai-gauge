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
| [adr/](docs/adr/) | Why things are the way they are |

## Status

Early. Milestone 1 (full vertical slice) is in progress: docs, board profile, gauge XML parser
with host tests, and display bring-up are in place. The dial renderer, sensor hub and
networking are not yet implemented, and **no performance figures have been measured on
hardware** — see [docs/performance.md](docs/performance.md).
