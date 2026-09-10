# ADR 0002 — Custom gauge XML schema

**Status:** Accepted

## Context

The gauge's look and feel must be customisable through a configuration file, so faces can be
authored — eventually by a web app — without rebuilding firmware. The same firmware should
also run on square and other round panels.

LVGL 9.4 ships its own declarative XML UI format with a runtime loader, which is an obvious
candidate.

## Decision

Define a **small, domain-specific XML schema that describes a dial gauge** — scale range,
angles, colour bands, ticks, labels, needle, readouts, alerts — and parse it into a flat
`gauge_config_t` consumed by a purpose-built renderer.

The normative schema is [../gauge-config-schema.md](../gauge-config-schema.md).

## Alternatives considered

**LVGL's native XML UI format.** Rejected as the primary format. It describes arbitrary widget
trees, which means a general layout engine sits in the render path. The 60fps requirement
depends on knowing exactly what is drawn and how often
([../display-pipeline.md](../display-pipeline.md)); a general format makes it trivially easy to
author a face that quietly blows the frame budget, and hard to detect that from the file. It
also brings a larger runtime parser and more RAM, and its runtime loader is still maturing.

**JSON instead of XML.** Genuinely easier to parse — cJSON ships with ESP-IDF, versus needing
an XML tokeniser. Rejected because XML's attribute model fits this data well (elements are
gauge features, attributes are their properties), the nesting of bands and ticks inside a face
reads naturally, and it was the requested format. The parsing cost is a one-time boot expense,
not a per-frame one, so the easier-to-parse argument carries little weight here.

**XML authored off-device, compiled to a binary blob.** Rejected for now as premature. It
would cut boot time and RAM, but requires a converter in the toolchain and makes configs
un-inspectable on the device. Worth revisiting if parse time ever shows up as a problem — the
schema is designed so this could be added later without changing the authoring format.

## Consequences

**Good:**
- Render cost is predictable, because the schema can only express things the renderer knows
  how to draw efficiently.
- A small, auditable parser with bounded memory use.
- An obvious target for a web app: every element maps to a visible gauge feature.
- Ignoring unknown elements means newer authoring tools do not brick older firmware.
- Being panel-agnostic, faces scale across board profiles.

**Bad:**
- Anything the schema cannot express requires a firmware change — layouts genuinely outside
  "a dial gauge" are not possible without extending both schema and renderer.
- We own the format: the schema doc, the parser, its tests and the authoring tool are all ours
  to maintain, with no upstream ecosystem to inherit from.
- Two XML dialects exist in the project if LVGL's own format is ever adopted for the settings
  screen.

**Neutral:**
- The parser is hand-written over the small XML subset the schema needs -- start tags,
  attributes, self-closing tags, comments and the prolog. Using LVGL's bundled tokeniser was
  considered and rejected during implementation: it would have made `gauge_config` depend on
  LVGL, forfeiting the host-testability that is the main reason the component is a leaf. The
  subset is small enough that the parser costs less than the dependency would.
