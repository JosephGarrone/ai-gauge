# ADR 0007 — Face editor: a static web app carrying a port of the firmware parser

**Status:** Accepted (2026-09-14). Built and tested; not yet deployed to Pages, and upload to a
gauge is blocked on firmware (see [../config-app.md](../config-app.md)).

## Context

M7 needs a browser tool that designs gauge faces, previews them faithfully and emits XML that the
firmware accepts without warnings. Three facts shape it:

- **The firmware forgives rather than rejects.** It clamps, drops and truncates, and only counts
  what it did ([gauge-xml-interface.md §6](../gauge-xml-interface.md#6-validation)). An editor that
  guesses at those rules will produce files that "work" but render differently from the preview.
- **The gauge cannot be reached from GitHub Pages today**: no CORS, HTTP only (§8). The editor must
  be complete without a device.
- **The repository is C firmware** with no JavaScript toolchain, and the flashing page is already a
  single static HTML file.

## Decision

1. **Static files and native ES modules, no dependencies, no build step.** Hosted on GitHub Pages at
   `/editor/`, beside the flashing page, from `tools/config-app/`.
2. **The XML parser is a line-for-line JavaScript port of `gauge_config.c`**, including its scanner
   quirks, float32 storage, byte-based truncation and warning count. The editor uses it to import
   files and, as a backstop, to re-read everything it writes.
3. **Parity is enforced by a differential test in CI.** `tools/host-tests/gauge_config_dump` prints
   the C parser's result as JSON; the test runs both parsers over the shipped faces, the firmware's
   own test cases, parser quirks and seeded random documents, and fails on any difference.
4. **Pure logic is separated from the browser shell** (parse, serialize, validate, geometry, printf,
   render, sim), so all of it is unit-tested under Node's built-in test runner.
5. **One Pages workflow assembles the whole site** on every relevant push to `main` and after every
   release, taking the flashing page's firmware from a GitHub Release. A Pages deploy replaces the
   entire site, so the editor and the flashing page cannot be deployed independently.

## Alternatives considered

**A framework with a bundler (React, Vite).** Rejected. It adds a toolchain, a lockfile and
dependency updates to a C repository, for a single page whose state fits in one object. Native
modules load directly in every current browser.

**The browser's `DOMParser`.** Rejected. It decodes entities, rejects documents the firmware accepts
and knows nothing of the clamping and warning rules, so its view of a file differs from the device's
in exactly the cases an editor must catch.

**Compile `gauge_config.c` to WebAssembly.** Considered seriously: it would give parity by
construction. Rejected for now because it needs Emscripten (or similar) in CI and for anyone
running the editor locally, and it would still leave the serializer, validator and preview in
JavaScript. The firmware only counts warnings, while the editor needs to say what each one was, so
the C would need changing too. The differential test gives the same confidence for far less
machinery. If the port starts drifting in ways the corpus misses, revisit this.

**Serve the editor from the gauge itself.** Not decided, and not rejected: a same-origin page would
sidestep CORS and mixed content entirely. It costs flash and, more importantly, HTTP server stack
and internal RAM, which are the board's binding constraint ([../performance.md](../performance.md)).
It belongs with the other open browser-access options in [../networking.md](../networking.md).

**Separate Pages deploys for the editor and the flashing page.** Not possible: a deployment
replaces the whole site. Hence one workflow that always assembles both.

## Consequences

**Good:**
- The "device reports 0 warnings" verdict is computed by the firmware's own rules, and CI proves it.
- Nothing to install to run or test the editor beyond Node for the tests.
- The pure modules document the rendering model in executable form, and are reusable by future
  tools (a command-line validator, for instance).

**Bad:**
- Every parser change is made twice, in C and in JavaScript. The differential test catches a
  forgotten half, but only for inputs the corpus exercises.
- No type checking or bundling: mistakes in browser-only code surface at runtime, not at build.
- The editor now deploys from `main`, so the `github-pages` environment must allow it.

**Neutral:**
- `gauge_config_dump.c` becomes part of the schema-change checklist in
  [../gauge-config-schema.md](../gauge-config-schema.md).
