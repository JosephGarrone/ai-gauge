# CI and Releases

**Every push produces a flashable binary that anyone can download and run.** CI is a
deliverable, not an afterthought — it lands early in the project precisely so nothing is ever
developed against a pipeline that does not exist.

## `build.yml` — every push and pull request

1. Build `firmware/` with `espressif/esp-idf-ci-action@v1`, pinned to `esp_idf_version:
   v5.5.1` and `target: esp32s3` — the same version used locally, so CI and developer builds
   cannot diverge.
2. Merge bootloader, partition table, application and LittleFS image into one flashable file:
   ```
   esptool.py --chip esp32s3 merge_bin -o ai-gauge-merged.bin @flash_args
   ```
3. Run the host-side `gauge_config` parser tests.
4. Upload artifacts with `actions/upload-artifact@v4`:
   - `ai-gauge-merged.bin` — flash to `0x0`, contains everything
   - `bootloader.bin`, `partition-table.bin`, `ai-gauge.bin`, `storage.bin` — individual images
   - `ai-gauge.map` and a build-size summary
5. Fail on compiler warnings, so the tree stays clean rather than accumulating noise that
   hides real problems.

Artifacts are retained by GitHub for 90 days.

Also on every push, a **face editor** job builds the C parser's JSON dump (`gauge_config_dump`) and
runs the editor's tests, including the differential test that holds its parser port to the
firmware's. See [config-app.md](config-app.md).

## `release.yml` — on a `v*` tag

1. Same build.
2. Attach every binary to a **GitHub Release**.
3. Call `pages.yml` with the tag, so the flashing page serves the new firmware.

## `pages.yml` — the GitHub Pages site

A Pages deploy replaces the whole site, so this one workflow always assembles all of it:

| Path | Content | Source |
|---|---|---|
| `/` | ESP Web Tools flashing page and manifest | `tools/web-installer`, with `ai-gauge-merged.bin` downloaded from a GitHub Release |
| `/editor/` | Face editor | `tools/config-app` at the triggering commit |
| `/editor/examples/` | Shipped faces, offered as starting points | `firmware/assets/gauges` |

It runs when `release.yml` calls it (serving that tag), on pushes to `main` that touch the editor,
installer or shipped faces (serving the latest release), and by hand. The flashing page only ever
serves released firmware. With no release yet, `/` redirects to the editor. The editor's tests must
pass before anything is published.

Requires *Settings → Pages → Source: GitHub Actions*, and a `github-pages` environment whose
deployment rules allow both `main` and `v*` tags.

## Browser flashing

ESP Web Tools flashes over WebSerial with no toolchain installed:

1. Open the project's GitHub Pages URL in **Chrome or Edge** (Firefox and Safari do not
   implement WebSerial).
2. Connect the board over USB.
3. Click **Connect**, choose the serial port, and flash.

This is the intended path for anyone who is not developing the firmware.

The manifest names the merged binary at offset `0x0`:

```json
{
  "name": "AI-Gauge",
  "version": "1.0.0",
  "builds": [{
    "chipFamily": "ESP32-S3",
    "parts": [{ "path": "ai-gauge-merged.bin", "offset": 0 }]
  }]
}
```

## Versioning

Semantic-ish versioning on git tags: `v<major>.<minor>.<patch>`.

The version is derived from the tag and compiled in, so `GET /api/status` and the settings
screen report exactly which build is running. Untagged builds report the short commit SHA —
a device should always be able to tell you precisely what firmware it has, without guesswork.

| Bump | When |
|---|---|
| Major | Breaking gauge-schema change, or an OTA that cannot be rolled back cleanly |
| Minor | New features, additive schema changes |
| Patch | Fixes only |

## Verifying the pipeline

A CI job that produces a `.bin` has not proven anything until that exact file has been
flashed and booted. Producing an artifact and producing a *working* artifact are different
claims.

Whenever the build or partition layout changes:

1. Download `ai-gauge-merged.bin` from the workflow run.
2. `esptool.py --chip esp32s3 write_flash 0x0 ai-gauge-merged.bin`
3. Confirm it boots and the gauge renders.

For release changes, additionally tag a pre-release and flash a board from the ESP Web Tools
page end to end.

## Open items

- Sign release artifacts so OTA images can be verified — see
  [networking.md](networking.md).
- Cache managed components between runs to cut build time.
- Add a build-size regression check once a baseline exists.
