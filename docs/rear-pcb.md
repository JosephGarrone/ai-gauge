# Rear PCB

A small round board soldered to the 8-pin expansion header on the back of the display. It takes
vehicle 12V, powers the display, carries the sensor front-end from
[sensor-frontend.md](sensor-frontend.md), and passes power on to the next gauge. One design
serves both the boost and the EGT gauge.

> **Status: planning.** Nothing here has been drawn, built or measured. Facts about the display
> board are marked with where they came from. Everything else is a proposal until the
> *Verification* list says otherwise. Do not describe any of this as working.

## Requirements

| # | Requirement |
|---|---|
| R1 | Fits inside a **45mm diameter circle** |
| R2 | Solders directly to the display's 8-pin header; no flying leads between the two boards |
| R3 | Powered from vehicle 12V through the JRP 4-wire power lead |
| R4 | Two identical power connectors, wired pin-for-pin, so gauges can be **daisy-chained** |
| R5 | The same board works as a boost gauge (3-pin MAP sensor) or an EGT gauge (2-pin K-type) |
| R6 | Boost or EGT selected per gauge. Met by the face config, not a switch — see [Mode selection](#mode-selection--no-switch-needed) |
| R7 | ~~Speaker~~ Out of scope: a speaker plugs straight into the display's H3 connector ([hardware-reference.md](hardware-reference.md)) |
| R8 | **Every board is identical and swappable** between boost and EGT gauges: no soldering, jumpers, switches or part variants |

## Block diagram

```
 JRP 4-wire in ──┬── J1 ──┐                            ┌── J3  boost MAP sensor (3-pin)
                 │        │ BATT, IGN, spare, GND      │
 JRP 4-wire out ─┴── J2 ──┘ paralleled pin-for-pin     │   ┌── J4  EGT thermocouple (2-pin)
                    │                                  │   │
                    IGN                                │   │
                    │                                  │   │
        fuse → TVS → reverse-polarity → 12V→5V buck ───┼── 5V sensor supply (current-limited)
                                          │            │   │
                                    reverse-block      │   │
                                          │         divider│
                                          ▼            │   │
                                   H2 pin 1 (VBUS)  ADS1115 MCP9600 ── I2C ── H2 pins 6/7/8
                                   H2 pin 2 (GND)      ▲
                                   H2 pin 3 (3V3) ─────┴── logic supply, I2C pull-ups
```

Both sensor chips share the one I2C bus; neither needs its own header pin.

## Interface to the display board

### The header

The header is **2.54mm pitch, 8 pins** (upstream `HARDWARE_REFERENCE.md` and Waveshare docs).

| Header pin | Display signal | ESP32-S3R8 pin | Rear PCB role |
|---|---|---|---|
| 1 | VBUS | — | 5V in, through the reverse-current block |
| 2 | GND | — | Ground |
| 3 | 3V3 | — | Logic supply for the sensor chips and pull-ups |
| 4, 5 | UART0 | — | Not connected. UART0 stays free for recovery; never drive it from this board |
| 6 | GPIO16 | 22 | I2C SDA |
| 7 | GPIO17 | 23 | I2C SCL |
| 8 | GPIO18 | 24 | ALERT / DRDY (open-drain, ADS1115) |

Pins 6–8 were confirmed by the project owner (2026-09-14). They match the upstream schematic's
header symbol (H2). Upstream `HARDWARE_REFERENCE.md` lists them as 17, 18, 16, which is wrong.
GPIO16 is the ESP32-S3's XTAL_32K_N pad; this board routes it to the header, not to a crystal.

Still to find out: the header's gender and its position relative to the display's centre.
Measure it, or take it from Waveshare's 3D model (listed under Resources on the Waveshare wiki).
Our board's outline and the placement of every part depend on it.

### Power the display through VBUS (pin 1), not 3V3

From the upstream schematic:

- **3V3 (pin 3)** is `VCC3V3`, the output of the AXP2101's DCDC1. Feeding a second supply into
  a regulator output makes the two fight each other. Use pin 3 only as a **supply** for this
  board's logic (ADS1115, MCP9600, I2C pull-ups), which draws a few mA.
- **VBUS (pin 1)** is the same net as the USB-C connector's VBUS and the AXP2101 VBUS input.
  Its only protection is a `LTVS16H5.0ET5G` TVS diode at the USB connector. There is no diode
  between USB and the header.

So the 5V from our buck goes to **pin 1**, with two conditions:

1. **Block reverse current.** Without it, plugging USB in with the car on pushes our 5V back
   into the laptop, and USB power flows backwards into our buck. Use an ideal-diode or
   reverse-blocking load switch, or a Schottky diode. A Schottky costs ~0.3–0.4V, which the
   AXP2101 should tolerate, but that needs confirming against its datasheet.
2. **Regulate to 5.0V, not higher.** The USB TVS is rated for 5.0V. A buck set to 5.3V "to
   make up for the diode drop" would make that TVS conduct.

The display's current draw has **not been measured**. We know only that a front-panel USB port
could not supply it once WiFi started ([build-and-flash.md](build-and-flash.md)). **Design target: 1.5A
at 5V** (project owner). U1 delivers 2A; U2's absolute-maximum continuous rating is 1.5A, so that
is the ceiling.

## Power input and daisy chain

### JRP power lead

4-wire, left to right as supplied:

| Position | Colour | Function | Use on this board |
|---|---|---|---|
| 1 | Red | 12V battery (permanent) | Passed through J1↔J2. Not used locally (see below) |
| 2 | White | 12V ignition | Passed through, **and feeds this board's supply** |
| 3 | Yellow | Unused | Passed through, otherwise unconnected |
| 4 | Black | Ground | Passed through, board ground |

**"Left to right" is defined** (project owner): looking from the plug side as it is pushed into
the socket, with the locating tabs facing up, position 1 is the leftmost wire. The same definition
applies to J3 and J4. Which JST pin number position 1 lands on is checked once with a real PH
header, which carries JST's circuit-1 mark, during the Q3 measurement. The footprint follows that check.

**Connector family: JST PH (2.0mm pitch) is assumed but not confirmed.** JST XH (2.5mm) and ZH
(1.5mm) look alike. Measure the pitch across all four pins with calipers: PH ≈ 6.0mm, XH ≈
7.5mm, ZH ≈ 4.5mm.

### Daisy chain

J1 and J2 are the same connector, wired **pin-for-pin in parallel**. Either can be the input.
All pass-through current flows through board-level copper and connector contacts:

- The first board in a chain carries **the current of every gauge after it**. JST's catalogue
  rates PH contacts at 2A. Keep the chain's total under that, and fuse the vehicle feed at or
  below it. At the full 1.5A-at-5V target, each gauge draws about 0.74A from 12V, so one chain
  carries about two gauges at full load. More fit at typical loads.
- Pass-through copper traces are sized for the full chain current, not one gauge.
- Each board **fuses only its own supply branch** (after the tap from the chain). A shorted
  buck then blows its own fuse, not the whole chain.

### Ignition vs battery

The board runs **only from ignition**, so it cannot flatten the battery. Battery 12V is passed
through untouched. It has no job yet. Possible later uses, none planned:

- Battery-voltage readout through a divider into a spare ADS1115 channel (AIN1/AIN2 are free).
- A deliberate shutdown, e.g. saving min/max to flash when ignition drops. This would need a
  battery-fed supply with very low quiescent current, and is out of scope for v1.

### Protection chain

From [sensor-frontend.md](sensor-frontend.md#power-and-automotive-protection), in order along
the IGN branch:

1. **Fuse** — resettable PTC on this board's branch.
2. **Transient / load-dump TVS** — bidirectional, placed before the reverse-polarity diode, so
   it clamps both polarities and protects that diode. Standoff above normal charging voltage
   (~14.4V); clamp below the buck's absolute maximum.
3. **Reverse polarity** — a series Schottky diode. Its ~0.5V drop is irrelevant ahead of a
   buck, and it is simpler than a P-channel MOSFET.
4. **12V → 5V buck** — wide input range, ≥1A, small: this dominates the board area.
5. **Input/output filtering** — keep buck switching noise off the 5V sensor supply and the
   ADS1115 ground.

Chosen parts, pinouts and wiring diagrams: [rear-pcb-parts.md](rear-pcb-parts.md).

## Sensor connectors

Both front-ends are fitted on every board. The circuit and the scaling are in
[sensor-frontend.md](sensor-frontend.md); this section covers only what the PCB adds.

### J3 — boost MAP sensor, 3-pin

| Position (left to right) | Colour | Function |
|---|---|---|
| 1 | Black | GND |
| 2 | White | Signal, 0–5V |
| 3 | Red | 5V supply |

"Left to right" as defined for the power lead. The colours come from the harness; the JRP
manual has not been checked for this ([sensor-frontend.md](sensor-frontend.md) verification list).

- **5V sensor supply** comes from the buck through a **current limit** (resistor, polyfuse or
  load switch). A pinched sensor harness must not brown out the display.
- **Signal input**: the 2:1 divider from sensor-frontend.md, plus a series resistor and clamp
  on the ADS1115 side. The divider protects against 5V, but not against the signal wire
  shorting to 12V: 12V ÷ 2 = 6V, beyond the ADS1115 absolute maximum at 3.3V.
- **Ground**: the sensor ground returns directly to the ADS1115 ground (star point), not through
  the buck's switching current path.

### J4 — EGT K-type thermocouple, 2-pin

**Polarity does matter.** A thermocouple is a voltage source. Reversed, the MCP9600 sees
temperature rises as *falls*: at idle it reads below ambient, and it goes further wrong as the
exhaust heats. The pins must be marked `T+` / `T−` on the silkscreen, and the probe's leads
identified. Common colour codes: ANSI yellow `+` / red `−`; IEC green `+` / white `−`. Confirm
against the probe actually fitted rather than assuming a standard. Two ways to identify the leads
on the probe itself: in a type K thermocouple the negative (alumel) conductor is magnetic, so a
magnet picks it out; or warm the tip and read the millivolt sign on a meter.

The copper connector here is the thermocouple's **cold junction**. That is correct, provided
the connector sits at the same temperature as the MCP9600, which measures that temperature
itself:

- Place J4 **immediately beside** the MCP9600, sharing a solid ground pour, away from the buck
  and its inductor.
- Everything upstream of J4 (probe to connector) stays K-type extension wire, as
  sensor-frontend.md already requires.
- Probe type is **unknown**: grounded (tip welded to its sheath) or insulated. A grounded probe
  bonds the thermocouple to the exhaust, and therefore to chassis ground, which adds a
  ground-offset path. Find out before finalising the input filtering.

## Mode selection — no switch needed

The original idea was a DIP switch choosing which sensor's value reaches the ESP32. With this
architecture there is nothing to switch:

- **Both sensors share one I2C bus** on header pins 6/7, at different addresses (ADS1115 `0x48`,
  MCP9600 `0x67`). The firmware reads both on every cycle over the same two wires.
- **The shared ALERT line** (pin 8) is open-drain, so both chips can drive it.
- **Which value the gauge shows is already configuration**: `<source channel="boost">` or
  `<source channel="egt">` in the face XML ([gauge-config-schema.md](gauge-config-schema.md)).

So the same board becomes a boost or EGT gauge by loading a different face. Both channels stay
live, so a gauge can also send the other channel over telemetry, or show it on a second face.

A switch would be justified only if each sensor used its own analogue wire, like an internal ADC
pin, which [ADR 0001](adr/0001-external-i2c-sensor-frontend.md) rules out.

**Decided: J3 is always powered**, through its current limit, on every board (R8). An EGT gauge
simply leaves J3 unplugged. The PH header's pins sit inside a plastic shroud, and a short on the
sensor 5V trips the limit, not the display supply. So a live, unused connector costs nothing,
where a jumper would make boards non-identical.

Rear boards are therefore fully interchangeable. What makes a unit a boost or EGT gauge is the
face config stored on the display, which does not change when the rear board is swapped. If a
hardware role selector is ever wanted anyway, **no GPIO is left to read it with**. It would have
to go into a spare ADS1115 input (AIN3) and be read at startup.

## Board outline and layout

| Quantity | Value |
|---|---|
| Outline | Circle, 45mm diameter |
| Area | ≈1590mm² per side |
| Largest square inside it | ≈31.8mm × 31.8mm |
| Stack-up | 2-layer, 1.6mm (proposal) |

Approximate footprint widths for JST PH top-entry headers, from `2.0mm × (pins − 1) + ~4mm`
(confirm against the JST drawing):

| Part | Approx. width | Qty |
|---|---|---|
| 4-pin (J1, J2) | ~10mm | 2 |
| 3-pin (J3) | ~8mm | 1 |
| 2-pin (J4) | ~6mm | 1 |

Four connectors total ~34mm of edge. The circumference is ~141mm, so they fit around the edge
with the wires leaving outward. Top-entry vs side-entry depends on how much depth there is behind
the display in the housing.

Layout priorities:

1. **Header position first** — everything else is placed around wherever the display's header
   actually is.
2. **Keep clear of the display board's rim features**: USB-C, buttons, the microSD slot and the
   battery connector must stay reachable, or at least not be crushed.
3. **Split noisy and quiet**: buck and inductor on one side; ADS1115, MCP9600 and J4 on the other.
4. **Strain relief**: harness pulls must not go into the 8-pin header's solder joints. Add a
   cable-tie slot or adhesive standoff.
5. **Assembly order**: the rear PCB is soldered to a board with a display on it. Keep tall and
   hot-to-solder parts where an iron can reach, or assemble the PCB first and solder the header
   last.

## Open questions

| # | Question | Blocks |
|---|---|---|
| Q1 | ~~Header pin order~~ **Resolved:** pins 6/7/8 = GPIO16/17/18 | — |
| Q3 | ~~JRP connector family~~ **Resolved:** JST PH assumed (matches the owner's recollection) | — |
| Q4 | ~~"Left to right"~~ **Resolved:** leftmost wire, looking from the plug side while inserting, locating tabs up | — |
| Q5 | ~~EGT probe polarity~~ **Not a PCB question:** J4 is marked + and −; the probe's leads are identified when the harness is made (see *Verification*) | — |
| Q6 | ~~Display current~~ **Resolved:** design target 1.5A at 5V (owner); U2's rating is the ceiling | — |

No open questions remain. Q2 (header gender and position) and Q8 (depth behind the display) are
handled directly by the project owner during layout. Q7 (speaker) is out of scope: see requirement R7.

## Verification

- [x] Q1: header pins 6/7/8 = GPIO16/17/18 (confirmed by project owner; matches the upstream schematic)
- [ ] Before ordering: plug a JRP lead into a real PH header and confirm position 1 lands on JST's
  circuit-1 pin. If J3 is laid out backwards, the MAP sensor gets 5V and GND swapped
- [ ] Harness: identify the EGT probe's + and − leads (type K negative is magnetic) and wire them to
  J4's + and −. Optionally check for tip-to-sheath continuity (grounded probe → consider fitting C16)
- [ ] Daisy chain: two boards, measure the drop across the first board at full chain current
- [ ] Reverse-polarity and over-voltage tests on the bench before any vehicle connection
