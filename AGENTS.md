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
| [docs/roadmap.md](docs/roadmap.md) | Milestones, ordering and exit criteria |
| [docs/architecture.md](docs/architecture.md) | Components, task model, data flow |
| [docs/display-pipeline.md](docs/display-pipeline.md) | Frame budget, LVGL/DMA/TE strategy, render rules |
| [docs/gauge-config-schema.md](docs/gauge-config-schema.md) | The gauge XML schema (normative) |
| [docs/gauge-xml-interface.md](docs/gauge-xml-interface.md) | Authoring guide for the web app: format, rendering model, upload API. Update with the schema |
| [docs/config-app.md](docs/config-app.md) | The face editor (M7): structure, firmware parity testing, Pages publishing, device-upload status |
| [docs/sensor-frontend.md](docs/sensor-frontend.md) | ADS1115/MCP9600 wiring, scaling maths, 12V conditioning |
| [docs/rear-pcb.md](docs/rear-pcb.md) | Rear PCB plan (planning only): 12V input and daisy chain, sensor connectors, speaker, 45mm outline |
| [docs/rear-pcb-parts.md](docs/rear-pcb-parts.md) | Rear PCB parts: BOM, KiCad symbols/footprints, per-pin nets, SVG wiring sheets in `docs/rear-pcb/` |
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
| [0006](docs/adr/0006-custom-shapes-as-polygons.md) | Custom tick/needle/hub shapes are SVG-style polygons and circles, filled by our own anti-aliased rasteriser (`gauge_shape`) | Keeps the needle's tight dirty box; LVGL triangles seam, rotated images blow the budget |
| [0007](docs/adr/0007-face-editor-static-app-with-parser-port.md) | The face editor is a dependency-free static web app carrying a line-for-line JS port of the gauge XML parser, held to the C by a differential test in CI | Its device verdict must follow the firmware's forgiving rules exactly; no JS toolchain in a C repository |

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
  components/  board_profile, gauge_config, gauge_shape, gauge_render, sensor_hub, app_settings, net_svc
  assets/      Contents of the LittleFS storage partition
tools/         Host-side tooling: host tests, web installer, config-app (the face editor)
.github/       CI: build artifacts on every push, releases + browser flashing on tags
```

## Status and roadmap

Full plan, ordering and exit criteria: **[docs/roadmap.md](docs/roadmap.md)**. Update it when
a milestone starts or finishes; a milestone is done when its exit criteria are *verified*, not
when the code compiles.

| | Milestone | State |
|---|---|---|
| M1 | Foundation: docs, build, parser, CI, display bring-up | complete |
| M2 | Renderer and screens; 60fps gate | complete |
| M3 | Configuration from LittleFS, gauge switching | complete |
| M4 | Networking: provisioning, HTTP API, telemetry, OTA | complete |
| **M5** | **Sensors: ADS1115 + MCP9600 front-end** | **next, blocked on hardware** |
| M6 | Alerts, chime, peak-hold, datalogging | in progress |
| M7 | Configuration web app | in progress |
| M8 | Portability: a second board profile | |

**Verified on hardware** (ESP32-S3 rev v0.2): **66.6 fps**, 13.2% dirty, 5.9ms render per
15ms period, with the needle sweeping continuously. The swipe transition costs ~33 fps, a
documented and accepted trade. Numbers and the two designs that failed first are in
[docs/performance.md](docs/performance.md).

**Custom shapes (ADR 0006) are built and host-tested only**: not yet run on a board, frame cost unmeasured.

**The face editor (`tools/config-app`) is built and tested, not yet deployed.** Its parser is a port of `gauge_config.c`: change both in the same commit, or CI's differential test fails. Uploading from it is blocked by the firmware's missing CORS and HTTP-only API. See [docs/config-app.md](docs/config-app.md).

Faces load from `/storage/gauges/*.xml` on LittleFS, falling back to the next available
config and then to the compiled-in face. All three paths are verified on hardware.

**M4 verified on hardware:** provisioning, mDNS, all HTTP endpoints (17 checks), live config reload, OTA over WiFi, boot-time rollback of an image that aborts before confirming itself, and telemetry driving the needle at 58.6 fps.

**Bench power:** once WiFi starts, a front-panel USB port or hub can cut power to the board (dial flashes, goes black, COM port vanishes). Use a rear motherboard port.

**Internal RAM is the binding constraint, not CPU.** After WiFi starts, only ~3KB of internal heap remains. The LVGL flush buffers are deliberately in internal DMA memory (`retarget_draw_buffers()` in `app_main.c`) because the BSP's PSRAM buffers forced a bounce copy on every flush and broke the display once WiFi ran. `DRAW_BUF_LINES` and the memory settings in `sdkconfig.defaults` are load-bearing: change them only with a hardware measurement. See [docs/performance.md](docs/performance.md).

Two ways internal RAM leaks away unnoticed:
- **Task stacks are internal RAM.** That includes LVGL's draw threads, the HTTP server and every `xTaskCreate()`. Size them from a measured high-water mark, not a default. WiFi's own startup needs ~48KB of internal DMA memory, and shrinking the draw-thread stacks is what made room for 6 static RX buffers.
- **Plain `malloc()` of under 4KB is internal RAM too** (`SPIRAM_MALLOC_ALWAYSINTERNAL`). Allocate buffers and large structs, such as a 2.3KB `gauge_config_t`, with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. Never put them on a task stack or in a static.

**Never call the BSP's audio init** (`bsp_audio_init()`, `bsp_audio_codec_*_init()`). It
allocates speaker and microphone buffers that do not fit once WiFi runs, and the BSP aborts on
the failure, boot-looping the board. OTA images confirm themselves 15s after startup, so a
crash in that window rolls back rather than looping.

**The value source is simulated** (`CONFIG_AI_GAUGE_SIMULATED_SOURCE`). **No sensor is being
read**, and no sensor hardware exists yet.

### Measuring performance

`CONFIG_AI_GAUGE_BENCH_TRANSITION=y` flips tiles continuously so the transition is a
repeatable input. Turn it off afterwards -- it makes the display unusable.

**Measure with nobody touching the screen.** Interaction pollutes the numbers badly: sitting
on the settings tile reports single-digit fps simply because nothing is being redrawn, which
looks like a stall and is not one.
