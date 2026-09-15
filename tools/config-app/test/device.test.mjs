import assert from 'node:assert/strict';
import { test } from 'node:test';

import {
    DeviceError, detectGauge, deviceBaseUrl, gaugeEditorUrl, getFaceInventory, isGaugeStatus, isLoopback, pageBlockReason,
    uploadPlan,
} from '../js/device.js';
import { LIMITS } from '../js/schema.js';

async function withFakeGauge(routes, fn) {
    const realFetch = globalThis.fetch;
    globalThis.fetch = async (url) => (url in routes
        ? new Response(JSON.stringify(routes[url]), { status: 200 })
        : new Response(JSON.stringify({ error: 'unexpected end of document' }), { status: 404 }));
    try {
        return await fn();
    } finally {
        globalThis.fetch = realFetch;
    }
}

test('reading the faces on a gauge, and what an upload would do', async () => {
    const status = { version: '1', wifi: { hostname: 'ai-gauge-91e8' }, storage_mounted: true, active_gauge: 'boost' };
    const inv = await withFakeGauge({
        '/api/status': status,
        '/api/gauges': { gauges: ['boost', 'broken'], max: 2 },
        '/api/config/boost': { id: 'boost', channel: 'boost', unit: 'psi', min: 0, max: 30, warnings: 0 },
    }, () => getFaceInventory(''));

    assert.equal(inv.active, 'boost');
    assert.equal(inv.max, 2);
    assert.deepEqual(inv.faces[0].summary.unit, 'psi');
    assert.equal(inv.faces[1].error, 'unexpected end of document', 'a face the gauge cannot load is still listed');

    assert.equal(uploadPlan(inv, 'boost'), 'update');
    assert.equal(uploadPlan(inv, 'egt'), 'full');
    assert.equal(uploadPlan({ ...inv, max: 3 }, 'egt'), 'add');

    const older = await withFakeGauge({ '/api/status': { ...status, active_gauge: undefined }, '/api/gauges': { gauges: [] } },
        () => getFaceInventory(''));
    assert.equal(older.max, LIMITS.deviceFaces, 'firmware that does not report a limit gets the known one');
    assert.equal(older.active, null);
});

test('pages that cannot have been served by a gauge do not ask', async () => {
    const fetched = [];
    const realFetch = globalThis.fetch;
    globalThis.fetch = async (url) => { fetched.push(url); throw new Error('unexpected'); };
    try {
        assert.equal(await detectGauge({ protocol: 'https:', hostname: 'user.github.io' }), null);
        assert.equal(await detectGauge({ protocol: 'http:', hostname: 'localhost' }), null);
        assert.deepEqual(fetched, []);
        assert.equal(await detectGauge({ protocol: 'http:', hostname: 'ai-gauge-91e8.local' }), null, 'a failed request is not a gauge');
        assert.deepEqual(fetched, ['/api/status']);
    } finally {
        globalThis.fetch = realFetch;
    }
    assert.equal(isLoopback('127.0.0.1'), true);
    assert.equal(isLoopback('192.168.1.5'), false);
});

test('addresses', () => {
    assert.equal(deviceBaseUrl('ai-gauge-1a2b.local'), 'http://ai-gauge-1a2b.local');
    assert.equal(deviceBaseUrl(' http://192.168.1.5/editor/ '), 'http://192.168.1.5');
    assert.equal(deviceBaseUrl('10.0.0.2:8080'), 'http://10.0.0.2:8080');
    assert.equal(deviceBaseUrl(''), '', 'the gauge that served the page stays same-origin');
    assert.throws(() => deviceBaseUrl('not a host'), DeviceError);
    assert.equal(gaugeEditorUrl('ai-gauge-1a2b.local '), 'http://ai-gauge-1a2b.local/editor/');
});

test('only an https page is blocked before trying', () => {
    assert.match(pageBlockReason('https:'), /mixed content/);
    assert.equal(pageBlockReason('http:'), null);
});

test('recognising a gauge from its status reply', () => {
    const status = {
        version: '0.0.0-dev+e260462', idf: 'v5.5.1', uptime_s: 12,
        wifi: { state: 'connected', ssid: 'home', ip: '192.168.1.5', rssi: -50, hostname: 'ai-gauge-91e8' },
        heap: { internal: 8000, internal_min: 2000, psram: 7000000 }, httpd_stack_min: 2500, storage_mounted: true,
    };
    assert.equal(isGaugeStatus(status), true);
    assert.equal(isGaugeStatus({ version: '1' }), false);
    assert.equal(isGaugeStatus(null), false);
});
