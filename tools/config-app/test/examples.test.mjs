import assert from 'node:assert/strict';
import { readdirSync } from 'node:fs';
import { test } from 'node:test';

import { BLANK_XML, EXAMPLES } from '../js/examples.js';
import { parseGauge } from '../js/parse.js';
import { validateModel } from '../js/validate.js';
import { GAUGES_DIR } from './corpus.mjs';

test('the example list matches the faces the firmware ships', () => {
    const shipped = readdirSync(GAUGES_DIR).filter((f) => f.endsWith('.xml')).sort();
    assert.deepEqual(EXAMPLES.map((e) => e.file).sort(), shipped);
});

test('the blank face is valid and clean', () => {
    const r = parseGauge(BLANK_XML);
    assert.equal(r.ok, true);
    assert.equal(r.warnings.length, 0);
    const serious = validateModel(r.model).issues.filter((i) => i.severity !== 'info');
    assert.deepEqual(serious, []);
});
