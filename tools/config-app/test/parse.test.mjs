/*
 * The parser port, checked against the behaviour tools/host-tests/test_gauge_config.c asserts for the
 * firmware. The differential test compares the two parsers directly; these keep the documented
 * contract readable and run without a C toolchain.
 */

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseGauge } from '../js/parse.js';
import { ERRORS } from '../js/schema.js';
import { CASES } from './corpus.mjs';

const ok = (xml) => {
    const r = parseGauge(xml);
    assert.equal(r.ok, true, `should parse: ${r.error}`);
    return r;
};
const near = (a, b) => Math.abs(a - b) < 1e-4;

test('minimal document takes the documented defaults', () => {
    const { model: m, warnings } = ok(CASES.minimal);
    assert.equal(m.id, 'x');
    assert.equal(m.source.channel, 'c');
    assert.equal(warnings.length, 0);
    assert.equal(m.face.startAngle, 225);
    assert.equal(m.face.sweep, 270);
    assert.ok(near(m.source.damping, 0.15));
    assert.equal(m.needle.style, 'taper');
    assert.equal(m.needle.pivotRadius, 16);
    assert.equal(m.ticks, null);
    assert.equal(m.peak, null);
});

test('full document', () => {
    const { model: m, warnings } = ok(CASES.fullDocument);
    assert.equal(warnings.length, 0);
    assert.equal(m.panel.width, 466);
    assert.equal(m.source.unit, 'psi');
    assert.ok(near(m.source.damping, 0.2));
    assert.equal(m.face.background, '#101010');
    assert.equal(m.bands.length, 3);
    assert.equal(m.bands[1].width, 22);
    assert.equal(m.bands[0].color, '#00c853');
    assert.equal(m.ticks.minorEvery, 1);
    assert.equal(m.ticks.color, '#ffffff', '#fff expands to #ffffff');
    assert.equal(m.ticks.minorLen, 10);
    assert.equal(m.labels.radius, 150);
    assert.equal(m.needle.style, 'arrow');
    assert.equal(m.titles.length, 1);
    assert.equal(m.titles[0].text, 'BOOST');
    assert.equal(m.titles[0].x, null, 'title x defaults to centred');
    assert.equal(m.readout.suffix, ' psi');
    assert.equal(m.alerts.length, 1);
    assert.equal(m.alerts[0].chime, true);
    assert.equal(m.alerts[0].below, null);
});

test('rejections', () => {
    const cases = {
        garbage: ERRORS.NO_ROOT,
        wrongRoot: ERRORS.NO_ROOT,
        missingVersion: ERRORS.BAD_VERSION,
        futureVersion: ERRORS.BAD_VERSION,
        fractionalVersion: ERRORS.BAD_VERSION,
        missingId: ERRORS.MISSING_REQUIRED,
        missingSource: ERRORS.MISSING_REQUIRED,
        missingMax: ERRORS.MISSING_REQUIRED,
        missingChannel: ERRORS.MISSING_REQUIRED,
        invertedRange: ERRORS.BAD_RANGE,
        emptyRange: ERRORS.BAD_RANGE,
    };
    for (const [name, error] of Object.entries(cases)) {
        assert.deepEqual(parseGauge(CASES[name]), { ok: false, error }, name);
    }
});

test('unknown elements and attributes are ignored without warnings', () => {
    const { model, warnings } = ok(CASES.unknownContent);
    assert.equal(model.needle.width, 9);
    assert.equal(warnings.length, 0);
});

test('clamping and malformed colours warn', () => {
    let r = ok(CASES.dampingClamp);
    assert.equal(r.model.source.damping, 1);
    assert.equal(r.warnings.length, 1);

    r = ok(CASES.bandClamp);
    assert.equal(r.model.bands.length, 1);
    assert.equal(r.model.bands[0].from, 0);
    assert.equal(r.model.bands[0].to, 10);
    assert.equal(r.warnings.length, 2);

    r = ok(CASES.badColour);
    assert.equal(r.model.needle.color, '#ff1744');
    assert.equal(r.warnings.length, 1);
});

test('degenerate elements are dropped', () => {
    let r = ok(CASES.thresholdlessAlert);
    assert.equal(r.model.alerts.length, 0);
    assert.equal(r.warnings.length, 1);

    r = ok(CASES.zeroBand);
    assert.equal(r.model.bands.length, 0);

    r = ok(CASES.tooManyBands);
    assert.equal(r.model.bands.length, 8);
    assert.equal(r.warnings.length, 5);
});

test('strings truncate by bytes', () => {
    assert.equal(ok(CASES.longId).model.id.length, 31);
    // 9 two-byte characters cut at 15 bytes leaves half a character, which the device keeps as a raw
    // byte and a JS string can only show as U+FFFD.
    const { model } = ok(CASES.utf8Truncation);
    assert.equal(model.source.unit, '°'.repeat(7) + '�');
    assert.equal(model.titles[0].text, 'é'.repeat(15) + '�');
});

