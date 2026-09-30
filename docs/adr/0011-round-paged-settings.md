# ADR 0011: Round, paged settings built on demand

**Status:** accepted, 2026-10-01

## Context

The settings tile was a single long scrolling column: sliders, button rows, dropdowns, switches
and read-outs. On a 466 px round panel mounted at the base of an A-pillar it had four problems:

- Reaching anything meant scrolling a long list, and the edges of each row fell into the corners
  the round panel does not have.
- The dropdowns and unselected buttons were white, which glares at night.
- Every widget existed for the life of the gauge, each a small malloc in internal RAM.
- There was no quick path back to the dial.

## Decision

- **A home screen and one page per topic.** The home has a brightness arc around the top rim, six
  round buttons in the wide middle band, and Done at the bottom centre. Each page has a title at
  the top and a floating Back pill at the bottom centre.
- **Pages are built when opened and deleted when closed.** Leaving the settings tile closes any
  open page.
- **Night-safe palette:** true black, dark-grey controls, and a single accent. No white fills.
- **Controls at least 64 px**, and buttonmatrices where there are several choices, so four options
  cost one widget.
- **Return to the dial after 30 s** untouched.
- **A tap on the dial counts only if the finger stayed within 20 px.** A downward swipe on the dial
  goes nowhere, and LVGL reported it as a click, which cleared the peak. It was found by driving
  the UI over HTTP.

Details and screenshots: [settings-ui.md](../settings-ui.md).

## Alternatives rejected

- **Keep the scrolling list and restyle it.** That fixes the glare but neither the reach nor the
  RAM.
- **A radial menu around the rim.** Its buttons would sit at the narrowest, most clipped part of
  the panel, and it needs more precision to hit than a 3×2 grid in the middle.
- **Back in the top corner,** as phones place it. The top of a circle is its narrowest point, and a
  small target high on an A-pillar is the hardest to reach from the seat.
- **Build every page once and hide the inactive ones.** Simpler, but it holds every page's
  widgets in internal RAM forever. That is the cost the redesign removes.

## Consequences

- Internal RAM free after startup rose from 15,051 B to 22,503 B, and the largest DMA block from
  7,424 B to 14,336 B.
- Opening a page costs one full-page render. It happens on a tap, and is not in the dial's render
  path.
- A page's widgets do not exist while it is closed, so its labels are fed from text cached in
  PSRAM (network, update, warning), and page-specific updates check for NULL.
