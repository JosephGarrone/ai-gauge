# Face Editor (Configuration Web App)

The M7 web app: design a gauge face in the browser, see it move, find out exactly what the gauge
will make of it, and export the XML. Lives in [`tools/config-app/`](../tools/config-app/). It is
published to GitHub Pages at **`/editor/`**, next to the flashing page, and built into the firmware
so every gauge serves it at **`http://<gauge>/editor/`**, which is where it uploads from.

It is written against [gauge-xml-interface.md](gauge-xml-interface.md). The decisions behind its
shape are in [ADR 0007](adr/0007-face-editor-static-app-with-parser-port.md).

## What it does

| Feature | Notes |
|---|---|
| Edit every schema element | Gauge and source, dial, bands, ticks, labels, needle, titles, readout, peak hold, alerts |
| Custom shapes | All four slots (major/minor tick, needle, hub): drag vertices, double-click an edge to add one, circles, presets, per-part colours, and **SVG path import** with curve flattening to 0.1 px and simplification to the 64-vertex budget. The canvas shows neighbouring ticks and the tested needle footprint |
| Live preview | SVG rendering of the rendering model in §5, with the firmware's damping, alert flashing (including a quirk, below), and peak hold. Sweep, set a value, reset the peak. Drag titles, the readout and the peak value to place them; click any part of the face to edit it. Optional construction guides |
| Validation | Every forgiven condition in §6 is reported as an error before export, plus warnings for things that parse but are probably wrong (ticks running into each other, alerts that can never fire, over-long label text, non-ASCII glyphs) and the needle performance guidance in §4.6 |
| Device verdict | The XML is re-read with a port of the firmware parser: "device reports 0 warnings" is a prediction made by the same logic as the firmware, not an approximation |
| Library | Faces are kept in the browser's `localStorage`. Import (file picker or drag and drop), download, duplicate, delete; undo and redo |
| Gauge | Opened from a gauge: upload the face and read back the gauge's warning count, open faces stored on the gauge, delete them; see below |

Deep links: `editor/#needle` opens a section; `editor/?example=boost_custom.xml` opens a shipped
face.

## How it is built

Static files, native ES modules, **no dependencies and no build step**. Montserrat is loaded from
Google Fonts for the preview; everything else is in the repository.

| Module | Role | Tested under Node |
|---|---|---|
| `js/schema.js` | Limits, fonts and line heights, defaults, error strings | via the others |
| `js/parse.js` | **Line-for-line port of `gauge_config.c`**: same XML subset, clamps, truncation by bytes, float32 storage, warning count | yes, plus the firmware differential test |
| `js/serialize.js` | Model to canonical XML (§3 order, ≤4 decimals, shapes on the 1/16 px grid) | yes |
| `js/validate.js` | Editor checks (§2, §4.5, §6), with the parser as a backstop | yes |
| `js/geometry.js` | Rendering maths from `gauge_render.c` and `gauge_shape.c`; self-intersection, RDP simplification, SVG path flattening | yes |
| `js/printf.js` | The `%f` / `%e` / `%g` subset formats may use, rounding ties to even like C | yes, and cross-checked against Python's C-style formatting on 4,000 cases |
| `js/render.js` | SVG layers in the firmware's stacking order | yes |
| `js/sim.js` | `gauge_render_set_value()`: damping, peak, alerts | yes |
| `js/store.js` | Browser storage | no (browser-only) |
| `js/device.js` | HTTP API client; recognising a page served by a gauge | addresses and detection; requests are browser-only |
| `js/ui.js`, `js/shape-editor.js`, `js/app.js` | The interface | no; checked by rendering in headless Chrome |

### Staying in step with the firmware

The parser port is only worth having if it matches the C exactly. `tools/host-tests` builds
`gauge_config_dump`, which prints what `gauge_config_parse()` produces as JSON in the editor's model
shape. `test/firmware-diff.test.mjs` runs both parsers over a corpus and compares every field and
the warning count:

- the shipped faces in `firmware/assets/gauges/`
- every case from `test_gauge_config.c`, plus parser quirks (single quotes, `>` in values,
  duplicate and unquoted attributes, doctype, bare `<` in text, hex numbers, repeated elements,
  UTF-8 cut mid-character)
- 400 seeded random documents built from the schema's vocabulary with hostile values
- every one of those documents re-written by the serializer

CI runs this in `build.yml`. Without `GAUGE_CONFIG_DUMP` set the test is reported as skipped, not
passed.

**When the schema changes**, update `schema.js`, `parse.js`, `serialize.js`, `validate.js` and
`render.js` together with the firmware, extend `gauge_config_dump.c` for any new field, and add
cases to `test/corpus.mjs`. See *Adding to the schema* in
[gauge-config-schema.md](gauge-config-schema.md).

### What the preview does not reproduce exactly

- **Text metrics.** Montserrat is the same family as LVGL's built-in fonts, but glyph rasterisation,
  kerning and the baseline differ by a pixel or two. Long text that LVGL would wrap inside its
  `8 × line height` box is not wrapped.
- **Anti-aliasing.** The browser's, not LVGL's or `gauge_shape`'s. Close, not identical.
- **Colour depth.** The preview is 24-bit; the panel is RGB565.
- **A full-circle band** (sweep 360 covering the whole range) is drawn as a full ring; LVGL's
  handling of that case has not been checked on hardware.