test('peak', () => {
    let { model: m, warnings } = ok(CASES.peak);
    assert.equal(warnings.length, 0);
    assert.deepEqual(m.peak, {
        color: '#00ff00', length: 30, width: 6, showValue: false, valueY: 380,
        font: 'montserrat_16', format: '%.0f', prefix: 'MAX ',
    });
    ({ model: m } = ok(CASES.barePeak));
    assert.equal(m.peak.showValue, true);
    assert.equal(m.peak.color, '#ffab00');
    assert.equal(m.peak.valueY, null);
});

test('shapes parse', () => {
    const { model: m, warnings } = ok(CASES.shapes);
    assert.equal(warnings.length, 0);

    const maj = m.ticks.majorShape;
    assert.equal(maj.parts.length, 1);
    assert.deepEqual(maj.parts[0].points[2], [1.5, 26]);
    assert.equal(maj.color, null);
    assert.equal(maj.parts[0].color, null);
    assert.equal(m.ticks.majorEvery, 5);

    const min = m.ticks.minorShape;
    assert.equal(min.parts[0].points.length, 4, 'space-separated points');
    assert.equal(min.color, '#808080');

    const ndl = m.needle.shape;
    assert.equal(ndl.parts.length, 2);
    assert.equal(ndl.parts[0].points[0][1], -170);
    assert.deepEqual(ndl.parts[1], { kind: 'circle', cx: 0, cy: 0, r: 10, color: '#202020' });

    const hub = m.needle.hub;
    assert.equal(hub.color, '#333333');
    assert.equal(hub.parts[1].cx, 1);
    assert.equal(hub.parts[1].cy, -2);
});

test('shape elements only count inside their parent', () => {
    const { model: m, warnings } = ok(CASES.shapeContext);
    assert.equal(warnings.length, 0);
    assert.equal(m.ticks.majorShape, null);
    assert.equal(m.needle.hub, null);
    assert.equal(m.needle.shape.parts.length, 1);
    assert.equal(m.needle.shape.parts[0].points[0][1], -10);
});

test('invalid parts are dropped with one warning each', () => {
    for (const name of ['twoPoints', 'oddCoords', 'junkPoints', 'noPoints', 'zeroArea', 'circleNoR',
        'circleZeroR', 'circleNegR', 'circleTinyR']) {
        const r = ok(CASES[name]);
        assert.equal(r.model.needle.shape, null, name);
        assert.equal(r.warnings.length, 1, name);
    }
});

test('shape limits and number forms', () => {
    let r = ok(CASES.coordClamp);
    assert.equal(r.model.needle.shape.parts[0].points[1][0], 2047);
    assert.equal(r.warnings.length, 1);

    r = ok(CASES.numberForms);
    assert.equal(r.warnings.length, 0);
    assert.deepEqual(r.model.needle.shape.parts[0].points, [[10, -2.5], [0.5, 0.5], [3, -4]]);

    r = ok(CASES.partCap);
    assert.equal(r.model.needle.shape.parts.length, 6);
    assert.equal(r.warnings.length, 2);

    r = ok(CASES.pointPool);
    assert.equal(r.model.needle.shape.parts.length, 2);
    assert.equal(r.model.needle.shape.parts[0].points.length, 40);
    assert.equal(r.model.needle.shape.parts[1].kind, 'circle', 'circles need no pool space');
    assert.equal(r.warnings.length, 1);

    r = ok(CASES.badSlotColour);
    assert.equal(r.model.needle.hub.color, null);
    assert.equal(r.model.needle.hub.parts.length, 1);
    assert.equal(r.warnings.length, 1);

    r = ok(CASES.repeatedSlot);
    assert.equal(r.model.needle.shape.parts.length, 1);
    assert.equal(r.model.needle.shape.parts[0].r, 9);

    r = ok(CASES.selfClosingSlotClears);
    assert.equal(r.model.needle.shape, null);

    r = ok(CASES.emptySlot);
    assert.equal(r.model.needle.shape, null);
    assert.equal(r.warnings.length, 0);
});

test('scanner quirks match the firmware', () => {
    assert.equal(ok(CASES.singleQuotes).model.titles[0].text, 'say "hi"');
    assert.equal(ok(CASES.quotedGt).model.titles[0].text, 'a > b');
    assert.equal(ok(CASES.spacedEquals).model.titles[0].y, 4);
    assert.equal(ok(CASES.duplicateAttr).model.titles[0].text, 'first');
    assert.equal(ok(CASES.textWithLt).model.titles.length, 1, 'a bare < in text ends parsing');
    assert.equal(ok(CASES.contentAfterRoot).model.titles.length, 0, '</gauge> ends parsing');
    assert.equal(ok(CASES.bandBeforeSource).model.bands[0].to, 100, 'bands clamp against the source read so far');
    assert.equal(ok(CASES.numberQuirks).model.needle.width, 16, 'strtod reads hex');
});
