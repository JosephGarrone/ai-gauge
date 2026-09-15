# Roadmap

The plan of work, in the order it is intended to happen, with the exit criteria that decide
when each milestone is actually done.

**Keep this honest.** A milestone is complete when its exit criteria have been *verified*, not
when the code has been written. Anything measured belongs in
[performance.md](performance.md); anything decided belongs in [adr/](adr/).

## Ordering principle

Software-only work comes before sensor work, even though the sensors are the point of the
product. Two reasons: the external I2C front-end does not physically exist yet
([sensor-frontend.md](sensor-frontend.md) lists what has to be built), and the channel
abstraction in [architecture.md](architecture.md) means a simulated source, a network feed and
a real sensor are interchangeable to everything above them. So the entire UI and network stack
can be finished and verified before a single wire is crimped.

The one thing that could not wait was the frame budget. If the renderer could not hold 60fps,
everything built on top of it would have been wasted — which is why M2 was gated on a
measurement rather than on the code compiling.

---

## M1 — Foundation ✅ complete

Repository, documentation, build and display bring-up.

- `AGENTS.md`, `docs/`, five ADRs
- ESP-IDF project on the Waveshare BSP, LVGL 9.4, LittleFS
- `board_profile` as the portability seam
- `gauge_config`: the gauge XML parser, with host tests and no ESP-IDF/LVGL dependency
- CI publishing a flashable merged binary on every push, plus a browser installer on tags

**Exit criteria met:** builds clean, boots on hardware, panel and touch initialise, parser
runs on target with zero warnings, CI artifact verified flashable.

## M2 — Renderer and screens ✅ complete

The dial, the settings page, and proof that the frame budget holds.

- `gauge_render`: pre-rendered PSRAM face, custom-drawn needle, readout, threshold alerts
- `gauge_perf`: frame-cost instrumentation
- `app_ui`: tileview with swipe-up settings
- `app_settings`: NVS-backed brightness, FPS toggle, active gauge

**Exit criteria met:** 66.6 fps at 13.2% dirty with the needle sweeping continuously. Two
designs failed on the way; both are recorded in [performance.md](performance.md).

---

## M3 — Configuration from storage ✅ complete

Make the gauge genuinely data-driven, which is the point of the XML schema.

- Mount LittleFS on the `storage` partition
- Load `/storage/gauges/<id>.xml`, selected by `app_settings.active_gauge`
- Fall back to the built-in face when the file is missing or unparseable, and surface the
  reason through `app_ui_set_warning()` rather than only in the serial log
- Gauge switching from the settings page, re-rendering the face without a reboot
- Enumerate available configs so the picker lists what is actually on the device

**Exit criteria met**, all verified on hardware:

| Case | Result |
|---|---|
| Valid config | `boost` loaded from `/storage/gauges/boost.xml` |
| Malformed active config | rejected with the specific reason, fell back to `egt`, warning shown |
| Empty or corrupt filesystem | auto-formatted, fell back to the built-in face, warning shown |
| Runtime switching | 8 consecutive switches, ~41ms each, PSRAM free unchanged |
| No regression | scenario 2 still 66.6 fps |

`gauge_store_save()` already validates before replacing, ready for the M4 upload path.

## M4 — Networking ✅ complete

Remote access, and the last unmeasured performance question.

- WiFi station with SoftAP fallback (`ai-gauge-setup`) and a captive provisioning page
- mDNS as `ai-gauge.local`
- HTTP API per [networking.md](networking.md): status, config list/get/put/delete, OTA
- Validate an uploaded config *before* replacing the live one, so a bad upload cannot leave a
  vehicle with a blank gauge
- UDP telemetry ingest, feeding the same channel snapshot as physical sensors
- OTA with rollback

**Exit criteria:** provision from clean NVS; `ai-gauge.local` resolves; upload a config over
HTTP and see the face reload without rebooting; upload a deliberately malformed one and
confirm the existing config survives; complete an OTA and roll back a deliberately bad image.

