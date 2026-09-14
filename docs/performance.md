# Performance

60fps+ is a hard requirement. This document records **how it is measured** and **what has
actually been measured on hardware**.

> **Rule:** nothing in the *Results* section may be filled in from theory, estimation or a
> desktop simulator. Only numbers observed on a real board go there. Predictions live in the
> *Budget* section and are labelled as such.

## Budget (theoretical)

| Quantity | Value |
|---|---|
| Frame period at 60fps | 16.6 ms |
| Full frame, 466×466 RGB565 | 434,312 bytes |
| QSPI throughput (80MHz × 4 lanes, theoretical) | ~40 MB/s |
| **Full-frame blit** | **~11 ms** (~66% of budget) |
| Needle sprite (~32×200 ARGB8888) rotation | ~6,400 px |
| Expected steady-state dirty region | <15% of screen |

Derivation and the design that follows from it: [display-pipeline.md](display-pipeline.md).

## Method

### On-screen overlay

`CONFIG_LV_USE_PERF_MONITOR` shows FPS and CPU load, toggleable from the settings screen.
Convenient, but it measures LVGL's own view of the world — treat it as an indicator, not
evidence.

### Instrumented flush

The two numbers that actually predict affordability are accumulated in the flush callback:

- **Bytes transferred per frame** — the QSPI cost.
- **Dirty area per frame** (px², and as a % of the screen) — the render cost.

Both are reported over the serial log and via `GET /api/status`.

### Standard test scenarios

Run each for at least 30 seconds and record min / mean / max.

| # | Scenario | What it isolates |
|---|---|---|
| 1 | Static needle, no input | Baseline idle cost |
| 2 | Needle sweeping min→max at 1Hz (simulated source) | Steady-state gauge cost — **the number that matters** |
| 3 | Needle at realistic boost rate-of-change | Representative driving |
| 4 | Swipe-up transition, repeated | The known worst case |
| 5 | Scenario 2 with WiFi connected and telemetry streaming | Cross-core interference |
| 6 | Scenario 2 with the FPS overlay enabled | Cost of the instrumentation itself |

Scenario 5 matters more than it looks: WiFi runs on core 0, LVGL on core 1, and the claim that
they do not interfere is an assumption until it is measured.

## Results

Measured on hardware: ESP32-S3 rev v0.2, 240MHz, 8MB octal PSRAM @80MHz, QIO flash @80MHz,
466x466 CO5300 panel. Scenario 2 (needle sweeping full scale continuously), simulated source
ticking every 5ms so the needle differs on every frame.

| Date | Firmware | Scenario | FPS | Dirty % | Bytes/frame | Render ms | Notes |
|---|---|---|---|---|---|---|---|
| 2026-09-10 | needle as rotated `lv_image` | 2 | **45.4** | 28.8% (62,632 px) | 125,264 | 11.65 | **Failed the gate.** See below. |
| 2026-09-10 | needle custom-drawn, tight invalidation | 2 | **66.2** | 13.3% (28,900 px) | 57,900 | 5.80 | **Passes.** |
| 2026-09-10 | + tileview screens | 2 | **66.6** | 13.2% (28,700 px) | 57,400 | 5.86 | No regression from adding the screens. |
| 2026-09-10 | + tileview screens | 4 | **33.4** | 46.2% (100,200 px) | 200,500 | 15.76 | Transition. Accepted worst case — see below. |
| 2026-09-14 | + net_svc, radio up (setup AP), internal flush buffers | 5 (partial) | **66.6** | 11.8% (25,600 px) | 51,200 | 6.12 | Radio active, no client traffic. See below. |
| 2026-09-14 | sim off, telemetry sweep at 58.6Hz over WiFi (STA) | 5 | **58.6** | 13.7% (29,800 px) | 59,700 | 7.59 | Every datagram rendered; rate equals the input rate, not a ceiling. |
| 2026-09-14 | + redraw fix, sim on, WiFi STA connected | 2 | **66.7** | 13.0% (28,200 px) | 56,300 | 6.91 | No regression from WiFi or the fix. |

