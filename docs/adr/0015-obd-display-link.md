# ADR 0015 — The OBD Display link

**Status:** Accepted (2026-11-27). Built, and checked on the host and against a scripted display.
**Not yet run on a board.** The protocol is in [networking.md](../networking.md#the-obd-display-link).

## Context

The gauges sit in the same car as the OBD Display, a Raspberry Pi dashboard (the obd-display
repository) that reads the vehicle's CAN buses. Each has what the other lacks:

- The display knows the dash dimmer and its own panel brightness, so the gauges, which do not,
  stay at daytime brightness at night. It also has every CAN and UDS reading, so a gauge could show
  coolant, RPM or the PCM's boost without wiring a sensor.
- The gauges have the sensors the vehicle lacks (their own MAP and an EGT thermocouple), which the
  display should log beside its CAN captures.

The display now has a *USB upstream* Wi-Fi mode, in which its onboard radio is always a hotspot,
so the gauges can always be on its network in the car. The gauge already listens for telemetry on
UDP 5005 (`{"ch","v"}`), which drives the needle through the same path as a sensor.

## Decision

1. **The display beacons and the gauge answers its source.** A 1 Hz broadcast to 5005 carrying
   the display's brightness; the gauge replies to the beacon's source address and port, every
   100 ms while beacons arrive. Neither end is told an address. The display also sends its beacon
   straight to each gauge it knows, for networks that do not pass broadcasts between clients.
2. **The gauge asks for one channel: the one its face shows.** `want` and `unit` are the face's
   `<source>`. The display sends that channel, in that unit, in the existing telemetry frame. So
   a face shows vehicle data by naming a display catalog id; nothing else on the gauge changes,
   and the gauge never parses a list of values it does not show.
3. **The gauge reports its sensor channels in native units**, only the valid ones. The display
   converts, labels and logs them.
4. **Brightness scales the user's setting rather than replacing it.** The gauge's brightness arc
   becomes its level at the display's full brightness, and the display's percentage scales it
   down. An AMOLED and an LCD backlight do not look alike at the same percentage, so the user keeps
   the relative level. It can be turned off (*Dim with OBD Display*).
5. **One socket and one task.** The telemetry task gained the beacon and the reports; its receive
   timeout dropped from 1 s to 100 ms to clock them, and its stack moved to PSRAM (4KB), which
   frees the 3KB of internal RAM it used.
6. **JSON, as the telemetry frame already was.** Small, readable in a packet capture, and parsed
   on the gauge with the same few lines of string handling.

## Alternatives rejected

- **mDNS discovery.** The gauge advertises itself, but the display has Avahi off for boot time and
  would need its own mDNS client; and the gauge would still need to find the display. The beacon
  does both directions in one datagram.
- **A fixed address or a configured peer.** The Pi's hotspot address is fixed (10.42.0.1), but
  the display can be on any network in client mode, and a configured address is one more thing to
  get wrong from a phone.
- **The display sending every signal.** About 150 catalog signals at 20 Hz to a board whose
  binding constraint is internal RAM, to show one.
- **A new task or port on the gauge.** Another internal-RAM stack, for no gain over the socket
  that already exists.
- **WebSocket or HTTP polling.** TCP holds a late value back to deliver it; telemetry is better
  dropped than late (see *Telemetry ingest*). HTTP also costs the server task's stack per request.

## Consequences

- Anything on the network can set a gauge's brightness (within its own setting) or feed its face,
  as it already could feed telemetry; noted under Security in networking.md.
- A gauge provisioned onto the display's hotspot reaches the internet for updates only through the
  display's USB upstream.
- Either end can be updated first: an old gauge drops the beacon as a malformed frame, and a new
  gauge with no display sees no beacon and behaves as before.
