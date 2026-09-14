/*
 * XML documents that exercise the parser: the shipped faces, the cases from the firmware's own host
 * tests, parser quirks, and seeded random documents. Shared by the parser tests and the
 * differential test against the C parser.
 */

import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const GAUGES_DIR = join(dirname(fileURLToPath(import.meta.url)), '../../../firmware/assets/gauges');

export function shippedFaces() {
    return readdirSync(GAUGES_DIR)
        .filter((f) => f.endsWith('.xml'))
        .map((f) => ({ name: f, xml: readFileSync(join(GAUGES_DIR, f), 'utf8') }));
}

const src = '<source channel="c" min="0" max="10"/>';
const doc = (body) => `<gauge version="1" id="x">${src}${body}</gauge>`;
const needleShape = (body) => doc(`<needle><shape>${body}</shape></needle>`);

function ring(n, r) {
    return Array.from({ length: n }, (_, i) => {
        const a = (i * 2 * 3.14159265) / n;
        return `${(r * Math.cos(a)).toFixed(2)},${(r * Math.sin(a)).toFixed(2)}`;
    }).join(' ');
}

/** Hand-written cases: name -> xml. */
export const CASES = {
    minimal: doc(''),
    garbage: 'not xml at all',
    wrongRoot: '<other version="1"/>',
    missingVersion: '<gauge id="x"><source channel="c" min="0" max="1"/></gauge>',
    futureVersion: '<gauge version="99" id="x"/>',
    fractionalVersion: '<gauge version="1.5" id="x">' + src + '</gauge>',
    missingId: '<gauge version="1"/>',
    missingSource: '<gauge version="1" id="x"/>',
    missingMax: '<gauge version="1" id="x"><source channel="c" min="0"/></gauge>',
    missingChannel: '<gauge version="1" id="x"><source min="0" max="1"/></gauge>',
    invertedRange: '<gauge version="1" id="x"><source channel="c" min="30" max="0"/></gauge>',
    emptyRange: '<gauge version="1" id="x"><source channel="c" min="5" max="5"/></gauge>',
    unknownContent: '<gauge version="1" id="x" future-attr="whatever">' + src +
        '<hologram depth="3" shimmer="true"/><needle width="9" glow="#ff0000"/></gauge>',
    dampingClamp: '<gauge version="1" id="x"><source channel="c" min="0" max="10" damping="5"/></gauge>',
    bandClamp: doc('<band from="-5" to="50" color="#fff"/>'),
    badColour: doc('<needle color="not-a-colour"/>'),
    thresholdlessAlert: doc('<alert color="#fff"/>'),
    zeroBand: doc('<band from="5" to="5" color="#fff"/>'),
    tooManyBands: doc(Array.from({ length: 13 }, (_, i) => `<band from="${i}" to="${i + 1}" color="#fff"/>`).join('')),
    tooManyTitles: doc(Array.from({ length: 6 }, (_, i) => `<title text="T${i}" y="${i * 10}"/>`).join('')),
    tooManyAlerts: doc(Array.from({ length: 6 }, (_, i) => `<alert above="${i}"/>`).join('')),
    longId: '<gauge version="1" id="' + 'a'.repeat(52) + '">' + src + '</gauge>',
    longStrings: doc('<title text="' + 'W'.repeat(40) + '"/><readout prefix="' + 'p'.repeat(40) +
        '" suffix="' + 's'.repeat(40) + '" format="%.1f and some more text" font="montserrat_24_but_much_longer"/>'),
    utf8Truncation: '<gauge version="1" id="x"><source channel="c" unit="°°°°°°°°°" min="0" max="10"/>' +
        '<title text="' + 'é'.repeat(20) + '"/></gauge>',
    fullDocument: `<?xml version="1.0" encoding="UTF-8"?>
<!-- a comment, with a > inside it -->
<gauge version="1" id="boost">
  <panel shape="round" width="466" height="466"/>
  <source channel="boost" unit="psi" min="0" max="30" damping="0.2"/>
  <face start-angle="225" sweep="270" background="#101010">
    <band from="0" to="18" color="#00c853"/>
    <band from="18" to="25" color="#ffab00" width="22"/>
    <band from="25" to="30" color="#d50000"/>
    <ticks major-every="5" minor-every="1" major-len="24" color="#fff"/>
    <labels every="5" font="montserrat_24" radius="150"/>
  </face>
  <needle style="arrow" length="170" width="14" color="#ff1744"/>
  <title text="BOOST" y="150"/>
  <readout y="320" format="%.1f" suffix=" psi"/>
  <alert above="25" flash-hz="2" chime="true"/>
</gauge>
`,
    peak: doc('<peak color="#00ff00" length="30" width="6" show-value="false" value-y="380" format="%.0f" prefix="MAX "/>'),
    barePeak: doc('<peak/>'),
    boolVariants: doc('<peak show-value="0"/><alert above="1" chime="1"/><alert below="1" chime="yes"/><alert above="2" chime="true "/>'),

    shapes: doc(`<face><ticks major-every="5" minor-every="1" color="#ffffff">
        <major-shape><polygon points="-3,0 3,0 1.5,26 -1.5,26"/></major-shape>
        <minor-shape color="#808080"><polygon points="-1 0  1 0\n1 12 -1 12"/></minor-shape>
      </ticks></face>
      <needle color="#ff1744"><shape><polygon points="0,-170 9,0 0,30 -9,0"/><circle r="10" color="#202020"/></shape>
      <hub color="#333"><circle r="18"/><circle cx="1" cy="-2" r="6" color="#ff1744"/></hub></needle>`),
    shapeContext: doc('<shape><polygon points="0,0 10,0 0,10"/></shape><hub><circle r="5"/></hub>' +
        '<needle><major-shape><polygon points="0,0 10,0 0,10"/></major-shape></needle>' +
        '<ticks><polygon points="0,0 10,0 0,10"/><shape><circle r="3"/></shape></ticks>' +
        '<needle/><polygon points="0,0 10,0 0,10"/>' +
        '<needle><shape><polygon points="0,-10 2,0 -2,0"/></shape></needle><circle r="4"/>'),
    twoPoints: needleShape('<polygon points="0,0 10,0"/>'),
    oddCoords: needleShape('<polygon points="0,0 10,0 5"/>'),
    junkPoints: needleShape('<polygon points="0,0 10,x 5,5"/>'),
    noPoints: needleShape('<polygon/>'),
    zeroArea: needleShape('<polygon points="0,0 10,0 20,0"/>'),
    tinyArea: needleShape('<polygon points="0,0 0.5,0 0,0.9"/>'),
    circleNoR: needleShape('<circle/>'),
    circleZeroR: needleShape('<circle r="0"/>'),
    circleNegR: needleShape('<circle r="-4"/>'),
    circleTinyR: needleShape('<circle r="0.01"/>'),
    circleHalfUnit: needleShape('<circle r="0.03125"/>'),
    coordClamp: needleShape('<polygon points="0,0 3000,0 0,10"/>'),
    circleClamp: needleShape('<circle cx="-5000" cy="3000" r="9000"/>'),
    numberForms: needleShape('<polygon points="1e1,-2.5 .5.5 3-4"/>'),
    roundingForms: needleShape('<polygon points="0.03,0.3 10.0937,0.0312 -0.0313,9.96875 +4e-1,2E+1 5e,6"/>'),
    partCap: needleShape(Array.from({ length: 8 }, (_, i) => `<circle r="${i + 1}"/>`).join('')),
    pointPool: needleShape(`<polygon points="${ring(40, 50)}"/><polygon points="${ring(30, 20)}"/><circle r="3"/>`),
    exactPool: needleShape(`<polygon points="${ring(32, 50)}"/><polygon points="${ring(32, 20)}"/>`),
    badSlotColour: doc('<needle><hub color="nope"><circle r="5"/></hub></needle>'),
    repeatedSlot: doc('<needle><shape><circle r="5"/><circle r="6"/></shape><shape><circle r="9"/></shape></needle>'),
    selfClosingSlotClears: doc('<needle><shape><circle r="5"/></shape><shape/></needle>'),
    emptySlot: needleShape(''),
    slotColourOnly: doc('<needle><shape color="#123456"></shape></needle>'),

    /* Scanner quirks. */
    singleQuotes: "<gauge version='1' id='q'><source channel='c' min='0' max='10'/><title text='say \"hi\"' y='3'/></gauge>",
    quotedGt: doc('<title text="a > b" y="1"/>'),
    spacedEquals: doc('<title  text = "spaced"  y =  "4" />'),
    duplicateAttr: doc('<title text="first" text="second" y="1" y="2"/>'),
    valuelessAttr: doc('<title hidden text="shown" y="7"/>'),
    unquotedAttr: doc('<title text=bare y="7"/><title text="ok" y=8/>'),
    doctype: '<!DOCTYPE gauge>' + doc(''),
    prologAfterRoot: doc('<?pi stuff?><!-- c --><title text="t"/>'),
    unterminatedComment: doc('<!-- never closed <title text="t"/>'),
    textWithLt: doc('<title text="before"/> 1 < 2 <title text="after"/>'),
    unterminatedTag: doc('<title text="t"'),
    closeBeforeRoot: '</x><gauge version="1" id="x">' + src + '</gauge>',
    contentAfterRoot: doc('') + '<title text="late"/>',
    bandBeforeSource: '<gauge version="1" id="x"><band from="50" to="200"/>' + src + '</gauge>',
    repeatedTicks: doc('<ticks major-every="2" major-len="30"/><ticks minor-every="0.5"/>'),
    repeatedElements: doc('<readout y="10" prefix="A"/><readout suffix="B"/><labels font="montserrat_12"/><labels every="2"/>'),
    numberQuirks: doc('<face start-angle="-450.5" sweep="0x1p4" radius="  12.9"/><needle length="1e2" width="0x10" tail="nan" pivot-radius="inf"/><title text="n" x="-5000" y="4096.9"/>'),
    longNumber: doc('<needle length="0.000000000000000000000000000000000000001"/><band from="1" to="00000000000000000000000000000000009"/>'),
    emptyNumber: doc('<needle length=""/><face sweep=" "/>'),
    tickEdge: '<gauge version="1" id="x"><source channel="c" min="0" max="1"/><ticks major-every="0.1" minor-every="1e-9"/><labels every="-1"/></gauge>',
    panelSquare: doc('<panel shape="square" width="-3" height="99999"/><panel shape="hexagon"/>'),
    styles: doc('<needle style="line"/><needle style="sword"/>'),
    alertBoth: doc('<alert above="8" below="2" flash-hz="30" color="#abc"/>'),
    shortColours: doc('<face background="#AbC"/><band from="1" to="2" color="123456"/><band from="2" to="3" color="#12345"/><band from="3" to="4" color="#ggg"/>'),
};