**Also closes scenario 5** in [performance.md](performance.md): WiFi runs on core 0 and LVGL
on core 1, and the claim that they do not interfere is an assumption until measured with a
telemetry stream running.

**Exit criteria met**, all verified on hardware:

| Item | State |
|---|---|
| Provision from clean NVS | **verified**, including a failed attempt falling back safely |
| `ai-gauge-XXXX.local` resolves | **verified** |
| Upload a config and see it reload without rebooting | **verified** |
| Malformed upload leaves the existing config intact | **verified** |
| OTA completes | **verified**: 1.58MB over WiFi in ~15s, boots ota_1, recorded VALID |
| Roll back a deliberately bad image | **verified** both ways: a corrupt upload is rejected, and an image that boots then aborts before confirming itself is marked ABORTED and the previous image boots |
| Telemetry drives the needle | **verified** at 58.6Hz |
| Scenario 5 | **measured:** every telemetry datagram rendered at 58.6 fps with WiFi carrying it |

Bringing WiFi up exposed a memory fault that had existed since M2: the BSP's PSRAM flush buffers forced a ~46KB internal bounce buffer and copy on every flush, and WiFi took the memory it relied on, stopping the display. Fixed by moving the flush buffers into internal RAM; the full account is in [performance.md](performance.md).

Scenario 5 also exposed wasted redraws of an unchanged needle. Fixed in `gauge_render` and verified; see [performance.md](performance.md).

**Known gap:** the HTTP API is unauthenticated and unencrypted. Acceptable on a private
network, not a considered security posture. See *Security* in
[networking.md](networking.md) — this is tracked, not forgotten.

## M5 — Sensors 🔜 next

The actual product goal. **Blocked on hardware that does not exist yet.**

- Build the sensor board: ADS1115 + MCP9600 on `I2C_NUM_1` (SDA GPIO16, SCL GPIO17, alert GPIO18), the 2:1 divider, and
  the 12V automotive conditioning described in [sensor-frontend.md](sensor-frontend.md)
- `sensor_hub`: drivers, scaling, IIR filtering, lock-free snapshot publication
- Calibration values as settings, not constants, so a different sensor is not a recompile
- Fault handling: absent board, open thermocouple, out-of-range readings, I2C errors

**Exit criteria:** the open items checklist at the end of
[sensor-frontend.md](sensor-frontend.md) — confirmed MAP sensor part number and transfer
function, measured divider ratio, ADS1115 verified against a pressure reference, MCP9600
verified against ambient and boiling water, and readings stable with the engine running.

**Settled:** the JRP 7-bar sensor is **absolute** (0–8 bar abs), so the firmware must capture a
zero at key-on with the engine off, as the JRP gauge does. See
[sensor-frontend.md](sensor-frontend.md).

## M6 — Alerts and refinement 🚧 in progress

- Audible over-boost / over-EGT chime through the ES8311 codec, wiring up
  `<alert chime="true">` ([ADR 0004](adr/0004-retain-sd-and-audio.md)). The BSP's own audio
  setup does not fit alongside WiFi and aborts when it fails, so the chime needs its own
  speaker-only I2S channel; see the audio section of [performance.md](performance.md).
- Recolour the needle on alert, not just the readout
- Peak-hold and min/max recall — conventional on boost and EGT gauges
- Optional datalogging to the microSD slot

**Progress:**

