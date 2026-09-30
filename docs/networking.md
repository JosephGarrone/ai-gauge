# Networking

WiFi exists to serve the gauge, never to delay it. `net_svc` starts **last** and runs on core
0; nothing in the display path waits on it. See [architecture.md](architecture.md).

> **Verification status (2026-09-14, on hardware, gauge joined to a home network):**
>
> - Provisioning from a phone via the setup page, including a failed attempt that correctly fell
>   back to the setup network without saving anything.
> - Credentials persist across reboots and OTA updates.
> - `ai-gauge-91e8.local` resolves from a Windows PC.
> - 17 endpoint checks pass: status, gauge list, config get, 404 for unknown, malformed upload
>   rejected with the live config intact, valid upload reloaded live, create, delete, path
>   traversal rejected, malformed telemetry ignored, garbage OTA image rejected without a reboot.
> - Telemetry drives the needle: a 58.6Hz feed rendered every datagram.
> - OTA of a real 1.58MB image over WiFi in ~15s; the bootloader's OTA data then records ota_1 as
>   VALID, and it stayed booted there across two further reboots.
>
> - Boot-time rollback: an image that passed upload validation, booted, and aborted before
>   confirming itself was marked ABORTED by the bootloader, which then booted the previous image.
>   The gauge was back on the network about 5s later.
> - Late-crash rollback: an image that built its UI, joined WiFi, then crashed 3s later (before
>   its 15s confirmation) was marked ABORTED and the previous image booted and re-confirmed.

## Provisioning

1. On boot, credentials are read from NVS (namespace `net_svc`).
2. If present, connect as a station. Retry with exponential backoff (2s up to 30s),
   indefinitely, in the background.
3. If absent, start an **open** SoftAP named **`ai-gauge-setup`**. Join it and browse to
   **`http://192.168.4.1`** for a form that takes the network name and password.
4. New credentials are saved to NVS **only after they connect**. If they fail, the setup
   network comes back and any previously working credentials are left untouched, so a typo
   cannot lock the device out.

This is a plain setup page at a fixed address, not a captive portal: phones will not pop it up
automatically. A DNS-redirecting captive portal is an open item.

The settings page shows the current state: connected network and address, setup mode with
instructions, or connecting. Its **Reset network** button (tap, then tap again within 4s to
confirm) forgets the stored credentials and brings the setup network straight back up, with no
reboot.

## Hostname and mDNS

The device advertises as **`ai-gauge-XXXX.local`**, where `XXXX` is the last four hex digits
of its WiFi MAC. The suffix means two gauges on one network cannot collide. The exact name
is shown on the settings page and in `GET /api/status`. An `_http._tcp` service record is
advertised on port 80.

## HTTP API

