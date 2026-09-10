# ADR 0004 — Retain the microSD slot and audio codecs

**Status:** Accepted

## Context

The gauge application needs neither the microSD slot nor the audio codecs. Their pins account
for six of the ten ADC1-capable GPIOs (GPIO1/2/3 for SD, GPIO8/9/10 for I2S), and ADC1 is the
only ADC usable alongside WiFi ([ADR 0001](0001-external-i2c-sensor-frontend.md)).

So the obvious question: drop both, reclaim ADC1, and read the boost sensor with the internal
ADC instead of an external chip?

## Decision

**Keep both**, and give them jobs rather than leaving them idle.

## Rationale

The reclaimed pins are free in software but not accessible in hardware:

- **GPIO1/2/3 land only on the microSD socket pads.** There is no header, so using them means
  soldering to the socket or removing it. The upstream hardware reference's phrasing — *"do
  not repurpose GPIO1, GPIO2, or GPIO3 while the card is mounted"* — confirms they are
  electrically reclaimable, but says nothing about physical access, and the schematic would
  need checking for pull-ups that would skew a divider reading.
- **GPIO8/9/10 run to the ES8311 and ES7210**, which remain populated. The **ES7210 actively
  drives GPIO10**, so that pin is unusable as an input regardless of firmware.

That leaves board rework as the price of admission. And it would not even remove the external
front-end: a K-type thermocouple needs a cold-junction-compensated amplifier, and the only
option on a free bus is the I2C MCP9600. Once that I2C bus exists, adding an ADS1115 costs
about two dollars and one address — while delivering 16-bit differential conversion with a
stable reference, which is meaningfully better than the ESP32-S3's internal ADC in the
electrically noisy environment of a running engine.

So the rework buys nothing. Both subsystems stay, and both earn their place:

- **microSD** — optional bulk storage for large gauge graphics that would not fit the 7MB
  LittleFS partition, and a natural destination for datalogging.
- **Audio (ES8311)** — an audible over-boost / over-EGT chime, exposed through the
  `<alert chime="true">` schema attribute. A gauge that can get the driver's attention without
  being looked at is genuinely more useful than one that cannot.

## Consequences

**Good:**
- No board rework; the design works on a stock board.
- Audible alerts become possible, which a purely visual gauge cannot offer.
- Bulk storage available for artwork and logging.
- Better analogue performance than the internal ADC would have provided.

**Bad:**
- Both subsystems consume flash and boot time for features that are secondary.
- ADC1 remains permanently unavailable, so the external sensor board is mandatory rather than
  optional.

**Neutral:**
- If a future revision ever needs ADC1, this decision is reversible — the analysis above is
  the starting point, and the schematic pull-up question would need answering first.
