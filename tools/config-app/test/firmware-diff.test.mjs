/*
 * The editor's parser port must agree with the firmware's parser, field by field and warning for
 * warning, or the preview and the "0 warnings" promise are guesses.
 *
 * Needs the C side built: tools/host-tests builds `gauge_config_dump`. Point GAUGE_CONFIG_DUMP at
 * it; without it this test is skipped (and says so). CI always sets it.
 */

import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';

import { parseGauge } from '../js/parse.js';
import { serializeGauge } from '../js/serialize.js';
import { fullCorpus } from './corpus.mjs';

const DUMP = process.env.GAUGE_CONFIG_DUMP;

/** Deep equality where numbers compare as float32, which is what both sides store. */
function diff(c, js, path = '') {
    if (typeof c === 'number' && typeof js === 'number') {
        return Math.fround(c) === Math.fround(js) ? null : `${path}: C ${c} vs JS ${js}`;
    }
    if (c === null || js === null || typeof c !== 'object' || typeof js !== 'object') {
        return c === js ? null : `${path}: C ${JSON.stringify(c)} vs JS ${JSON.stringify(js)}`;
    }
    if (Array.isArray(c) !== Array.isArray(js)) return `${path}: array vs object`;
    const keys = new Set([...Object.keys(c), ...Object.keys(js)]);
    for (const k of keys) {
        const d = diff(c[k], js[k], `${path}.${k}`);
        if (d) return d;
    }
    return null;
}

function runC(docs) {
    const dir = mkdtempSync(join(tmpdir(), 'gauge-diff-'));
    try {
        const files = docs.map((d, i) => {
            const f = join(dir, `${i}.xml`);
            writeFileSync(f, d.xml);
            return f;
        });
        const out = execFileSync(DUMP, files, { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
        return out.trimEnd().split('\n').map((line) => JSON.parse(line));
    } finally {
        rmSync(dir, { recursive: true, force: true });
    }
}

function compare(docs) {
    const results = runC(docs);
    assert.equal(results.length, docs.length, 'one result per document');
    const failures = [];
    docs.forEach((d, i) => {
        const c = results[i];
        const js = parseGauge(d.xml);
        let problem = null;
        if (c.ok !== js.ok) {
            problem = `C ok=${c.ok} (${c.error ?? ''}) vs JS ok=${js.ok} (${js.error ?? ''})`;
        } else if (!c.ok) {
            problem = c.error === js.error ? null : `error: C '${c.error}' vs JS '${js.error}'`;
        } else if (c.warnings !== js.warnings.length) {
            problem = `warnings: C ${c.warnings} vs JS ${js.warnings.length} [${js.warnings.join('; ')}]`;
        } else {
            problem = diff(c.model, js.model, 'model');
        }
        if (problem) failures.push(`${d.name}: ${problem}`);
    });
    assert.deepEqual(failures, [], `${failures.length} of ${docs.length} documents disagree`);
}

test('JS parser matches the firmware parser over the corpus', { skip: DUMP ? false : 'GAUGE_CONFIG_DUMP not set' }, () => {
    compare(fullCorpus());
});

test('files the editor writes parse identically on the firmware', { skip: DUMP ? false : 'GAUGE_CONFIG_DUMP not set' }, () => {
    // Round-trip every corpus model that parses through the serializer, and check the firmware reads
    // the editor's output exactly as the editor's own parser does, with no warnings.
    const docs = fullCorpus()
        .map((d) => ({ name: d.name, parsed: parseGauge(d.xml) }))
        .filter((d) => d.parsed.ok)
        .map((d) => ({ name: `${d.name} (re-serialized)`, xml: serializeGauge(d.parsed.model) }));
    compare(docs);
});