/** Deterministic PRNG (mulberry32) so a failing random case can be reproduced. */
function rng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/**
 * Random documents built from the schema's vocabulary with hostile values mixed in: numbers out of
 * range, malformed colours, over-long strings, shapes in and out of context, broken point lists.
 */
export function randomDocuments(count, seed = 1) {
    const r = rng(seed);
    const pick = (arr) => arr[Math.floor(r() * arr.length)];
    const num = () => pick([
        () => String(Math.round((r() * 2 - 1) * 100)),
        () => ((r() * 2 - 1) * 5000).toFixed(pick([0, 1, 3, 6])),
        () => (r() * 30).toFixed(4),
        () => pick(['0', '1', '-0', '1e3', '2.5e-1', '0x1A', '', ' 7', '7px', 'abc', '.5', '5.', '360', '361', '4097', '-4097']),
    ])();
    const colour = () => pick(['#fff', '#123456', '#ABCDEF', 'red', '#12', '#1234567', '00c853', '#0f0']);
    const text = () => pick(['', 'BOOST', 'x'.repeat(Math.floor(r() * 40)), 'a b', 'ünïcödé', 'P&amp;Q']);
    const font = () => pick(['montserrat_12', 'montserrat_24', 'montserrat_48', 'montserrat_14', 'comic_sans', '']);
    const fmt = () => pick(['%.1f', '%g', '%.0f psi', '%d', 'x'.repeat(20)]);
    const bool = () => pick(['true', 'false', '1', '0', 'yes', 'TRUE']);
    const attrs = (spec) => spec
        .filter(() => r() < 0.7)
        .map(([name, gen]) => `${name}="${gen()}"`)
        .join(' ');
    const points = () => {
        const n = pick([0, 1, 2, 3, 4, 5, 8, 20, 40, 70]);
        let s = Array.from({ length: n }, () => `${num()},${num()}`).join(pick([' ', ',', '  ', '\n']));
        if (r() < 0.1) s += ' 3';
        return s;
    };
    const part = () => (r() < 0.6
        ? `<polygon ${attrs([['points', points], ['color', colour]])}/>`
        : `<circle ${attrs([['cx', num], ['cy', num], ['r', num], ['color', colour]])}/>`);
    const slot = (name) => {
        const open = `<${name} ${attrs([['color', colour]])}`;
        if (r() < 0.15) return `${open}/>`;
        return `${open}>${Array.from({ length: Math.floor(r() * 9) }, part).join('')}</${name}>`;
    };

    const elements = [
        () => `<panel ${attrs([['shape', () => pick(['round', 'square', 'oval'])], ['width', num], ['height', num]])}/>`,
        () => `<face ${attrs([['start-angle', num], ['sweep', num], ['background', colour], ['radius', num]])}>`,
        () => '</face>',
        () => `<band ${attrs([['from', num], ['to', num], ['color', colour], ['width', num]])}/>`,
        () => {
            const a = attrs([['major-every', num], ['minor-every', num], ['major-len', num], ['minor-len', num],
                ['major-width', num], ['minor-width', num], ['color', colour]]);
            if (r() < 0.3) return `<ticks ${a}/>`;
            return `<ticks ${a}>${[slot('major-shape'), slot('minor-shape'), r() < 0.2 ? part() : ''].join('')}</ticks>`;
        },
        () => `<labels ${attrs([['every', num], ['font', font], ['color', colour], ['radius', num], ['format', fmt]])}/>`,
        () => {
            const a = attrs([['style', () => pick(['taper', 'line', 'arrow', 'sword'])], ['length', num], ['width', num],
                ['tail', num], ['color', colour], ['pivot-radius', num]]);
            if (r() < 0.3) return `<needle ${a}/>`;
            return `<needle ${a}>${[slot('shape'), slot('hub'), r() < 0.2 ? slot('major-shape') : ''].join('')}</needle>`;
        },
        () => `<title ${attrs([['text', text], ['x', num], ['y', num], ['font', font], ['color', colour]])}/>`,
        () => `<readout ${attrs([['x', num], ['y', num], ['font', font], ['format', fmt], ['prefix', text], ['suffix', text], ['color', colour]])}/>`,
        () => `<peak ${attrs([['color', colour], ['length', num], ['width', num], ['show-value', bool], ['value-y', num], ['font', font], ['format', fmt], ['prefix', text]])}/>`,
        () => `<alert ${attrs([['above', num], ['below', num], ['color', colour], ['flash-hz', num], ['chime', bool]])}/>`,
        () => pick(['<!-- note -->', '<unknown a="1"/>', slot('hub'), part(), '</needle>', '</ticks>']),
    ];

    const docs = [];
    for (let i = 0; i < count; i++) {
        const min = pick(['0', '-10', '100', '0.5']);
        const max = r() < 0.05 ? min : pick(['30', '900', '1', '100.25']);
        const source = `<source channel="${pick(['boost', 'egt', 'x'.repeat(40)])}" unit="${text()}" min="${min}" max="${max}" ${r() < 0.5 ? `damping="${num()}"` : ''}/>`;
        const body = Array.from({ length: 3 + Math.floor(r() * 14) }, () => pick(elements)()).join('\n');
        const sourceFirst = r() < 0.9;
        docs.push({
            name: `random-${seed}-${i}`,
            xml: `<gauge version="1" id="${pick(['r', 'boost_2', 'y'.repeat(35)])}">\n${sourceFirst ? source + body : body + source}\n</gauge>`,
        });
    }
    return docs;
}

export function fullCorpus() {
    return [
        ...shippedFaces(),
        ...Object.entries(CASES).map(([name, xml]) => ({ name, xml })),
        ...randomDocuments(400, 7),
    ];
}
