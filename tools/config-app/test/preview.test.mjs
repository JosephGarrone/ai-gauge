import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseGauge } from '../js/parse.js';
import { renderFace, renderNeedle, renderPeakText, renderReadout, renderStaticSvg } from '../js/render.js';
import { createSim, needleAlertColor, readoutColor, resetPeak, stepSim } from '../js/sim.js';
import { shippedFaces } from './corpus.mjs';

const face = (name) => parseGauge(shippedFaces().find((f) => f.name === name).xml).model;
const count = (s, re) => (s.match(re) ?? []).length;

test('damping and peak-hold', () => {
    const m = face('boost.xml');
    const st = createSim(m);
    stepSim(m, st, 10, 16);
    assert.ok(Math.abs(st.displayed - 8.5) < 1e-5, 'damping 0.15 keeps 15% of the error');
    stepSim(m, st, 50, 16);
    assert.equal(st.peak, 30, 'peak follows the raw value, clamped to max');
    stepSim(m, st, 5, 16);
    assert.equal(st.peak, 30, 'and holds');
    resetPeak(st);
    assert.equal(st.peak, null);

    m.source.damping = 0;
    stepSim(m, st, 12, 16);
    assert.equal(st.displayed, 12);
});

test('alerts flash at the configured rate', () => {
    const m = face('boost.xml');
    m.source.damping = 0;
    const st = createSim(m);

    stepSim(m, st, 20, 16);
    assert.equal(st.alertActive, false);
    assert.equal(readoutColor(m, st), '#ffffff');

    stepSim(m, st, 28, 0);
    assert.equal(needleAlertColor(m, st), '#d50000');
    assert.equal(readoutColor(m, st), '#d50000');

    stepSim(m, st, 28, 250); // flash-hz 2 toggles every 250 ms
    assert.equal(needleAlertColor(m, st), null);
    stepSim(m, st, 28, 250);
    assert.equal(needleAlertColor(m, st), '#d50000');

    m.alerts[0].flashHz = 0;
    const steady = createSim(m);
    stepSim(m, steady, 28, 0);
    stepSim(m, steady, 28, 5000);
    assert.equal(needleAlertColor(m, steady), '#d50000');
});

test('alert handover keeps the first alert for the needle, as the firmware does', () => {
    const m = face('boost.xml');
    m.source.damping = 0;
    m.alerts = [
        { above: 25, below: null, color: '#ff0000', flashHz: 0, chime: false },
        { above: null, below: 5, color: '#0000ff', flashHz: 0, chime: false },
    ];
    const st = createSim(m);
    stepSim(m, st, 30, 16);
    stepSim(m, st, 0, 16);
    assert.equal(needleAlertColor(m, st), '#ff0000');
    assert.equal(readoutColor(m, st), '#0000ff');
});

test('face renders ticks, labels and titles', () => {
    const svg = renderFace(face('boost.xml'));
    assert.equal(count(svg, /<line /g), 31 + 7);
    assert.deepEqual([...svg.matchAll(/data-section="labels">(.*?)<\/g>/g)].length, 1);
    for (const label of ['0', '5', '10', '15', '20', '25', '30']) assert.match(svg, new RegExp(`>${label}</text>`));
    assert.match(svg, />BOOST<\/text>/);
    assert.equal(count(svg, /<path /g), 3, 'three bands');
});

test('shaped ticks are stamped from one definition', () => {
    const svg = renderFace(face('boost_custom.xml'), 't');
    assert.equal(count(svg, /<use href="#t-major-tick"/g), 7);
    assert.equal(count(svg, /<use href="#t-minor-tick"/g), 31);
    assert.match(svg, /<g id="t-minor-tick"><circle cx="0" cy="5" r="2.5" fill="#9e9e9e"\/>/);
});

test('only inheriting needle parts take the alert colour', () => {
    const m = face('boost_custom.xml');
    const svg = renderNeedle(m, 300, '#00ff00');
    assert.match(svg, /<polygon points="0,-176[^"]*" fill="#00ff00"/);
    assert.match(svg, /fill="#ffffff"/, 'the stripe keeps its own colour');
});

test('readout and peak text', () => {
    const m = face('boost.xml');
    assert.match(renderReadout(m, 12.34), />12.3 psi<\/text>/);
    assert.match(renderPeakText(m, null), />PEAK --<\/text>/);
    assert.match(renderPeakText(m, 21), />PEAK 21.0<\/text>/);
    assert.match(renderStaticSvg(m, 10), /^<svg /);
});