Geometry that the firmware rounds (tick and needle endpoints to whole pixels, band angles to whole
degrees) is rounded the same way.

### A firmware quirk the preview reproduces

When one alert hands over to another without the value ever clearing both (possible with damping
0), `evaluate_alerts()` returns early: the needle keeps the first alert's colour and flash rate
while the readout switches to the second alert's colour. The preview does the same, so it matches
the gauge. It is probably unintended; it has not been fixed or filed.

## Uploading to a gauge

Every gauge serves this editor at **`http://ai-gauge-XXXX.local/editor/`** (or its IP address),
built into its firmware ([ADR 0008](adr/0008-gauge-serves-face-editor.md)). Opened there, the editor
recognises the gauge (`detectGauge()`, a same-origin `GET /api/status`), and the Device section
manages its faces with no address to enter:

- **Every face on the gauge**, `n of 12`, with its channel and range, any warnings, which one is
  **on screen**, and which is open in the editor. A face the gauge cannot load is still listed,
  with the reason, so it can be opened or deleted.
- **Edit** opens the gauge's copy (`GET /api/config/<id>.xml`). If the browser already has a face
  with that id, the only choices are to replace it or cancel: the editor never renames a face that
  came from a gauge, because a renamed copy would upload as a second face instead of an update.
- **Add or update** the face open in the editor. The button says which, from the gauge's list; the
  choice is re-checked against the gauge before sending, replacing a face asks first, and a full
  gauge disables adding. The gauge's own warning count is read back after the upload.
- **Delete**, with a warning when the face is on screen, which makes the gauge switch to another
  face.

The face's id is its name on the gauge. To keep a changed face alongside the original, change the id
in the Gauge section before adding it.

Requests to the gauge run one at a time, and a refresh already waiting is not repeated. Overlapping
requests let an older face list arrive after a newer one, showing a face that had just been deleted.

**Verified on hardware** (2026-09-15), driving the Device section in Chrome: the list with the face on
screen marked; update with confirmation; edit offering only Replace or Cancel; delete and add a face
not on screen; delete the face on screen, after which the gauge showed another face and the log named
it. Directly against the API: a mismatched id and a 13th face are both refused with nothing written,
and replacing a face on a full gauge succeeds.

| Editor served from | Device section |
|---|---|
| A gauge | Works on that gauge |
| The dev server (`http://localhost`) | Works on a gauge by address; the firmware allows CORS for loopback origins only |
| GitHub Pages | Cannot reach a gauge: an HTTPS page may not call a plain-HTTP device. Links to the gauge's copy, and offers the equivalent `curl` command |

Faces kept in the browser are per origin, so the Pages copy and each gauge's copy have separate
libraries. Download a face from one and import it into the other.

A gauge's copy is the editor from its firmware build. It changes only with a firmware update, and
its parser is always the one that matches that firmware.

**Verified on hardware** (2026-09-15), in Chrome driven over the DevTools protocol:
- `boost_custom` opened from the gauge's examples and uploaded, and the gauge reported 0 warnings.
- A face listed on the gauge opened into the browser library.
- From the dev server, status and the face list worked by address.
- Loading the editor from the gauge took 0.4–0.6 s over WiFi.

**Not yet done:** use by a person in an everyday browser session.

## Running it locally

```bash
cd tools/config-app
npm start        # node dev-server.mjs, no install needed; open http://localhost:8080/editor/
npm test         # node --test; needs Node 22+
```

The dev server mirrors the Pages layout: the flashing page at `/`, the editor at `/editor/`, and
`firmware/assets/gauges` at `/editor/examples/`. A server is needed because browsers do not load
ES modules from `file://`.

To include the differential test, build the dump tool and point the test at it:

```bash
cmake -S tools/host-tests -B build/host-tests
cmake --build build/host-tests --target gauge_config_dump
GAUGE_CONFIG_DUMP=$PWD/build/host-tests/gauge_config_dump npm --prefix tools/config-app test
```

## Publishing

[`.github/workflows/pages.yml`](../.github/workflows/pages.yml) assembles and deploys the whole
Pages site, because a deploy replaces everything:

- `/editor/`: this app from the triggering commit, and `/editor/examples/` from
  `firmware/assets/gauges`
- `/`: the flashing page, with `ai-gauge-merged.bin` downloaded from a GitHub Release (the tag
  that triggered it, otherwise the latest release). With no release yet, `/` redirects to the
  editor

It runs on pushes to `main` that touch the editor, the installer or the shipped faces; after every
release (called from `release.yml`); and by hand. It refuses to publish an editor whose tests fail.

**Repository settings it relies on**, which cannot be checked from the repository itself:

1. *Settings → Pages → Build and deployment → Source* is **GitHub Actions**.
2. The `github-pages` environment's deployment rules allow **`main`** as well as the **`v*`** tags,
   since the editor now deploys from `main`.

## Open items

- Measure the per-frame cost of a shaped needle on hardware, then revisit the §4.6 guidance the
  validator enforces.
- Compare the preview against a photo of the panel for each shipped face, and record the
  differences here.
