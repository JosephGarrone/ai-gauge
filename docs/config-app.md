# Face Editor (Configuration Web App)

The M7 web app: design a gauge face in the browser, see it move, find out exactly what the gauge
will make of it, and export the XML. Lives in [`tools/config-app/`](../tools/config-app/) and is
published to GitHub Pages at **`/editor/`**, next to the flashing page.

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
| Upload to a gauge | Implemented, but **blocked by firmware gaps**; see below |

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
| `js/store.js`, `js/device.js` | Browser storage; HTTP API client | no (browser-only) |
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

`js/device.js` implements the §7 API: upload (then read back the warning count), list, delete and
status. **It cannot work from the published site today**, and no workaround is attempted, per §8:

- The site is HTTPS and the gauge is plain HTTP, so the browser blocks every request (mixed content).
  The Device section says so before trying.
- From any other origin (the local dev server included), the gauge's missing CORS headers and
  `OPTIONS` handler mean responses cannot be read and a `PUT` preflight fails.

Until the firmware changes, the Device section offers the equivalent `curl` command. **Upload has
not been exercised against a real gauge.** The fix is an open firmware decision, tracked in
[networking.md](networking.md).

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

- Upload to a gauge, blocked on the firmware gaps above.
- Measure the per-frame cost of a shaped needle on hardware, then revisit the §4.6 guidance the
  validator enforces.
- Compare the preview against a photo of the panel for each shipped face, and record the
  differences here.
