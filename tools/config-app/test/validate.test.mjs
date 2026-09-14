import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseGauge } from '../js/parse.js';
import { validateModel } from '../js/validate.js';
import { shippedFaces } from './corpus.mjs';

const boost = () => parseGauge(shippedFaces().find((f) => f.name === 'boost.xml').xml).model;
const boostCustom = () => parseGauge(shippedFaces().find((f) => f.name === 'boost_custom.xml').xml).model;

const having = (m, severity, pattern) =>
    validateModel(m).issues.filter((i) => i.severity === severity && pattern.test(i.message));

test('shipped faces have no errors or warnings', () => {
    for (const { name, xml } of shippedFaces()) {
        const { issues, device } = validateModel(parseGauge(xml).model);
        const serious = issues.filter((i) => i.severity !== 'info');
        assert.deepEqual(serious, [], name);
        assert.equal(device.ok && device.warnings.length, 0, name);
    }
});

test('ids, ranges and required strings', () => {
    const m = boost();
    m.id = 'has space';
    m.source.channel = '';
    m.source.max = m.source.min;
    const errors = validateModel(m).issues.filter((i) => i.severity === 'error').map((i) => i.message).join('\n');
    assert.match(errors, /Id 'has space'/);
    assert.match(errors, /Channel is required/);
    assert.match(errors, /Max must be greater than min/);
    assert.match(errors, /would reject/);
});

test('formats, fonts and characters', () => {
    const m = boost();
    m.readout.format = '%d';
    m.labels.font = 'montserrat_14';
    m.readout.suffix = ' °C';
    m.titles[0].text = 'A & B';
    assert.equal(having(m, 'error', /Readout format '%d'/).length, 1);
    assert.equal(having(m, 'error', /not a device font/).length, 1);
    assert.equal(having(m, 'warning', /'°'/).length, 1);
    assert.equal(having(m, 'error', /& </).length, 1);
});

test('label text longer than the device buffer', () => {
    const m = boost();
    m.labels.format = '%.6f psi units';
    assert.equal(having(m, 'warning', /longer than the 15 characters/).length, 1);
});

test('bands outside the scale, and alerts that can never fire', () => {
    const m = boost();
    m.bands[2].to = 40;
    m.alerts[0].above = 30;
    assert.equal(having(m, 'error', /to 40 is outside the scale/).length, 1);
    assert.equal(having(m, 'warning', /can never fire/).length, 1);
    // The clamp is explained, so the device backstop adds nothing on top of it.
    assert.equal(having(m, 'error', /would report/).length, 0);
});

test('shape geometry', () => {
    const m = boostCustom();
    // An asymmetric bow tie: a symmetric one has zero signed area and is reported as empty instead.
    m.needle.shape.parts.push({ kind: 'polygon', points: [[0, 0], [30, 10], [30, 0], [0, 20]], color: null });
    assert.equal(having(m, 'error', /edges cross/).length, 1);

    const ring = Array.from({ length: 70 }, (_, i) => [10 * Math.cos(i / 11), 10 * Math.sin(i / 11)]);
    m.needle.hub.parts = [{ kind: 'polygon', points: ring, color: null }];
    assert.equal(having(m, 'error', /70 polygon vertices/).length, 1);
});

test('needle shapes beyond the tested footprint', () => {
    const m = boostCustom();
    m.needle.shape.parts[0].points = [[0, -220], [30, 0], [-30, 0]];
    assert.equal(having(m, 'warning', /footprint/).length, 1);
});

test('ticks that run into each other', () => {
    const m = boostCustom();
    m.ticks.minorShape.parts[0].r = 20;
    assert.equal(having(m, 'warning', /Minor ticks are .* apart/).length, 1);
});

test('the device backstop catches what the checks do not', () => {
    const m = boost();
    m.panel.shape = 'hexagon';
    assert.equal(having(m, 'error', /would report 1 warning/).length, 1);
});

test('file size', () => {
    const m = boost();
    m.bands = Array.from({ length: 400 }, (_, i) => ({ from: i / 20, to: i / 20 + 0.05, color: '#123456', width: 18 }));
    assert.equal(having(m, 'error', /bytes; the device accepts at most 16384/).length, 1);
    assert.equal(having(m, 'error', /400 bands/).length, 1);
});