**The gate passes at 66.2 fps.** That figure is the LVGL refresh-period ceiling
(`CONFIG_LV_DEF_REFR_PERIOD=15` gives 1000/15 = 66.7 fps), not a rendering limit: rendering
consumes 5.8ms of each 15ms period, about 39% utilisation, so there is real headroom above
the 60fps requirement rather than a result that only just scrapes in.

Other measurements from the same run:

- Face pre-render: **36 ms**, once at startup. Zero per-frame cost thereafter, as designed.
- Face buffer: 424KB in PSRAM. Free after startup: 6,874KB PSRAM, 165KB internal.
- First measurement window shows a ~35ms max render: that is the initial full-screen paint,
  not steady state.
- Switching gauges at runtime (tear down, re-rasterise the face, rebuild): **~41 ms**. Eight
  consecutive switches left PSRAM free unchanged at 6,866 KB, so the 424 KB face buffer is
  released cleanly.

### What the first attempt got wrong

ADR 0003 originally specified the needle as a small ARGB8888 sprite rotated with
`lv_image_set_rotation()`, predicting under 15% dirty area. Measured, it produced **28.8%**
and only 45 fps.

The cause: LVGL grows a transformed object's invalidation area using `ext_draw_size`, which
is a **single scalar applied on all four sides**. A 14x170 needle pivoting about its end
therefore invalidates a square roughly 354x354 -- essentially the whole dial -- no matter how
few pixels the needle itself covers. The sprite was small; its rotation envelope was not.

The fix was to drop image rotation and draw the rotated triangle directly from a
`LV_EVENT_DRAW_MAIN` callback, with the needle object left stationary and covering the whole
sweep, and only two tight rectangles invalidated per update: where the needle was, and where
it now is. Dirty area fell to 13.3% and render time roughly halved.

Two smaller fixes in the same change:

- The readout label was full panel width (466px), so every value change dirtied a
  466 x 56 strip. It is now sized to its content.
- The instrumentation itself was wrong at first: it summed `LV_EVENT_INVALIDATE_AREA`
  events, which are per-request and overlap freely, and reported over 100% of the screen per
  frame. It now measures `LV_EVENT_FLUSH_START` areas, which are what actually crosses the
  QSPI bus. **A metric that can report 103% is not measuring a real quantity** -- worth
  remembering before trusting any number here.

### Scenario 4: the swipe transition

Measured with `CONFIG_AI_GAUGE_BENCH_TRANSITION=y`, which flips tiles every 1.2s so the
transition is a repeatable input rather than a human swiping.

**~33 fps** across the measurement window, which mixes animating frames with settled ones.
The animating frames themselves are worse: max render sits at **34.5ms** with the full
217,156 px invalidated, so during the slide the display runs at roughly **29 fps**.

This is the compromise [display-pipeline.md](display-pipeline.md) anticipated and accepted.
Sliding two full-screen tiles past each other cannot avoid redrawing everything, and a dip
during a deliberate user gesture is far less costly than any dip in the needle's steady-state
motion. The needle is unaffected: scenario 2 still measures 66.6 fps with the tileview in
place.

If the transition ever needs to be smoother, the fallbacks are listed in
[display-pipeline.md](display-pipeline.md) -- snapshotting both tiles at gesture start and
sliding the snapshots, so the transition becomes a pure blit. That work has **not** been done.

### Scenario 5: WiFi active (partial)

Measured with the setup access point up, the HTTP server listening, mDNS advertising and the
telemetry socket bound — but **with no client connected and no traffic flowing**. That is
radio overhead only. The full scenario, with a telemetry stream running, is still owed.

**66.6 fps, 11.8% dirty, 6.12ms render.** The claim that WiFi on core 0 would not disturb
LVGL on core 1 holds for CPU time.

It did **not** hold for memory, and that was the real finding. The first build with WiFi
enabled did not drop frames — it stopped drawing entirely:

```
E lcd_panel.io.spi: panel_io_spi_tx_color(395): spi transmit (queue) color failed
E esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_NO_MEM
```

The chain of causes, each confirmed by measurement or by reading the driver source:

