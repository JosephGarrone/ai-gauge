/*
 * Client for the device's HTTP API (docs/gauge-xml-interface.md §7).
 *
 * This does NOT work from the published GitHub Pages site today, and no workaround is attempted.
 * The firmware sends no CORS headers and answers no OPTIONS preflight, and it serves plain HTTP,
 * which an https:// page may not call at all. Those are open firmware gaps (§8), recorded in
 * docs/networking.md. Every failure here says which of them is the likely cause, so the editor can
 * explain it rather than show a bare "failed to fetch".
 */

export class DeviceError extends Error {
    /** @param {'blocked'|'unreachable'|'http'|'bad-response'} kind */
    constructor(kind, message) {
        super(message);
        this.kind = kind;
    }
}

/** "ai-gauge-1a2b.local", "http://192.168.1.5/", " 10.0.0.2:80 " -> "http://host[:port]". */
export function deviceBaseUrl(host) {
    const h = host.trim().replace(/^https?:\/\//i, '').replace(/\/.*$/, '');
    if (!/^[A-Za-z0-9.-]+(:\d+)?$/.test(h)) throw new DeviceError('unreachable', `'${host}' is not a host name or IP address.`);
    return `http://${h}`;
}

/** Why this page cannot reach any device, before trying; or null if it might. */
export function pageBlockReason(protocol = globalThis.location?.protocol) {
    if (protocol === 'https:') {
        return 'This page is served over HTTPS and the gauge only speaks plain HTTP, so the browser blocks every request to it (mixed content). ' +
            'Direct upload needs a firmware change; until then, export the XML and upload it another way.';
    }
    return null;
}

async function request(host, method, path, body) {
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
        throw new DeviceError('unreachable',
            `No readable response from ${url}. Either the gauge is not reachable at that address, or the browser refused the response because the firmware sends no CORS headers (a ${method === 'GET' ? 'cross-origin read' : `${method} preflight`} is blocked).`);
    }

    let json = null;
    try {
        json = await res.json();
    } catch {
        // Fall through: handled below.
    }
    if (!res.ok) throw new DeviceError('http', json?.error ? `The gauge said: ${json.error}` : `HTTP ${res.status} from the gauge.`);
    if (json === null) throw new DeviceError('bad-response', `The gauge's reply to ${method} ${path} was not JSON.`);
    return json;
}

export async function getStatus(host) {
    return request(host, 'GET', '/api/status');
}

export async function listDeviceFaces(host) {
    const j = await request(host, 'GET', '/api/gauges');
    if (!Array.isArray(j.gauges)) throw new DeviceError('bad-response', 'The gauge list was not in the expected form.');
    return j.gauges;
}

export async function getSummary(host, id) {
    return request(host, 'GET', `/api/config/${encodeURIComponent(id)}`);
}

/**
 * Upload a face and read back what the device made of it. PUT does not report warnings, so the
 * summary is fetched straight after; `warnings` other than 0 means the device changed something.
 */
export async function uploadFace(host, id, xml) {
    await request(host, 'PUT', `/api/config/${encodeURIComponent(id)}`, xml);
    return getSummary(host, id);
}

export async function deleteDeviceFace(host, id) {
    return request(host, 'DELETE', `/api/config/${encodeURIComponent(id)}`);
}
