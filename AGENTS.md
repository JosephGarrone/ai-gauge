# AGENTS.md — AI-Gauge

Firmware for a customisable automotive dial gauge on the Waveshare
**ESP32-S3-Touch-AMOLED-1.75** (466×466 round AMOLED). Replaces physical boost and EGT gauges,
re-using the existing vehicle sensors through an external signal-conditioning front-end.

---

## Instructions for all agents

**These instructions are binding on every agent that works on this repository, including
future sessions. Propagate them — do not let them decay.**

1. **Record decisions here.** Any significant technical decision goes in this file, with the
   detailed reasoning in `docs/adr/`. If you make a choice a future reader might reasonably
   question, write it down with the alternatives you rejected and why.
2. **Keep `AGENTS.md` lean.** This file is loaded into every agent's context, so it pays for
   its size in tokens on every single session. It holds decisions, constraints and pointers —
   never tutorials, never long explanations, never generated output. Detail belongs in
   `docs/`. If a section here grows past a few paragraphs, move the body to `docs/` and leave
   a one-line pointer.
3. **Update the docs in the same change as the code.** A decision that isn't written down did
   not happen. Docs drifting from code is treated as a bug.
4. **The 60fps requirement is a hard constraint, not an aspiration.** See
   [docs/display-pipeline.md](docs/display-pipeline.md) for the frame budget and the rules
   that protect it. Any change that could plausibly affect render cost must be measured, and
   the numbers recorded in [docs/performance.md](docs/performance.md).
5. **Verify, don't assume.** Build before claiming a change works. If you cannot test
   something (no board attached, no sensor wired), say so explicitly rather than implying it
   was verified.
6. **Never invent hardware facts.** Pin assignments, I2C addresses and register maps come
   from [docs/hardware-reference.md](docs/hardware-reference.md) or an upstream source you
   actually read. Getting these wrong can destroy hardware.

---

## Documentation map

| Document | Contents |
|---|---|
| [docs/hardware-reference.md](docs/hardware-reference.md) | GPIO map, I2C addresses, pin-usage warnings |
| [docs/architecture.md](docs/architecture.md) | Components, task model, data flow |
| [docs/display-pipeline.md](docs/display-pipeline.md) | Frame budget, LVGL/DMA/TE strategy, render rules |
| [docs/gauge-config-schema.md](docs/gauge-config-schema.md) | The gauge XML schema (normative) |
| [docs/sensor-frontend.md](docs/sensor-frontend.md) | ADS1115/MCP9600 wiring, scaling maths, 12V conditioning |
| [docs/networking.md](docs/networking.md) | Provisioning, HTTP API, telemetry ingest, OTA |
| [docs/build-and-flash.md](docs/build-and-flash.md) | Local toolchain, build and flash |
| [docs/ci-release.md](docs/ci-release.md) | CI pipeline, versioning, browser flashing |
| [docs/performance.md](docs/performance.md) | Measurement method and recorded results |
| [docs/adr/](docs/adr/) | One file per significant decision |

---

## Hardware constraints that shape everything

Full detail in [docs/hardware-reference.md](docs/hardware-reference.md). The two that drive
the architecture:

- **No internal ADC is usable.** ESP32-S3 ADC1 is GPIO1–10, all of which are consumed by the
  microSD slot, the QSPI display data lanes and the audio codecs. The only solder-free spare
  pins (GPIO16/17/18, on the 8-pin header) are **ADC2**, which ESP32-S3 cannot use while WiFi
  is active. Sensors therefore connect over a **second I2C bus** — see
  [ADR 0001](docs/adr/0001-external-i2c-sensor-frontend.md).
- **A full-frame display blit costs ~11ms of the 16.6ms frame budget.** Redrawing the whole
  screen every frame cannot hit 60fps. The renderer is built so the steady-state dirty region
  stays small — see [ADR 0003](docs/adr/0003-static-background-plus-needle-sprite.md).

---

## Key decisions