| Item | State |
|---|---|
| Audible chime on `<alert chime="true">` | code complete and running: the codec accepts playback at full volume without errors, fits alongside WiFi, rate-limited, user toggle in settings. **Silent on this board, which appears to have no speaker fitted** (nothing audible at 100% volume). Ready for when one is connected |
| Needle takes the alert colour while flashing | **verified on hardware**; flashing repaints the needle even when it is not moving |
| Peak-hold marker, tap to reset | **verified on hardware:** amber marker and PEAK readout appear, a tap clears both, 66.7 fps; `<peak>` schema element with 85 host checks |
| WiFi memory ceiling (found while adding peak-hold) | **fixed and verified:** smaller LVGL draw-thread stacks restored 6 WiFi RX buffers; config uploads no longer near-overflow the HTTP server stack. 66.7 fps, OTA and 17/17 endpoint checks pass. See [performance.md](performance.md) |
| Min/max recall | not started — next |
| SD datalogging | not started; SD mounting has its own internal-memory cost to measure |

## M7 — Configuration web app 🚧 in progress

- A face editor published to GitHub Pages alongside the existing browser installer
- Emits schema-conformant XML and uploads it over the M4 HTTP API
- Live preview, so a face can be designed without flashing anything

**Exit criteria:** the editor is live on GitHub Pages; a face designed in it, including custom
shapes, is uploaded to a gauge and renders as previewed with 0 warnings; a shaped needle's frame
cost is measured and still holds the frame budget.

**Progress:**

| Item | State |
|---|---|
| Custom major/minor tick, needle and hub shapes in the schema ([ADR 0006](adr/0006-custom-shapes-as-polygons.md)) | parser, rasteriser and renderer done; 192 host checks pass; firmware builds. **Not yet run on hardware**; frame cost of a shaped needle owed ([performance.md](performance.md)) |
| Authoring interface document for the web app | done: [gauge-xml-interface.md](gauge-xml-interface.md) |
| Face editor ([config-app.md](config-app.md), [ADR 0007](adr/0007-face-editor-static-app-with-parser-port.md)) | **built**: every schema element, shape editor with SVG path import, live preview with damping/alerts/peak, validation, import/export, browser library. 47 tests; the parser port matches the firmware parser on all 480 corpus documents. Checked by rendering in headless Chrome, **not yet used by a person on a real browser session, and not yet deployed** |
| GitHub Pages publishing | `pages.yml` written: editor at `/editor/`, flashing page at `/` with release firmware; `release.yml` now calls it. **Not yet run**: needs a push to `main`, and the `github-pages` environment must allow `main` |
| Browser access to the device ([ADR 0008](adr/0008-gauge-serves-face-editor.md)) | **verified on hardware**: the firmware embeds the gzipped editor and serves it at `/editor/`, adds raw XML download and loopback-only CORS. 66.7 fps unchanged, `httpd` stack and internal heap measured ([performance.md](performance.md)) |
| Managing a gauge's faces from the editor | **verified on hardware** in Chrome: list with the face on screen marked, add, update with confirmation, edit without renaming, delete, and deleting the face on screen switches the gauge to another. Firmware refuses a mismatched id and a 13th face. Lowest `httpd` stack 2,192 B ([performance.md](performance.md)) |
| Upload from the editor | **verified on hardware** in Chrome: `boost_custom` uploaded from the gauge-served editor with 0 warnings reported by the gauge, a face opened back from the gauge, and the dev-server path worked by address. **Not yet used by a person** |

## M8 — Portability

Prove the `board_profile` seam is real rather than theoretical.

- A second profile for a square or differently-sized panel
- Proportional scaling when a face's authored `<panel>` does not match the running panel
- Re-measure scenario 2 on the second panel

---

## Not scheduled

Worth recording so they are decisions rather than oversights:

- **Snapshot-based screen transitions.** The swipe currently costs ~33 fps. The fallback is
  designed and documented in [display-pipeline.md](display-pipeline.md) but deliberately not
  implemented: a dip during a deliberate gesture is an acceptable trade, and the needle is
  unaffected.
- **Signed OTA images.** Needed before the HTTP API is exposed to anything untrusted.
- **Bluetooth.** The hardware supports it; nothing in the product needs it.
- **The IMU, RTC and battery telemetry.** Present on the board, unused. The RTC could
  timestamp datalogs if M6 grows that far.