1. The BSP puts LVGL's flush buffers in **PSRAM**. PSRAM is not DMA-capable for SPI, so the
   SPI master driver allocates a **~46KB internal bounce buffer and copies every flush into
   it**. This had been happening on every frame since M2, invisibly, because a large enough
   internal block happened to be free.
2. WiFi's static RX buffers must live in internal DMA memory, and bringing the radio up cut
   the largest free DMA block to ~20KB — below the bounce buffer's size.

The fix, and the dead ends on the way:

| Change | Result |
|---|---|
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` | Display survived, but WiFi then failed to init: the option raises static RX buffers to 16 |
| Static RX buffers 16 -> 6, block-ack window 6 | WiFi up; display broke again, internal free 36KB |
| **Flush buffers moved to internal RAM** via `lv_display_set_buffers()` | **Bounce buffer and per-frame copy eliminated.** Display works with WiFi up |
| Flush buffers 20 -> 12 lines | LVGL task hung with no error. **Reverted to 20**; cause unknown |
| lwIP buffers and sockets trimmed | HTTP server could start |
| `SPIRAM_USE_CAPS_ALLOC` | Wrong direction: kept plain `malloc()` internal. Task watchdog fired |
| **`SPIRAM_USE_MALLOC`**, 4KB internal threshold, 32KB reserve | Full stack up at 66.6 fps |

**Headroom is thin.** After startup, internal free heap is ~3KB with a largest free DMA block
of a few hundred bytes. It is stable — the flush buffers are already allocated and nothing
on the render path allocates internal memory any more — but OTA, a reconnect, or a config
upload could push something over. A 1KB internal threshold was built to test for more
headroom but **not measured**, because the board was disconnected; the verified 4KB value is
what is committed.

### Scenario 5: telemetry streaming over WiFi

Measured with the gauge joined to a home network as a station, the simulated source compiled
out, and this PC streaming UDP telemetry to it. Three phases:

| Phase | Input | Drawn | Reading |
|---|---|---|---|
| A | none, 12s | **no frames** | Nothing redraws with no input — the sweep really is off |
| B | sine sweep, 1173 datagrams in 20s (58.6Hz) | **58.6 fps**, 13.7% dirty, 7.6ms render | Frame rate equals the send rate exactly: every datagram was rendered, none dropped |
| C | constant 22.5 psi at 20Hz, 14s | **16—17 fps** of an unchanged needle | Wasted work — see below |

Phase B shows the renderer keeps up with a 60Hz feed while WiFi is carrying it. It does not
measure a ceiling: the input, not the renderer, set the rate.

**Phase C exposed a real inefficiency** the always-moving simulated source had hidden.
`gauge_render_set_value()` invalidated the needle and readout on every call, even once damping
had settled and nothing on screen changed. A real sensor at idle would have repainted the gauge
continuously at its full sampling rate for no visible change. The needle now skips
invalidation when its geometry lands on the same pixels, and the readout skips
`lv_label_set_text()` when the text is identical.

**Verified after the fix**, same protocol (sweep, then hold 22.5 psi at 20Hz): the hold settled
within one 5-second window — 3.0 fps, the damping tail — and every window after that reported
**no frames drawn** while the feed kept arriving. The unfixed build drew 16—17 fps for as long as
the feed ran.

Internal heap during the whole run held at ~8—9KB free, better than the ~3KB seen with the
setup access point up.

### Audio: the stock BSP setup does not fit alongside WiFi

Measured with a temporary build that started the ES8311 speaker through the BSP after WiFi.

`bsp_audio_init()` always creates **both** a speaker and a microphone I2S channel, each with the
driver's default six DMA buffers of 480 bytes. DMA buffers must be internal memory. With WiFi
running, internal free heap was ~8KB but the **largest free DMA block was only 480—2,688
bytes**, and the allocation failed:

```
E i2s_common: i2s_alloc_dma_desc(510): allocate DMA buffer failed
E ESP32-S3-Touch-AMOLED-1.75: bsp_audio_init(222): I2S channel initialization failed
```

The BSP is built with `CONFIG_BSP_ERROR_CHECK`, so that failure is an **abort, not an error
return**: the board panicked about 2.7s into every boot. The test image also held a 4.4KB tone
table in `.bss`, which is internal RAM, and that alone was enough to stop the HTTP server
starting. Static buffers of that size must not live in internal memory on this board.

Consequences for the M6 chime:

- Do not call `bsp_audio_init()` or `bsp_audio_codec_speaker_init()`.
- Create a speaker-only I2S channel directly, with small DMA buffers, and allocate it before WiFi
  claims its share of internal memory — the same approach that made the flush buffers fit.
- Generate the chime into PSRAM, never into a static internal array.

**Result, measured on hardware.** `app_audio` creates a speaker-only channel with two 512-byte
DMA buffers, drives the ES8311 directly, and synthesises an 11.8KB two-note chime into PSRAM,
all before WiFi starts. The chime plays on every alert crossing, rate-limited to one per 3s,
and the dial stayed at **66.6 fps**.

**No sound has actually been heard.** A test image at full volume with a near full-scale chime,
explicitly unmuted, played with no codec errors but was inaudible. This unit most likely has no
speaker fitted, so the chime is verified only as far as the codec, not as audible output.

It did not fit cleanly at first. With audio claimed, the HTTP server — started after WiFi
init — could no longer find a contiguous ~6KB block for its task stack, and `httpd_start()`
failed, leaving the gauge with no API and no OTA. Starting the server **before** WiFi
initialises fixed it. The rule that emerges: **long-lived internal allocations first, WiFi last.**

Headroom is now razor thin. Immediately after WiFi starts, internal free heap dips to ~650B with
a largest free block of ~60B, then settles at ~5.3KB. It has proved stable, including a full OTA
update during which it remained the running image, but anything that adds internal allocations
must be measured. The HTTP request body and OTA buffers are now explicitly PSRAM, since both sat
at or under the 4KB threshold below which plain `malloc()` still takes internal memory.

### WiFi static RX buffers

Adding peak-hold stopped WiFi starting at all:

```
E wifi:Expected to init 6 rx buffer, actual is 5
E net_svc: net_svc_start(356): wifi_init
```

The obvious explanation — the new UI objects taking internal memory WiFi needed — did not survive
a test. Starting WiFi *before* building the UI made it worse, initialising only 4 of 6, even
though a 32KB internal block was free at that moment. So this is not simple fragmentation by UI
objects. The UI-first order was restored.

As a stopgap, `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` was cut to 4. WiFi connected and the dial
held 66.7 fps, but the cause was unexplained, so it was investigated.

**What the diagnostic showed.** A temporary build with 6 static RX buffers hooked
`heap_caps_register_failed_alloc_callback()` and dumped the internal heap either side of
`esp_wifi_init()`:

| Measurement | Value |
|---|---|
| Failing allocation | **1,604 bytes**, caps `0x80c` (8BIT + DMA + INTERNAL) — the 6th static RX buffer |
| Internal DMA free before `esp_wifi_init()` | 50.9KB, largest block 31.7KB |
| Internal DMA free at the failure | 2.7KB, largest block 1.4KB |
| `SPIRAM_MALLOC_RESERVE_INTERNAL` pool during init | drained to 1,427B free — WiFi can and does use it |
| RTC fast RAM | 7.7KB free throughout, but not DMA-capable, so unusable here |

So this is **not fragmentation, and the reserve pool is not a trap**. WiFi's own initialisation
consumes roughly 48KB of internal DMA-capable memory before it reaches its last RX buffer, and the
total left over after the display, audio, HTTP server and every task stack is simply smaller than
that. Real headroom has to come from removing other internal consumers; FreeRTOS task stacks are
the prime suspects, since ESP-IDF forces them into internal memory.

**Task stacks, measured.** A second temporary build dumped `vTaskList()` high-water marks every 20s
through boot, the needle sweep, a config upload with live reload, and an OTA:

| Task | Stack | Peak used | Notes |
|---|---|---|---|
| `swdraw` ×2 (LVGL draw threads) | 8,192 each | ~1.7KB | LVGL creates them with plain `xTaskCreate()`, so internal |
| `lvgl` (adapter task) | 8,192 | ~3.7KB | Adapter default `stack_in_psram = false` |
| `httpd` | 6,144 | **~5.5KB** | Only 668B left after a config upload |
| `wifi` | 6,656 | ~3.2KB | Owned by the driver |

**The fix, measured on hardware:**

1. `CONFIG_LV_DRAW_THREAD_STACK_SIZE` 8,192 → **4,096**, freeing 8KB of internal RAM. The draw
   threads still keep ~2.4KB free after boot, face pre-render, config reload and a sustained sweep.
2. `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` restored to **6**. All six initialise and WiFi connects.
3. **The HTTP server's near-overflow was a separate bug the dump uncovered.** A config upload put
   three `gauge_config_t` (924 bytes each) on the server task's stack: one validating the save, and
   two more in the reload callback, which runs on that same task and also rebuilds the face. All
   are now allocated in PSRAM, as is `gauge_store`'s file buffer, which plain `malloc()` had been
   placing in internal RAM because it is under 4KB. Minimum free `httpd` stack after an upload
   went from **668B to 2,500B**.

With all three: 66.7 fps, internal free heap ~8.6KB after startup (lowest ever 2.2KB), OTA
succeeds and the new image confirms itself, and all 17 endpoint checks pass. Internal RAM is still
the scarcest resource on the board, so anything that adds a task, a stack, or a small `malloc()`
must be measured. The method above (a failed-allocation callback plus a periodic `vTaskList()`
dump) is the quickest way to find where it went.

### Custom shapes (ADR 0006): not yet measured

Implemented 2026-09-14. **Built and host-tested only**: no board was attached, so nothing below
has been verified on hardware.

**Render path.**
- A shaped needle is rasterised in software on the LVGL task whenever its tip moves by at least
  1/8 px. The cost scales with each part's bounding-box area: about 17,000 px for a 176px
  needle at 45°, plus the A8 blend. Invalidation is unchanged, still the old box plus the new
  one.
- Shaped ticks and hubs are rasterised once, when the face is built.

**Memory.**
- `gauge_config_t` grew from 924 to 2,308 bytes.
- Every copy that sat in internal RAM now goes to PSRAM:
  - the active config in `app_main` (a static, so internal `.bss`)
  - the built-in default (also a static)
  - the renderer struct that embeds a copy (a sub-4KB `calloc()`, so internal)
  - the config loaded when switching gauges (on the LVGL task's stack)
- By arithmetic, not measurement, internal RAM use should *fall* by about 3KB, plus 924 bytes
  of transient LVGL-task stack.
- A shaped needle also holds one worst-case A8 mask per part and a float scratch buffer, all in
  PSRAM.

**Owed on hardware:**
- Scenario 2 with `boost_custom`: fps, dirty %, render ms
- Internal free heap after startup with WiFi, compared with the 8.6KB recorded above
- A live switch between `boost` and `boost_custom`

### Not yet measured
- Scenarios 1, 3 and 6.
- Scenario 2 with a custom needle shape (above).

## The gate

**Scenario 2 must hold ≥60fps with CPU headroom before sensor and networking work is layered
on.** If the render strategy cannot meet budget, that must be discovered while it is still
cheap to change the approach — not after everything else has been built on top of it.

**Status: passed** at 66.2 fps with 39% render utilisation — on the second attempt. The
first attempt failed at 45 fps, which is precisely why this gate exists before the rest of
the system is built on top of the renderer.

If a future change fails the gate, the fallbacks are in
[display-pipeline.md](display-pipeline.md).

## When something gets slower

1. Check whether dirty area or bytes/frame changed — that distinguishes a rendering
   regression from a transfer one.
2. Check whether a new UI element broke one of the rules in
   [display-pipeline.md](display-pipeline.md) — full-screen gradients and large translucent
   objects are the usual culprits.
3. Confirm the performance-critical config is still set: `CONFIG_COMPILER_OPTIMIZATION_PERF`,
   `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2`, `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`, the octal
   PSRAM settings, and 240MHz CPU.
4. Confirm the flush buffers are still in internal SRAM, not PSRAM. This one is easy to break
   accidentally and expensive when broken.
