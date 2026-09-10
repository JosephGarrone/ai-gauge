# Networking

WiFi exists to serve the gauge, never to delay it. `net_svc` starts **last** and runs on core
0; nothing in the display path waits on it. See [architecture.md](architecture.md).

## Provisioning

1. On boot, credentials are read from NVS.
2. If present, connect as a station. Retry with backoff, indefinitely, in the background.
3. If absent — or if the user requests it from the settings screen — start a SoftAP named
   **`ai-gauge-setup`** with a captive portal for entering network credentials.
4. Credentials are stored in NVS on a successful connection.

The settings screen always shows the current state (connected SSID and IP, connecting, AP
mode, or disabled) so the network state is never a mystery.

WiFi can be disabled entirely from the settings screen. In a vehicle, most of the time it is
not needed, and disabling it saves power and removes a source of RF noise near an analogue
front-end.

## mDNS

The device advertises as **`ai-gauge.local`**, so it is reachable without hunting for a DHCP
lease. The hostname is configurable in settings for installations with more than one gauge.

## HTTP API

Served on port 80.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/` | Built-in management page |
| `GET` | `/api/status` | Firmware version, uptime, WiFi state, channel values, config warnings |
| `GET` | `/api/gauges` | List available gauge configs |
| `GET` | `/api/config/<id>` | Fetch a gauge XML file |
| `PUT` | `/api/config/<id>` | Upload a gauge XML file |
| `DELETE` | `/api/config/<id>` | Remove a gauge config |
| `POST` | `/api/ota` | Upload a firmware image |

### Config upload

`PUT /api/config/<id>` is the path the future web app will use.

1. Body is written to a temporary file on LittleFS.
2. It is parsed and validated **before** anything is replaced.
3. On success it is moved into place and, if it is the active gauge, the face is re-rendered
   live — **no reboot**.
4. On failure the existing config is untouched and the response carries the parse errors.

Validating before replacing is what stops a bad upload from leaving a vehicle with a blank
gauge.

### Security

Currently **unauthenticated on the local network**, which is acceptable for a device on a
private network but is not a considered security posture. Recorded honestly rather than
overstated:

- No TLS — the HTTP server is plain.
- No authentication on config or OTA endpoints.
- Anyone on the same network can reflash the device.

Before this is used on an untrusted network it needs, at minimum, a shared secret on the
mutating endpoints and signature verification on OTA images. Tracked as an open item.

## Telemetry ingest

A UDP listener accepts channel values pushed from other devices — an ECU interface, a
datalogger, a phone. These feed the **same snapshot mechanism** as physical sensors, so a
remote channel and a local sensor are indistinguishable to the renderer
([architecture.md](architecture.md)).

Default port **5005**. Frame format (JSON, one datagram per update):

```json
{"ch": "boost", "v": 12.4, "t": 1234567890}
```

| Field | Meaning |
|---|---|
| `ch` | Channel name, matching a `<source channel="...">` |
| `v` | Value in the channel's native unit |
| `t` | Optional sender timestamp (ms); omitted means "now" |

UDP is deliberate: telemetry is a continuous stream of perishable values, so dropping a
datagram is strictly better than delaying the stream to retransmit one. A late value on a
gauge is worse than a missing one.

Values inherit the same staleness rule as sensor channels — if a feed stops, the channel
becomes invalid rather than freezing at its last value.

## OTA

- Dual `ota_0` / `ota_1` app partitions (4MB each; see `firmware/partitions.csv`).
- `POST /api/ota` writes to the inactive slot, then marks it for boot.
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` — a new image must confirm itself healthy or the
  bootloader rolls back to the previous one.
- The image is validated before the boot partition is switched.

The gauge must survive a failed update in a vehicle, which is why rollback is enabled rather
than assumed unnecessary.

## Open items

- Authentication and TLS for the mutating endpoints (see *Security* above).
- Signed OTA images.
- Whether to add an mDNS-advertised service record for automatic discovery by the web app.
