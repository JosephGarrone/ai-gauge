/*
 * Client for the device's HTTP API (docs/gauge-xml-interface.md §7).
 *
 * The editor reaches a gauge in one of two ways (ADR 0008):
 *
 * - Served by the gauge itself, at http://<gauge>/editor/. Requests are same-origin, so the browser
 *   stands in the way of nothing. detectGauge() recognises this case, and the host is then ''.
 * - From the local dev server (http://localhost), to a gauge by address. The firmware allows CORS
 *   for loopback origins only.
 *
 * From the published GitHub Pages site it cannot work at all: that page is HTTPS and the gauge is
 * plain HTTP, so the browser blocks every request (mixed content). pageBlockReason() says so before
 * trying, and every other failure says which cause is likely, rather than a bare "failed to fetch".
 */

import { LIMITS } from './schema.js';

export class DeviceError extends Error {
    /** @param {'blocked'|'unreachable'|'http'|'bad-response'} kind */
    constructor(kind, message) {
        super(message);
        this.kind = kind;
    }
}

/**
 * "ai-gauge-1a2b.local", "http://192.168.1.5/", " 10.0.0.2:80 " -> "http://host[:port]".
 * '' means the gauge that served this page, and stays '' so requests are same-origin.
 */
export function deviceBaseUrl(host) {
    if (host === '') return '';
    const h = host.trim().replace(/^https?:\/\//i, '').replace(/\/.*$/, '');
    if (!/^[A-Za-z0-9.-]+(:\d+)?$/.test(h)) throw new DeviceError('unreachable', `'${host}' is not a host name or IP address.`);
    return `http://${h}`;
}

/** Where a gauge serves this editor. */
export function gaugeEditorUrl(host) {
    return `${deviceBaseUrl(host.trim())}/editor/`;
}

/** Why this page cannot reach any device, before trying; or null if it might. */
export function pageBlockReason(protocol = globalThis.location?.protocol) {
    if (protocol === 'https:') {
        return 'This page is served over HTTPS and the gauge only speaks plain HTTP, so the browser blocks every request to it (mixed content). ' +
            'Every gauge serves this editor itself: open it from the gauge to upload directly.';
    }
    return null;
}

/** The page origins the firmware allows cross-origin calls from (net_svc_http.c). */
export function isLoopback(hostname) {
    return hostname === 'localhost' || hostname === '127.0.0.1' || hostname === '[::1]';
}

/** Whether a /api/status reply came from a gauge, rather than from some other server. */
export function isGaugeStatus(s) {
    return typeof s?.version === 'string' && typeof s?.wifi?.hostname === 'string' && typeof s?.storage_mounted === 'boolean';
}

/**
 * Whether this page was served by a gauge: its own origin answers /api/status like one. Resolves to
 * that status, or null (GitHub Pages, the dev server, anything else).
 */
export async function detectGauge({ protocol = globalThis.location?.protocol, hostname = globalThis.location?.hostname, timeoutMs = 2000 } = {}) {
    // A loopback page is the dev server, never a gauge; asking would only log a 404.
    if (protocol !== 'http:' || isLoopback(hostname)) return null;
    try {
        const res = await fetch('/api/status', { cache: 'no-store', signal: AbortSignal.timeout(timeoutMs) });
        if (!res.ok) return null;
        const status = await res.json();
        return isGaugeStatus(status) ? status : null;
    } catch {
        return null;
    }
}

async function request(host, method, path, { body, as = 'json' } = {}) {
    const blocked = pageBlockReason();
    if (blocked) throw new DeviceError('blocked', blocked);

    const url = deviceBaseUrl(host) + path;
    let res;
    try {
        res = await fetch(url, {
            method,
            body,
            headers: body === undefined ? undefined : { 'Content-Type': 'text/plain' },
            cache: 'no-store',
        });
    } catch {
        throw new DeviceError('unreachable', host === ''
            ? `No response from this gauge to ${method} ${path}. It may have restarted or left the network.`
            : `No readable response from ${url}. Either the gauge is not reachable at that address, or the browser refused the response: the gauge only accepts calls from other pages when they are served from http://localhost. Opening the editor from the gauge itself avoids this.`);
    }

    const text = await res.text().catch(() => '');
    let json = null;
    try {
        json = JSON.parse(text);
    } catch {
        // Not JSON: handled below.
    }
    if (!res.ok) throw new DeviceError('http', json?.error ? `The gauge said: ${json.error}` : `HTTP ${res.status} from the gauge.`);
    if (as === 'text') return text;
    if (json === null) throw new DeviceError('bad-response', `The gauge's reply to ${method} ${path} was not JSON.`);
    return json;
}

export async function getStatus(host) {
    return request(host, 'GET', '/api/status');
}

export async function getSummary(host, id) {
    return request(host, 'GET', `/api/config/${encodeURIComponent(id)}`);
}

/**
 * Everything the Device section shows about a gauge's faces: which is on screen, the face limit,
 * and each face's summary. A face whose stored file no longer loads is listed with the gauge's
 * reason instead of a summary, so it can still be opened or deleted.
 */
export async function getFaceInventory(host) {
    const status = await getStatus(host);
    const list = await request(host, 'GET', '/api/gauges');
    if (!Array.isArray(list.gauges)) throw new DeviceError('bad-response', 'The gauge list was not in the expected form.');

    const faces = [];
    // One at a time: the gauge serves at most four connections, and each summary parses a file.
    for (const id of list.gauges) {
        try {
            faces.push({ id, summary: await getSummary(host, id) });
        } catch (err) {
            if (!(err instanceof DeviceError) || err.kind !== 'http') throw err;
            faces.push({ id, error: err.message.replace(/^The gauge said: /, '') });
        }
    }
    return {
        status,
        active: typeof status.active_gauge === 'string' ? status.active_gauge : null,
        max: Number.isInteger(list.max) ? list.max : LIMITS.deviceFaces,
        faces,
    };
}

/** What uploading a face with this id would do: 'update' a stored face, 'add' one, or nothing because the gauge is 'full'. */
export function uploadPlan(inventory, id) {
    if (inventory.faces.some((f) => f.id === id)) return 'update';
    return inventory.faces.length >= inventory.max ? 'full' : 'add';
}

/** The face's XML exactly as stored on the gauge. */
export async function getFaceXml(host, id) {
    return request(host, 'GET', `/api/config/${encodeURIComponent(id)}.xml`, { as: 'text' });
}

/**
 * Upload a face and read back what the device made of it. PUT does not report warnings, so the
 * summary is fetched straight after; `warnings` other than 0 means the device changed something.
 */
export async function uploadFace(host, id, xml) {
    await request(host, 'PUT', `/api/config/${encodeURIComponent(id)}`, { body: xml });
    return getSummary(host, id);
}

export async function deleteDeviceFace(host, id) {
    return request(host, 'DELETE', `/api/config/${encodeURIComponent(id)}`);
}