| # | Decision | Rationale |
|---|---|---|
| [0001](docs/adr/0001-external-i2c-sensor-frontend.md) | Sensors on external I2C (ADS1115 + MCP9600) on `I2C_NUM_1`, GPIO17/18 | No usable internal ADC; a thermocouple needs a CJC amplifier regardless |
| [0002](docs/adr/0002-custom-gauge-xml-schema.md) | Custom domain-specific gauge XML, not LVGL's generic XML UI format | Keeps a general layout engine out of the 16ms render path |
| [0003](docs/adr/0003-static-background-plus-needle-sprite.md) | Pre-render the dial face once to PSRAM; animate only a needle sprite | Bounds the per-frame dirty region, which is what makes 60fps reachable |
| [0004](docs/adr/0004-retain-sd-and-audio.md) | Keep the microSD slot and audio codecs | Their pins are not connector-accessible anyway, and both have real uses |
| [0005](docs/adr/0005-consume-waveshare-bsp.md) | Use the upstream `waveshare/esp32_s3_touch_amoled_1_75` BSP | Maintained CO5300/CST9217 drivers; effort goes into rendering instead |

---

## Conventions

- **Language:** C (C11). C++ only where a dependency requires it.
- **ESP-IDF v5.5.1** — the BSP requires ≥5.5. CI pins the same version.
- **Namespacing:** each component prefixes its public symbols with its own name
  (`gauge_config_*`, `sensor_hub_*`, `net_svc_*`). Nothing reaches into another component's
  private headers.
- **Error handling:** return `esp_err_t` and use `ESP_RETURN_ON_ERROR` / `ESP_GOTO_ON_ERROR`.
  A failure that a user could plausibly hit at runtime (bad config file, missing sensor)
  degrades gracefully and surfaces on the settings screen; it never aborts the render loop.
- **Logging:** one `static const char *TAG` per file, matching the component name.
- **LVGL thread safety:** LVGL is touched **only** from the LVGL task, or under
  `bsp_display_lock()`. `sensor_hub` and `net_svc` publish data through snapshots and never
  call LVGL directly. See [docs/architecture.md](docs/architecture.md).
- **No hard-coded panel geometry.** Anything that would otherwise say `466` reads from
  `board_profile` instead, so other panels remain a config change.
- **Commit messages:** imperative mood, explain *why* in the body when it isn't obvious.

---

## Layout

```
docs/          Design documentation and ADRs
firmware/      The ESP-IDF project
  main/        Entry point and screen wiring
  components/  board_profile, gauge_config, gauge_render, sensor_hub, app_settings, net_svc
  assets/      Contents of the LittleFS storage partition
tools/         Host-side tooling (XML validation; future config web app)
.github/       CI: build artifacts on every push, releases + browser flashing on tags
```

## Status

Milestone 2 complete: **the 60fps gate passes on hardware.**

**Measured** (ESP32-S3 rev v0.2, full-scale needle sweep, simulated source): **66.2 fps**,
13.3% dirty area, 58KB/frame, 5.8ms render per 15ms period. That is the LVGL refresh-period
ceiling, not a rendering limit, so there is genuine headroom. The first attempt failed at
45 fps -- see [docs/performance.md](docs/performance.md), which records both the numbers and
what was wrong.

**Done:** docs and ADRs; ESP-IDF project building clean and booting on hardware;
`board_profile`; `gauge_config` parser (70 host-test checks, `-Werror`); `gauge_render`
(pre-rendered PSRAM face, custom-drawn needle, readout, threshold alerts); `gauge_perf`
instrumentation; CI workflows and the web installer page.

**Not yet done:** `sensor_hub`, `net_svc`, `app_settings`, the tileview screens and swipe-up
navigation, loading configs from LittleFS, and the audible alert chime. The value source is
still a simulated sweep in `app_main`.

**Not yet measured:** the swipe transition (scenario 4) and behaviour with WiFi active
(scenario 5). Both are listed in [docs/performance.md](docs/performance.md) and neither has
been run.
