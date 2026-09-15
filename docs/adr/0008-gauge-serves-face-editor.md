# ADR 0008 — The gauge serves the face editor; CORS only for loopback

**Status:** Accepted (2026-09-15).

## Context

The face editor ([ADR 0007](0007-face-editor-static-app-with-parser-port.md)) is published on
GitHub Pages, and could not upload to a gauge
([gauge-xml-interface.md §8](../gauge-xml-interface.md#8-known-gaps-for-a-browser-app)):

- **Pages is HTTPS; the gauge is plain HTTP.** Mixed-content rules block every request, and no
  response header the firmware could send changes that.
- **No CORS**, so even an HTTP page from another origin, such as the local dev server, cannot read
  replies, and the preflight a `PUT` or `DELETE` needs fails.
- **No raw XML download**, so a face cannot be loaded back from a gauge.

Internal RAM is the board's binding constraint ([../performance.md](../performance.md)). TLS on the
gauge would need a certificate that browsers trust for a `.local` name, which is not practical, and
mbedTLS sessions cost internal RAM the board does not have.

## Decision

1. **The firmware serves the editor at `/editor/`.** Loaded from the gauge, the editor shares the
   API's origin, so there is no CORS, no mixed content and no private-network restriction, in any
   browser, including on the setup network with no internet.
2. **The editor is built into the application image**, not the LittleFS partition. At build time
   `editor_bundle.cmake` gzips `tools/config-app` and the shipped faces into a C table. The HTTP
   server sends each file straight from flash with `Content-Encoding: gzip`: no decompression, no
   buffer, no filesystem read.
3. **The editor detects when a gauge served it** (same-origin `GET /api/status`) and then uploads
   to that gauge without asking for an address.
4. **`GET /api/config/<id>.xml` returns the stored file**, so faces can be opened from the gauge.
5. **CORS is allowed for loopback origins only** (`http://localhost`, `127.0.0.1`, `[::1]`, any
   port), on the status, list and config endpoints, with an `OPTIONS` handler for config. That
   serves development against a real gauge and nothing else.
6. **GitHub Pages stays** as the place to design without a gauge. Its Device section explains the
   block and links to the gauge's own copy.

## Alternatives considered

**Store the editor on LittleFS.** Rejected. OTA updates only the app partition, so after an update
the gauge would serve an editor from an older build, whose parser port may no longer match the
firmware. The "device reports 0 warnings" verdict would then be wrong exactly when the schema has
changed. Built into the app, the editor and firmware always update together.

**`Access-Control-Allow-Origin: *`.** Rejected. The API has no authentication, and the preflight is
currently what stops an arbitrary web page from sending `PUT` or `DELETE` to a gauge on the viewer's
network. A wildcard would let any site the user visits replace or delete faces.

**Rely on Chrome's local-network-access permission** so Pages could call the gauge. Rejected for
now: Chromium only, policy still changing, and Firefox and Safari stay blocked.

**TLS on the gauge.** Rejected: no trusted certificate for a `.local` name, and the internal RAM
cost.

**A proxy or browser extension.** Rejected: something extra to install, which the static editor
exists to avoid.

## Consequences

**Good:**
- Upload works from any browser that can reach the gauge, with nothing installed.
- A gauge always offers an editor that matches its firmware.
- Faces can be opened from a gauge, edited and sent back.

**Bad:**
- The application image grows by the gzipped editor (~67KB today); with `SPIRAM_RODATA` it is also
  copied into PSRAM at boot.
- The editor loads Montserrat from Google Fonts. On a network without internet the preview falls
  back to a system font; the geometry is unaffected.
- Faces kept in the browser's `localStorage` are per origin: the Pages copy and each gauge's copy
  have separate libraries. Moving a face between them is download and import.
- A gauge's editor only changes with a firmware update.

**Unchanged:** the API is still unauthenticated. `POST /api/ota` and `POST /api/wifi` are "simple"
requests that any web page can send without a preflight, even though it cannot read the reply. That
is not new, is recorded under *Security* in [../networking.md](../networking.md), and is part of the
case for authentication and signed images.