Served on port 80, at most four concurrent connections.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | Management page, or the setup form while in setup mode |
| `GET` | `/api/status` | Firmware and IDF version, uptime, WiFi state, free and lowest internal heap, lowest free HTTP-server stack, the face on screen (`active_gauge`, empty for the built-in face), storage state |
| `GET` | `/api/gauges` | `{"gauges":["boost","egt"],"max":12}` |
| `GET` | `/api/config/<id>` | Parses the stored config and returns a summary: channel, unit, range, warning count |
| `GET` | `/api/config/<id>.xml` | The stored XML exactly as written, for loading back into the face editor |
| `PUT` | `/api/config/<id>` | Upload gauge XML (body is the XML, max 16KB) |
| `DELETE` | `/api/config/<id>` | Remove a gauge config |
| `OPTIONS` | `/api/config/<id>` | CORS preflight, answered for loopback origins only (below) |
| `POST` | `/api/wifi` | Form-encoded `ssid` and `password`; used by the setup page |
| `POST` | `/api/ota` | Raw application image as the body |
| `GET` | `/api/sensors` | Sensor board state: per-input raw ADC code, pin voltage, reads/s and count; channel values; faults; last I2C bus scan |
| `GET` | `/api/update` | `{"state","running","latest","progress","message","repo"}`, see [Updates from GitHub](#updates-from-github) |
| `POST` | `/api/update/check` | Look for a newer release now (`202`; `409` with no connection or while busy) |
| `POST` | `/api/update/install` | Install the release a check found (`202`; `409` if there is none) |
| `GET` | `/editor/` | The face editor, built into the firmware image |

`GET /api/config/<id>` returns a parsed summary; add `.xml` for the file itself. The file is
returned even if it no longer parses, so it can be fixed.

### Browser access and the face editor

Decided in [ADR 0008](adr/0008-gauge-serves-face-editor.md); verified on hardware 2026-09-15.

- **The gauge serves the face editor at `/editor/`**, linked from the management page. The build
  gzips `tools/config-app` and the shipped faces into the application image
  (`editor_bundle.cmake`), and `editor_get()` sends each file from flash with
  `Content-Encoding: gzip`. The editor shares the API's origin, so the browser applies no CORS or
  mixed-content rules, and it updates with every OTA.
- **CORS is allowed for loopback origins only** (`http://localhost`, `127.0.0.1`, `[::1]`, any
  port), on status, the gauge list and config. That exists so the editor's dev server can talk to a
  real gauge. Any other origin gets no CORS headers, and a `403` preflight.
- GitHub Pages cannot reach a gauge at all (an HTTPS page calling a plain-HTTP device). Its editor
  links to the gauge's own copy instead.

### Config upload

`PUT /api/config/<id>`, handled by `gauge_store_save()`:

1. The body is **parsed and validated before anything is written**. A file that does not parse
   is rejected with a `400` naming the reason, and the existing config is untouched. So is a file
   whose `<gauge id>` differs from `<id>` in the URL, and a new id once 12 faces are stored;
   replacing an existing face is always allowed. Both rules keep every stored face listed, and
   addressed by one name.
2. It is written to `<id>.xml.tmp` and renamed into place, so an interrupted write cannot
   truncate the config already on the device.
3. If it is the gauge currently on screen, the face is rebuilt live, **without a reboot**.

Ids are restricted to letters, digits, `_` and `-`, so a request cannot escape the gauges
directory.

Deleting the face on screen switches the gauge to the first remaining face that loads, or to the
built-in face if none does, and makes it the selected gauge, as startup would. The settings page
says what happened.

### Security

**Unauthenticated and unencrypted.** Acceptable on a private network; not a considered security
posture. Stated plainly:

- No TLS.
- No authentication on config, WiFi or OTA endpoints.
- Anyone on the same network can replace configs or reflash the device.
- The setup network is **open** while the device is unprovisioned. Anyone in range during
  that window can join it and submit credentials.
- Web pages: CORS is echoed only to loopback origins, so another site cannot pass the preflight
  that a config `PUT` or `DELETE` needs. `POST /api/ota` and `POST /api/wifi` need no preflight,
  though, so a page on any site could send one to a gauge on the viewer's network without reading
  the reply, where the browser does not itself restrict public pages from calling private
  addresses. `esp_ota_end()` rejects an invalid image and the project-name check rejects another
  project's, but neither stops a valid ai-gauge image built by someone else.

Before exposure to any untrusted network this needs, at minimum, a shared secret on the
mutating endpoints and signature verification on OTA images.

## Telemetry ingest

A UDP listener on **port 5005** accepts channel values pushed from other devices. A value whose
channel matches the gauge on screen drives the needle through the same path a physical sensor
will use.

Frame format, one JSON object per datagram:

```json
{"ch": "boost", "v": 12.4}
```

| Field | Meaning |
|---|---|
| `ch` | Channel name, matching the gauge's `<source channel="...">` |
| `v` | Value in the channel's native unit |

Malformed datagrams are dropped silently, and so is a `v` that is not finite (`nan`, `inf`):
the renderer also treats a non-finite value as invalid, because NaN would otherwise stick in its
damping filter for good. UDP is deliberate: telemetry is a stream of perishable
values, so dropping a datagram is strictly better than delaying the stream to retransmit one.

**Not yet implemented:** the staleness rule. If a feed stops, the needle currently holds its
last value rather than going invalid. That belongs with the channel snapshot in `sensor_hub`
(M5), and until then the simulated source also keeps driving the needle alongside any feed.

## OTA

- Dual `ota_0` / `ota_1` app partitions, 4MB each ([../firmware/partitions.csv](../firmware/partitions.csv)).
- `POST /api/ota` streams the body into the inactive slot. `esp_ota_end()` validates the image
  before the boot partition is switched, so a corrupt upload is rejected rather than booted.
- Before anything is erased, the image's app description must carry this firmware's
  `project_name`. A well-formed image for some other project would pass `esp_ota_end()`, and
  rollback only catches one that crashes before confirming.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on. A newly installed image confirms itself with
  `esp_ota_mark_app_valid_cancel_rollback()` only once **startup has finished and the gauge has
  stayed up for 15 seconds**. A crash anywhere in that window leaves the image unconfirmed, and
  the bootloader returns to the previous image on the next boot.
- Confirming straight after the UI was built proved too early. A test image that crashed a few
  hundred milliseconds later, during audio start-up, had already confirmed itself, so it was
  never rolled back and the board boot-looped until it was reflashed over USB.
- An image is also left **unconfirmed if networking fails to start**, since it could never be
  updated remotely again. A missing WiFi network does not count; only a failure of the stack to
  initialise does. Verified: a build whose WiFi could not initialise logged
  `not confirming this image: networking failed to start`. Before this, a build that lost WiFi
  confirmed itself 15s later and could only be recovered over USB.

## Updates from GitHub

Decided in [ADR 0010](adr/0010-github-release-updates.md). Code: `net_svc_update.c`.

- **Source:** the latest release of `CONFIG_AI_GAUGE_UPDATE_REPO` (default
  `JosephGarrone/ai-gauge`, which must stay public). `release.yml` attaches `ota.json`
  (`version`, `tag`, `size`, `sha256`) next to `ai-gauge.bin`.
- **Check:** `GET https://github.com/<repo>/releases/latest/download/ota.json`, following the
  redirect to GitHub's asset host by hand. Not the REST API: that is rate-limited to 60 requests an
  hour per address and returns tens of KB. A `404` means no release yet, which reads as up to date.
  A version that is not `X.Y.Z` (a local build's SHA, CI's `0.0.0-dev+<sha>`) counts as older than
  any release.
- **When:** automatically a minute after boot if on WiFi, then every 12 hours
  (`CONFIG_AI_GAUGE_UPDATE_AUTO_CHECK`), and on demand from the settings page or
  `POST /api/update/check`. A check never installs anything.
- **Install:** only on request, because it restarts the gauge: tap the firmware line on the
  settings page twice within 4s, or `POST /api/update/install`. The image is downloaded whole into
  PSRAM, must carry this project's name and exactly the version `ota.json` promised, and only then
  is written. So a failed download changes nothing.
- **The write runs on the HTTP server task**, via `httpd_queue_work()`. A flash write asserts that
  the calling task's stack is in internal RAM (`spi_flash/cache_utils.c`), and the update task's
  stack is in PSRAM. The HTTP task's 6KB internal stack already does the same writes for
  `POST /api/ota`.
- After that it is an ordinary OTA: `esp_ota_end()` validates the image and its hash, and the new
  firmware must confirm itself after 15s or the bootloader rolls back.
- **TLS** uses the ESP-IDF certificate bundle. mbedTLS allocates from PSRAM
  (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`); a handshake does not fit in the internal RAM left once
  WiFi runs.

## Memory constraints

Bringing WiFi up on this board is primarily a **memory** problem, not a CPU one. Internal SRAM
must hold the LVGL flush buffers, WiFi's static RX buffers, lwIP, and every task stack. The
configuration in `firmware/sdkconfig.defaults` that makes it fit, and what breaks without it,
is recorded in [performance.md](performance.md) and [display-pipeline.md](display-pipeline.md).

## Open items

- Expose the running OTA partition in `GET /api/status`, so update checks do not need a serial cable.
- Captive-portal DNS redirect for the setup network.
- Settings-page controls to disable WiFi and to forget credentials (`net_svc_forget_credentials()`
  exists but is not wired to the UI).
- Telemetry staleness.
- Authentication, TLS and signed OTA images. Updates from GitHub are fetched over verified TLS,
  but the image itself is not signed: whoever can publish a release on the repository can update
  every gauge that installs it.
- `POST /api/update/install` needs no preflight either, so any web page can ask a gauge on the
  viewer's network to install the latest official release. That can only install what the
  repository published, and restarts the gauge.
