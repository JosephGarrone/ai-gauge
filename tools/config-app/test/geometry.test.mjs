import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
    builtinTickPolygon, flattenPath, labelRadius, polygonSelfIntersects, simplifyToBudget, stepCount,
    tickRadius, valueToDeg,
} from '../js/geometry.js';
import { parseGauge } from '../js/parse.js';
import { shippedFaces } from './corpus.mjs';

const face = (name) => parseGauge(shippedFaces().find((f) => f.name === name).xml).model;

test('value to angle', () => {
    const m = face('boost.xml');
    assert.equal(valueToDeg(m, 0), 225);
    assert.equal(valueToDeg(m, 15), 360);
    assert.equal(valueToDeg(m, 30), 495);
    assert.equal(valueToDeg(m, 99), 495, 'clamped to max');
});

test('tick circle and label radius', () => {
    const boost = face('boost.xml');
    assert.equal(tickRadius(boost), 225 - 18 - 6);

    const custom = face('boost_custom.xml');
    assert.equal(tickRadius(custom), 225 - 10 - 6);
    custom.labels.radius = 0;
    assert.equal(labelRadius(custom), 209 - 28 - 27, 'inside the shaped tick by one line height');

    boost.labels.radius = 0;
    assert.equal(labelRadius(boost), 201 - 24 - 27);
});

test('step counts use float32 like the firmware', () => {
    const m = parseGauge('<gauge version="1" id="x"><source channel="c" min="0" max="1"/></gauge>').model;
    assert.equal(stepCount(m, Math.fround(0.1)), 10);
});

test('self-intersection', () => {
    assert.equal(polygonSelfIntersects([[0, 0], [10, 0], [10, 10], [0, 10]]), false);
    assert.equal(polygonSelfIntersects([[0, 0], [10, 10], [10, 0], [0, 10]]), true, 'bow tie');
    assert.equal(polygonSelfIntersects([[0, 0], [10, 0], [5, 0], [5, 5]]), true, 'folds back on itself');
    assert.equal(polygonSelfIntersects([[0, -176], [3, -150], [7, -20], [9, 0], [6, 38], [-6, 38], [-9, 0], [-7, -20], [-3, -150]]), false);
    assert.equal(polygonSelfIntersects([[0, 0], [0, 0], [10, 0], [10, 10]]), false, 'repeated vertex');
});

test('SVG paths flatten to rings', () => {
    assert.deepEqual(flattenPath('M0 0 L10 0 L10 10 Z'), [[[0, 0], [10, 0], [10, 10]]]);
    assert.deepEqual(flattenPath('m0,0 h10 v10 h-10 z'), [[[0, 0], [10, 0], [10, 10], [0, 10]]]);

    const [arc] = flattenPath('M -10 0 A 10 10 0 0 1 10 0 Z', 0.1);
    for (const [x, y] of arc) {
        assert.ok(Math.abs(Math.hypot(x, y) - 10) < 0.11, 'on the circle');
        assert.ok(y <= 1e-9, 'clockwise from the left goes over the top');
    }

    const [curve] = flattenPath('M0 0 C 0 -50 20 -50 20 0 Z', 0.1);
    assert.ok(curve.length > 8);
    assert.ok(curve.every(([, y]) => y <= 0 && y >= -37.6));

    assert.equal(flattenPath('M0 0 L 5 5 M 10 10 L 20 10 L 20 20').length, 1, 'two-point subpaths are dropped');
    assert.throws(() => flattenPath('M0 0 X 5'));
    assert.throws(() => flattenPath('0 0 L 5 5'));
});

test('simplification fits the vertex budget', () => {
    const circle = Array.from({ length: 200 }, (_, i) => [50 * Math.cos((i / 200) * 2 * Math.PI), 50 * Math.sin((i / 200) * 2 * Math.PI)]);
    const { rings, tolerance } = simplifyToBudget([circle], 40);
    assert.ok(rings[0].length <= 40);
    assert.ok(tolerance < 1.5, `tolerance ${tolerance}`);
});

test('built-in tick equivalent', () => {
    assert.deepEqual(builtinTickPolygon(20, 4), [[-2, 0], [2, 0], [2, 20], [-2, 20]]);
});
