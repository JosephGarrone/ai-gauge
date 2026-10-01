# Architecture

## Components

| Component | Responsibility |
|---|---|
| `board_profile` | Panel geometry (shape, resolution, safe area) and which peripherals exist. The portability seam. |
| `gauge_config` | Parse gauge XML into `gauge_config_t`; validate; supply defaults. No LVGL, no I/O — pure data, so it is testable on the host. |
| `gauge_store` | Owns the LittleFS `storage` partition: list, load, save and delete configs. Keeps filesystem concerns out of `gauge_config`. |
| `gauge_shape` | Geometry and anti-aliased rasterisation of custom tick, needle and hub shapes ([ADR 0006](adr/0006-custom-shapes-as-polygons.md)). No LVGL, no I/O — tested on the host. |
| `gauge_render` | Build the LVGL dial from a `gauge_config_t`: pre-rendered face, needle sprite, readouts, alerts. |
| `sensor_hub` | Own `I2C_NUM_0`; drive ADS1115 and TMP1075 (EGT linearisation + cold-junction compensation); scale, filter and publish readings; own the sensor calibration in NVS. Pure maths in `sensor_math.c`, host-tested. See [ADR 0009](adr/0009-sensor-hub-sampling-and-calibration.md). |
| `app_settings` | NVS-backed user settings (active gauge, brightness, rotation, FPS badge). |
| `app_ui` | Tileview screens: the dial, and the swipe-up settings tile (`app_ui_settings.c`, [settings-ui.md](settings-ui.md)). Also remote screenshots and gestures (`app_ui_remote.c`). |
| `net_svc` | WiFi provisioning, HTTP server, mDNS, telemetry ingest, OTA. |

Dependency direction is strictly one-way:

```
main
 ├── app_ui       ──> gauge_render, gauge_store, gauge_perf, app_settings, sensor_hub
 ├── gauge_render ──> gauge_config, gauge_shape, board_profile
 ├── gauge_store  ──> gauge_config
 ├── sensor_hub   ──> (nothing above it)
 ├── net_svc      ──> gauge_store, app_settings
 └── app_settings
```

`gauge_config`, `gauge_shape` and `board_profile` depend on neither LVGL nor ESP-IDF drivers.
That is deliberate: it keeps the schema work unit-testable on a host machine, with no board
attached.

## Task model

| Task | Core | Prio | Responsibility |
|---|---|---|---|
| `lvgl_port` (from `esp_lvgl_port`) | 1 | 4 | LVGL timer handler and flush. Owns the LVGL mutex. |
| `sensor_hub` | 0 | 5 | I2C sampling, scaling, filtering, snapshot publish. 4KB stack **in PSRAM** (2.7KB unused, measured), so it never writes flash |
| `net_svc` | 0 | 3 | WiFi, HTTP, mDNS, telemetry, OTA |

The LVGL task gets **core 1 to itself**. Everything that can block — I2C transactions, WiFi,
flash writes — lives on core 0. This is the arrangement that keeps jitter out of the needle.

`sensor_hub` runs at a *higher* priority than `net_svc` despite being less urgent in the
abstract, because sampling jitter shows up as visible needle noise, whereas a slow HTTP
response does not.

## Data flow

```
ADS1115 / TMP1075 ──> sensor_hub ──> snapshot (lock-free) ─┐
UDP telemetry ──> net_svc ──> latest value (spinlock) ────────┼─> value_timer_cb ─> gauge_render
simulated sweep (until a board has answered) ─────────────────┘   (LVGL task, 5ms)
```

`value_timer_cb()` in `app_main.c` picks one source per tick. First, telemetry for the face's
channel, if it arrived in the last second. Then `sensor_hub`, once a board has answered. Then the
simulated sweep, if built in, until a board has answered. Otherwise the channel is invalid.
`sensor_hub` publishes in native units (kPa, °C, V); the face's `unit` converts them.

### The snapshot rule

**`sensor_hub` and `net_svc` never call LVGL.** They publish into a small per-channel
snapshot:

```c
typedef struct {
    float    value;        // in the channel's native unit
    bool     valid;        // false if the sensor is absent, faulted or stale
    int64_t  timestamp_us; // esp_timer_get_time() at sample
} gauge_reading_t;
```

Publication is a double-buffered slot with a release-store on the index; the reader does an
acquire-load and reads the other buffer. No mutex, so a producer can never stall the render
task, and the render task can never be blocked behind an I2C transaction.

