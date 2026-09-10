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

## `release.yml` — on a `v*` tag

1. Same build.
2. Attach every binary to a **GitHub Release**.
3. Generate an **ESP Web Tools** manifest and publish a flashing page to **GitHub Pages**.

The Pages site will eventually also host the gauge-config web app, so browser flashing and
browser configuration share one place.

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
