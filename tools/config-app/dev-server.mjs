/*
 * Local preview of the GitHub Pages site, with no dependencies: `node dev-server.mjs`, then open
 * http://localhost:8080/editor/.
 *
 * Mirrors the layout .github/workflows/pages.yml publishes:
 *   /                  the flashing page (tools/web-installer), without a firmware binary
 *   /editor/           this app
 *   /editor/examples/  firmware/assets/gauges
 *
 * ES modules do not load from file:// URLs, which is why a server is needed at all.
 */

import { createReadStream, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { dirname, extname, join, normalize, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const port = Number(process.env.PORT) || 8080;

const MOUNTS = [
    ['/editor/examples/', join(repo, 'firmware', 'assets', 'gauges')],
    ['/editor/', here],
    ['/', join(repo, 'tools', 'web-installer')],
];

const TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.xml': 'application/xml; charset=utf-8',
    '.svg': 'image/svg+xml',
};

function resolve(urlPath) {
    for (const [prefix, dir] of MOUNTS) {
        if (!urlPath.startsWith(prefix)) continue;
        const rel = normalize(decodeURIComponent(urlPath.slice(prefix.length)) || 'index.html');
        const file = join(dir, rel);
        if (!file.startsWith(dir + sep) && file !== dir) return null; // no escaping the mount
        return file;
    }
    return null;
}

createServer((req, res) => {
    const urlPath = new URL(req.url, 'http://localhost').pathname;
    if (urlPath === '/editor') {
        res.writeHead(301, { Location: '/editor/' }).end();
        return;
    }
    let file = resolve(urlPath);
    try {
        if (file && statSync(file).isDirectory()) file = join(file, 'index.html');
        if (!file || !statSync(file).isFile()) throw new Error('not found');
    } catch {
        res.writeHead(404, { 'Content-Type': 'text/plain' }).end('not found');
        return;
    }
    res.writeHead(200, { 'Content-Type': TYPES[extname(file)] ?? 'application/octet-stream', 'Cache-Control': 'no-store' });
    createReadStream(file).pipe(res);
}).listen(port, () => {
    console.log(`Face editor: http://localhost:${port}/editor/`);
});