An LVGL timer on the UI task polls the snapshot at the display's rate and pushes the value
into the needle. Sample rate and frame rate are fully decoupled — the sensor can run at 250Hz
or 4Hz and the needle animates smoothly either way.

Staleness is the reader's problem: if `timestamp_us` ages past a threshold the channel is
shown as invalid (dashes rather than a stale number). A gauge that silently displays an old
value is worse than one that admits it has lost the sensor.

### Channels

A *channel* is a named value (`boost`, `egt`) with a unit. Its source is interchangeable:
a real sensor, the simulator, or a UDP telemetry feed. `gauge_render` cannot tell the
difference, which is what lets the full UI be developed and demonstrated on a bare board with
no sensors attached — and what makes remote telemetry feeds a first-class feature rather than
a bolt-on.

## Screens

An `lv_tileview` with two vertically stacked tiles:

- **Tile 0 — dial.** The gauge. Optimised per [display-pipeline.md](display-pipeline.md).
  A tap (`LV_EVENT_SHORT_CLICKED`) clears the peak. A long-press shows the session min/max for
  3s, and a second long-press while shown resets them. `SHORT_CLICKED` rather than `CLICKED`
  because LVGL also sends `CLICKED` when a long press is released. Min/max lives in
  `gauge_render`, so it resets on reboot and on switching faces; persisting it needs the
  ignition-drop hold-up discussed in [rear-pcb.md](rear-pcb.md).
- **Tile 1 — settings.** WiFi status and provisioning, gauge selection, brightness, screen
  rotation, frame-rate limit (30 / 45 / 60 / Off), units, FPS overlay toggle, live sensor readings with *Zero boost now*, sensor
  calibration (one picker and one −/+ pair, for the RAM reason in ADR 0009), firmware version,
  and any config-load warnings. Sized for
  a finger on a 1.75" panel: 26px text or larger, and controls at least 54px tall.

Rotation is applied by the panel itself (MADCTL), not by LVGL: the adapter never rotates frames
for this QSPI panel, and a hardware rotation costs nothing per frame. Because the panel is
square, LVGL's resolution is unchanged; `lv_display_set_rotation()` is used only so LVGL maps
touch input to match.

`lv_tileview` gives momentum-tracked swipe-up for free. The transition is the most expensive
thing the UI does — see the worst-case analysis in
[display-pipeline.md](display-pipeline.md).

## Startup

1. `bsp_display_start()` — brings up the QSPI panel, touch, and the LVGL port.
   The boot splash goes up straight after, over everything built below it, and the panel is only
   switched on once that first frame is drawn ([boot-splash.md](boot-splash.md)).
2. Mount LittleFS on the `storage` partition. A failure here is non-fatal.
3. `app_settings` loads from NVS; defaults on first boot.
4. `gauge_store` loads the active gauge XML from LittleFS. If it is missing or malformed the
   next available config is tried, and the compiled-in default face is the last resort.
   Whatever happened is surfaced on the settings page, not just in the serial log.
5. `gauge_render` pre-renders the face and builds the screen.
6. `sensor_hub` starts. It never fails for a missing board: it keeps probing every 2s.
7. A 1s pause, so the first frames' transient allocations are gone before WiFi claims its
   buffers ([ADR 0009](adr/0009-sensor-hub-sampling-and-calibration.md)).
8. `net_svc` starts last — nothing in the display path waits on the network. Inside it, the HTTP
   server starts before WiFi initialises, for the memory reason in [performance.md](performance.md).
9. Once the ADC has a reading, the splash is released to hand over to the dial.

Allocation order is load-bearing on this board: long-lived internal allocations first, WiFi last.

The ordering matters: **the gauge shows a needle before the network is touched.** A driver
turning the ignition on should see the gauge immediately, regardless of WiFi state.

## Failure behaviour

| Failure | Behaviour |
|---|---|
| Malformed / missing gauge XML | Fall back to compiled-in default face; warning on settings screen |
| LittleFS mount failure | Compiled-in default face; warning on settings screen |
| Sensor absent or faulted | Channel marked invalid; readout shows dashes, needle parks at minimum |
| WiFi unavailable | Gauge runs normally; settings screen shows disconnected |
| Bad OTA image | Rejected before activation; rollback via `esp_ota` on boot failure |

The governing principle: **nothing that fails outside the render path is allowed to stop the
gauge from displaying.** This is a device someone relies on while driving.
