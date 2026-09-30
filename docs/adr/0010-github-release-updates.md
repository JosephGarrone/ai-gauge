# ADR 0010: Self-update from GitHub Releases

**Status:** accepted, 2026-09-30

## Context

The gauge could already be updated over WiFi, but only by something on the same network pushing
an image to `POST /api/ota`. The request was for the gauge to find and install new firmware
itself when it is on WiFi. The repository is public, and `release.yml` already builds and
publishes a GitHub Release for every `v*` tag.

Three board facts constrain how:

- **Internal RAM.** About 3-4KB of internal heap is left once WiFi runs. mbedTLS was configured to
  allocate internally, and a TLS handshake makes hundreds of small allocations.
- **Flash writes need an internal-RAM stack.** `spi_flash_disable_interrupts_caches_and_other_cpu()`
  asserts `esp_task_stack_is_sane_cache_disabled()`. A new task with a ~8KB internal stack for
  TLS and flash does not fit.
- **A vehicle gauge that restarts on its own** goes dark for about 30 seconds.

## Decision

1. **Discover releases through `https://github.com/<repo>/releases/latest/download/ota.json`**, a
   small manifest that `release.yml` attaches to every release (`version`, `tag`, `size`,
   `sha256`). Then download `ai-gauge.bin` from that tag, so the manifest and the image always
   belong together.
2. **Check automatically; install only on request.** A check runs a minute after boot and every
   12 hours while on WiFi. Installing takes two taps on the firmware card (Settings → System since ADR 0011), or
   `POST /api/update/install`.
3. **Download whole into PSRAM, then write.** The update task has its stack in PSRAM and never
   touches flash. Once the image has been checked, it goes to the **HTTP server task** through
   `httpd_queue_work()`. That task's internal stack already performs the same writes for
   `POST /api/ota`.
4. **mbedTLS allocates from PSRAM** (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`). Nothing else in the
   firmware used TLS. Hardware AES stays on: on the S3 it takes only DMA descriptors from internal
   RAM and works on PSRAM buffers directly.
5. **The image must match the manifest's version exactly**, as well as carry this project's name.
   After that it is an ordinary OTA, protected by `esp_ota_end()` and rollback.

## Alternatives rejected

- **The REST API (`api.github.com/.../releases/latest`).** It is rate-limited to 60 requests an
  hour per public IP, which is shared across a household, and its response is 10-30KB of JSON
  that needs a real parser. The download redirect needs no API and no token.
- **`esp_https_ota`.** It streams straight to flash from the calling task, so that task would
  need an internal-RAM stack big enough for a TLS handshake, which there is no room for. It also
  erases the target partition before the download has proved it can finish.
- **Streaming the download to flash in chunks, each written on the HTTP task.** It saves ~1.7MB of
  PSRAM that is otherwise idle, at the cost of a failed download leaving a half-erased partition
  and a much more involved hand-off between tasks.
- **Installing automatically.** An unexpected restart of a gauge is the wrong default. It is one
  line to change if wanted.
- **Verifying the manifest's `sha256` on the device.** TLS protects the transfer, and
  `esp_ota_end()` checks the hash embedded in the image. A manifest hash adds nothing against
  someone who can publish releases, which only signing addresses.

## Consequences

- The repository has to stay public, or the gauge needs a token (not implemented).
- Anyone who can publish a release on the repository can update every gauge that installs it.
  Signed images remain an open item ([networking.md](../networking.md#open-items)).
- `POST /api/update/install` is a cross-site "simple request", like `POST /api/ota`. A web page
  can make a gauge on the viewer's network install the latest official release, which restarts
  it but installs nothing else.
- An install briefly needs PSRAM equal to the image size (~1.7MB of the ~5.8MB free).
- Linking TLS in cost ~2KB of internal RAM in statics, which starved the panel's DMA at power-on.
  That was paid back by `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`, which moves lwIP and WiFi
  `.bss` to PSRAM ([performance.md](../performance.md)).
