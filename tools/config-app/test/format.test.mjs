/* printf port, and the serializer's round trip through the parser. */

import assert from 'node:assert/strict';
import { test } from 'node:test';

import { parseGauge } from '../js/parse.js';
import { checkFormat, formatNumber } from '../js/printf.js';
import { formatNum, serializeGauge } from '../js/serialize.js';
import { validateModel } from '../js/validate.js';
import { fullCorpus, shippedFaces } from './corpus.mjs';

test('formatNumber matches C printf', () => {
    const f = Math.fround;
    const cases = [
        ['%.1f', f(22.45), '22.5'],
        ['%.1f', 0.25, '0.2'],      // exact tie: to even
        ['%.1f', 0.35, '0.3'],      // 0.35 is just below the tie in binary
        ['%.0f', 2.5, '2'],
        ['%.0f', 3.5, '4'],
        ['%.2f', 0.125, '0.12'],
        ['%.2f', 0.375, '0.38'],
        ['%g', 0, '0'],
        ['%g', 30, '30'],
        ['%g', 100000, '100000'],
        ['%g', 1e6, '1e+06'],
        ['%g', 0.0001, '0.0001'],
        ['%g', 0.00001, '1e-05'],
        ['%g', f(0.1) * 3, '0.3'],
        ['%.3g', 1234.5, '1.23e+03'],
        ['%G', 1e-10, '1E-10'],
        ['%#g', 1, '1.00000'],
        ['%e', 12345.678, '1.234568e+04'],
        ['%E', 12345.678, '1.234568E+04'],
        ['%5.1f', 3.14159, '  3.1'],
        ['%-6.1f|', 3.14159, '3.1   |'],
        ['%+.1f', 3.14159, '+3.1'],
        ['% .1f', 3.14159, ' 3.1'],
        ['%05.1f', -3.14159, '-03.1'],
        ['%#.0f', 3, '3.'],
        ['%.1f%%', 50, '50.0%'],
        ['%.0f psi', 12.7, '13 psi'],
        ['%.1f', -0.04, '-0.0'],
        ['%f', 1.5, '1.500000'],
    ];
    for (const [fmt, v, want] of cases) {
        assert.equal(formatNumber(fmt, v), want, `printf("${fmt}", ${v})`);
    }
});

test('checkFormat allows exactly one float conversion', () => {
    for (const good of ['%.1f', '%g', '%e', '%5.2f psi', '%.0f%%', 'x%-+ #08.3Gy']) {
        assert.equal(checkFormat(good), null, good);
    }
    for (const bad of ['%d', '%s', '%.1f %.1f', 'abc', '%%', '%lf', '%', '%.1', '%*f']) {
        assert.notEqual(checkFormat(bad), null, bad);
    }
});

test('numbers are written as plain decimals with at most 4 places', () => {
    assert.equal(formatNum(5), '5');
    assert.equal(formatNum(-0), '0');
    assert.equal(formatNum(0.15000000596), '0.15');
    assert.equal(formatNum(0.0625), '0.0625');
    assert.equal(formatNum(-1.23456), '-1.2346');
    assert.equal(formatNum(-0.00001), '0');
    assert.equal(formatNum(100.5), '100.5');
});

test('shipped faces survive a round trip unchanged', () => {
    for (const { name, xml } of shippedFaces()) {
        const first = parseGauge(xml);
        assert.equal(first.ok, true, name);
        assert.equal(first.warnings.length, 0, `${name} parses cleanly`);
        const again = parseGauge(serializeGauge(first.model));
        assert.equal(again.ok, true, name);
        assert.equal(again.warnings.length, 0, `${name} re-serialized parses cleanly`);
        assert.deepEqual(again.model, first.model, name);
    }
});

test('source is written before face, and shapes as child elements', () => {
    const { model } = parseGauge(shippedFaces().find((f) => f.name === 'boost_custom.xml').xml);
    const xml = serializeGauge(model);
    assert.ok(xml.indexOf('<source') < xml.indexOf('<face'));
    assert.match(xml, /<ticks major-every="5" minor-every="1" color="#ffffff">\n\s+<major-shape>/);
    assert.match(xml, /<circle cy="5" r="2.5"\/>/);
    assert.doesNotMatch(xml, /<needle[^>]*style=/, 'style is irrelevant with a needle shape');
});

/** Numbers may move by the 4-decimal rounding plus float32 spacing; everything else must be equal. */
function assertClose(a, b, path) {
    if (typeof a === 'number') {
        assert.ok(Math.abs(a - b) <= 1e-4 + Math.abs(a) * 2.4e-7, `${path}: ${a} vs ${b}`);
    } else if (a !== null && typeof a === 'object') {
        assert.deepEqual(Object.keys(a).sort(), Object.keys(b ?? {}).sort(), path);
        for (const k of Object.keys(a)) assertClose(a[k], b[k], `${path}.${k}`);
    } else {
        assert.equal(a, b, path);
    }
}

test('everything the parser accepts is written back faithfully, or the validator says why not', () => {
    for (const { name, xml } of fullCorpus()) {
        const first = parseGauge(xml);
        if (!first.ok) continue;
        const out = serializeGauge(first.model);
        const again = parseGauge(out);
        assert.equal(again.ok, true, name);

        // Some device readings cannot be written back: a band read before <source> was clamped to the
        // default range, not the real one. The editor must never write such a file silently.
        if (again.warnings.length > 0) {
            const explained = validateModel(first.model).issues.filter((i) => i.severity === 'error' && i.section !== 'file');
            assert.ok(explained.length > 0, `${name}: device warnings with no validator error: ${again.warnings.join('; ')}`);
            continue;
        }

        // Ignored attributes are not written: len/width under a tick shape, and style etc. under a
        // needle shape. Compare what the device actually uses.
        const used = (m) => {
            const c = structuredClone(m);
            if (c.ticks?.majorShape) { delete c.ticks.majorLen; delete c.ticks.majorWidth; }
            if (c.ticks?.minorShape) { delete c.ticks.minorLen; delete c.ticks.minorWidth; }
            if (c.needle.shape) { delete c.needle.style; delete c.needle.length; delete c.needle.width; delete c.needle.tail; }
            if (c.needle.hub) delete c.needle.pivotRadius;
            for (const t of c.titles) t.text = t.text.includes('"') && t.text.includes("'") ? t.text.replaceAll('"', '') : t.text;
            return c;
        };
        assertClose(used(again.model), used(first.model), name);
    }
});
