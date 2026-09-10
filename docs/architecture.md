# Architecture

## Components

| Component | Responsibility |
|---|---|
| `board_profile` | Panel geometry (shape, resolution, safe area) and which peripherals exist. The portability seam. |
| `gauge_config` | Parse gauge XML into `gauge_config_t`; validate; supply defaults. No LVGL, no I/O — pure data, so it is testable on the host. |
| `gauge_store` | Owns the LittleFS `storage` partition: list, load, save and delete configs. Keeps filesystem concerns out of `gauge_config`. |
| `gauge_render` | Build the LVGL dial from a `gauge_config_t`: pre-rendered face, needle sprite, readouts, alerts. |
| `sensor_hub` | Own `I2C_NUM_1`; drive ADS1115 and MCP9600; scale, filter and publish readings. Also hosts the simulated and network sources. |
| `app_settings` | NVS-backed user settings (active gauge, brightness, units, overlay toggle). |
| `net_svc` | WiFi provisioning, HTTP server, mDNS, telemetry ingest, OTA. |

Dependency direction is strictly one-way:

```
main
 ├── app_ui       ──> gauge_render, gauge_store, gauge_perf, app_settings
 ├── gauge_render ──> gauge_config, board_profile
 ├── gauge_store  ──> gauge_config
 ├── sensor_hub   ──> (nothing above it)
 ├── net_svc      ──> gauge_store, app_settings
 └── app_settings
```

`gauge_config` and `board_profile` are leaves and depend on neither LVGL nor ESP-IDF drivers.
That is deliberate: it keeps the schema work unit-testable on a host machine, with no board
attached.

## Task model

| Task | Core | Prio | Responsibility |
|---|---|---|---|
| `lvgl_port` (from `esp_lvgl_port`) | 1 | 4 | LVGL timer handler and flush. Owns the LVGL mutex. |
| `sensor_hub` | 0 | 5 | I2C polling, scaling, filtering, snapshot publish |
| `net_svc` | 0 | 3 | WiFi, HTTP, mDNS, telemetry, OTA |

The LVGL task gets **core 1 to itself**. Everything that can block — I2C transactions, WiFi,
flash writes — lives on core 0. This is the arrangement that keeps jitter out of the needle.

`sensor_hub` runs at a *higher* priority than `net_svc` despite being less urgent in the
abstract, because sampling jitter shows up as visible needle noise, whereas a slow HTTP
response does not.

## Data flow

```
ADS1115 / MCP9600 ─┐
simulated source  ─┼─> sensor_hub ─> channel snapshot ─> LVGL timer ─> gauge_render
UDP telemetry     ─┘                  (lock-free)         (core 1)
```

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
- **Tile 1 — settings.** WiFi status and provisioning, gauge selection, brightness, units,
  FPS overlay toggle, firmware version, and any config-load warnings.

`lv_tileview` gives momentum-tracked swipe-up for free. The transition is the most expensive
thing the UI does — see the worst-case analysis in
[display-pipeline.md](display-pipeline.md).

## Startup

1. `bsp_display_start()` — brings up the QSPI panel, touch, and the LVGL port.
2. Mount LittleFS on the `storage` partition. A failure here is non-fatal.
3. `app_settings` loads from NVS; defaults on first boot.
4. `gauge_store` loads the active gauge XML from LittleFS. If it is missing or malformed the
   next available config is tried, and the compiled-in default face is the last resort.
   Whatever happened is surfaced on the settings page, not just in the serial log.
5. `gauge_render` pre-renders the face and builds the screen.
6. `sensor_hub` starts (real sources if the I2C front-end responds, simulated otherwise).
7. `net_svc` starts last — nothing in the display path waits on the network.

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
