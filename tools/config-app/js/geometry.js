/*
 * Dial geometry, following gauge_render.c and gauge_shape.c (docs/gauge-xml-interface.md §5).
 *
 * Counts and tick values are computed in float32 like the firmware, because a step such as 0.1
 * lands a tick on either side of `max` depending on it. Positions use doubles: sub-pixel
 * differences are below what the preview can show.
 */

import { LIMITS, PANEL, lineHeight } from './schema.js';

const f32 = Math.fround;
const DEG = Math.PI / 180;

/** lroundf(): half away from zero. */
export function lround(v) {
    return Math.sign(v) * Math.round(Math.abs(v));
}

export function scaleRadius(m) {
    return m.face.radius > 0 ? m.face.radius : PANEL.defaultRadius;
}

/** value_to_deg(): degrees clockwise from 12 o'clock, clamped to the scale. */
export function valueToDeg(m, value) {
    const span = f32(m.source.max - m.source.min);
    let frac = span > 0 ? (value - m.source.min) / span : 0;
    frac = Math.min(1, Math.max(0, frac));
    return m.face.startAngle + frac * m.face.sweep;
}

/** Offset from the dial centre of the point `r` px out at schema angle `deg`. */
export function polar(r, deg) {
    return [r * Math.sin(deg * DEG), -r * Math.cos(deg * DEG)];
}

export function widestBand(m) {
    return m.bands.reduce((w, b) => Math.max(w, b.width), 0);
}

/** tick_outer: the circle ticks hang from, inside the band ring. */
export function tickRadius(m) {
    const widest = widestBand(m);
    return scaleRadius(m) - widest - (widest ? 6 : 0);
}

/** `(int32_t)((max - min) / step)` in float32. */
export function stepCount(m, step) {
    return Math.trunc(f32(f32(m.source.max - m.source.min) / f32(step)));
}

/** `min + (float)i * step` in float32. */
export function stepValue(m, i, step) {
    return f32(m.source.min + f32(i * f32(step)));
}

/** The values a tick or label pass draws at, or [] when it is off or skipped for being too dense. */
export function stepValues(m, step, cap) {
    if (!(step > 0)) return [];
    const count = stepCount(m, step);
    if (count > cap) return [];
    return Array.from({ length: count + 1 }, (_, i) => stepValue(m, i, step));
}

export function labelStep(m) {
    if (!m.labels) return 0;
    return m.labels.every > 0 ? m.labels.every : (m.ticks?.majorEvery ?? 10);
}

/** Radius label centres sit on: explicit, or inside the major ticks by one line height. */
export function labelRadius(m) {
    if (m.labels.radius > 0) return m.labels.radius;
    const ticks = m.ticks;
    const reach = ticks?.majorShape ? Math.ceil(shapeMaxY(ticks.majorShape)) : (ticks?.majorLen ?? 20);
    return tickRadius(m) - reach - lineHeight(m.labels.font);
}

export function needleLength(m) {
    return m.needle.length > 0 ? m.needle.length : Math.trunc(f32(scaleRadius(m) * f32(0.72)));
}

/* -------------------------------------------------------------------- shapes -------- */

/** Round to the firmware's 1/16 px storage. */
export function quantize(v) {
    return lround(v * LIMITS.shapeScale) / LIMITS.shapeScale;
}

/** gauge_shape_max_y(): how far a tick shape reaches in towards the centre. */
export function shapeMaxY(shape) {
    let maxY = null;
    for (const p of shape.parts) {
        const ys = p.kind === 'circle' ? [p.cy + p.r] : p.points.map((pt) => pt[1]);
        for (const y of ys) maxY = maxY === null ? y : Math.max(maxY, y);
    }
    return maxY ?? 0;
}

/** gauge_shape_extent(): furthest distance from the origin to anything the shape covers. */
export function shapeExtent(shape) {
    let ext = 0;
    for (const p of shape.parts) {
        if (p.kind === 'circle') ext = Math.max(ext, Math.hypot(p.cx, p.cy) + p.r);
        else for (const [x, y] of p.points) ext = Math.max(ext, Math.hypot(x, y));
    }
    return ext;
}

/** Axis-aligned bounds of a shape in its own (unrotated) frame. */
export function shapeBounds(shape) {
    let x1 = Infinity, y1 = Infinity, x2 = -Infinity, y2 = -Infinity;
    for (const p of shape.parts) {
        if (p.kind === 'circle') {
            x1 = Math.min(x1, p.cx - p.r); x2 = Math.max(x2, p.cx + p.r);
            y1 = Math.min(y1, p.cy - p.r); y2 = Math.max(y2, p.cy + p.r);
        } else {
            for (const [x, y] of p.points) {
                x1 = Math.min(x1, x); x2 = Math.max(x2, x);
                y1 = Math.min(y1, y); y2 = Math.max(y2, y);
            }
        }
    }
    return x1 === Infinity ? null : { x1, y1, x2, y2 };
}

export function shapeVertexCount(shape) {
    return shape.parts.reduce((n, p) => n + (p.kind === 'polygon' ? p.points.length : 0), 0);
}

/** Signed area in px², shoelace. */
export function polygonArea(points) {
    let a = 0;
    for (let i = 0; i < points.length; i++) {
        const [x1, y1] = points[i];
        const [x2, y2] = points[(i + 1) % points.length];
        a += x1 * y2 - x2 * y1;
    }
    return a / 2;
}

function orient(a, b, c) {
    const v = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    return Math.abs(v) < 1e-9 ? 0 : Math.sign(v);
}

function onSegment(a, b, p) {
    return Math.min(a[0], b[0]) <= p[0] && p[0] <= Math.max(a[0], b[0]) &&
        Math.min(a[1], b[1]) <= p[1] && p[1] <= Math.max(a[1], b[1]);
}

function segmentsIntersect(a, b, c, d) {
    const o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
    if (o1 !== o2 && o3 !== o4) return true;
    return (o1 === 0 && onSegment(a, b, c)) || (o2 === 0 && onSegment(a, b, d)) ||
        (o3 === 0 && onSegment(c, d, a)) || (o4 === 0 && onSegment(c, d, b));
}

const samePoint = (a, b) => a[0] === b[0] && a[1] === b[1];

/**
 * True if any two non-adjacent edges touch, or adjacent edges fold back over each other.
 * The firmware fills overlaps instead of cutting them out, unlike SVG's even-odd rule, so the
 * editor refuses self-intersection rather than preview it differently. Zero-length edges from
 * repeated vertices are ignored: they fill nothing either way.
 */
export function polygonSelfIntersects(points) {
    const pts = points.filter((p, i) => !samePoint(p, points[(i + 1) % points.length]));
    const n = pts.length;
    if (n < 4) return n === 3 && orient(pts[0], pts[1], pts[2]) === 0 && false;
    for (let i = 0; i < n; i++) {
        const a = pts[i], b = pts[(i + 1) % n];
        for (let j = i + 1; j < n; j++) {
            const c = pts[j], d = pts[(j + 1) % n];
            const adjacent = j === i + 1 || (i === 0 && j === n - 1);
            if (adjacent) {
                // Adjacent edges share a vertex; they only intersect if they overlap collinearly.
                const shared = j === i + 1 ? b : a;
                const other1 = j === i + 1 ? a : b;
                const other2 = j === i + 1 ? d : c;
                if (orient(other1, shared, other2) === 0 &&
                    Math.sign(other1[0] - shared[0]) === Math.sign(other2[0] - shared[0]) &&
                    Math.sign(other1[1] - shared[1]) === Math.sign(other2[1] - shared[1])) {
                    return true;
                }
                continue;
            }
            if (segmentsIntersect(a, b, c, d)) return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ simplification -------- */

function perpDistance(p, a, b) {
    const dx = b[0] - a[0], dy = b[1] - a[1];
    const len = Math.hypot(dx, dy);
    if (len === 0) return Math.hypot(p[0] - a[0], p[1] - a[1]);
    return Math.abs(dy * p[0] - dx * p[1] + b[0] * a[1] - b[1] * a[0]) / len;
}

function rdpOpen(points, eps) {
    if (points.length < 3) return points.slice();
    let maxD = 0, idx = 0;
    const a = points[0], b = points[points.length - 1];
    for (let i = 1; i < points.length - 1; i++) {
        const d = perpDistance(points[i], a, b);
        if (d > maxD) { maxD = d; idx = i; }
    }
    if (maxD <= eps) return [a, b];
    const left = rdpOpen(points.slice(0, idx + 1), eps);
    const right = rdpOpen(points.slice(idx), eps);
    return left.slice(0, -1).concat(right);
}

/** Ramer–Douglas–Peucker on a closed ring, split at the vertex furthest from the first. */
export function simplifyRing(points, eps) {
    if (points.length <= 3) return points.slice();
    let far = 0, farD = -1;
    for (let i = 1; i < points.length; i++) {
        const d = Math.hypot(points[i][0] - points[0][0], points[i][1] - points[0][1]);
        if (d > farD) { farD = d; far = i; }
    }
    const first = rdpOpen(points.slice(0, far + 1), eps);
    const second = rdpOpen(points.slice(far).concat([points[0]]), eps);
    return first.slice(0, -1).concat(second.slice(0, -1));
}

/**
 * Simplify rings until their total vertex count fits `budget`, loosening the tolerance only as
 * far as needed. Returns the rings and the tolerance used.
 */
export function simplifyToBudget(rings, budget, minEps = 0.1) {
    const total = (rs) => rs.reduce((n, r) => n + r.length, 0);
    let eps = minEps;
    let out = rings.map((r) => simplifyRing(r, eps));
    while (total(out) > budget && eps < 1000) {
        eps *= 1.25;
        out = rings.map((r) => simplifyRing(r, eps));
    }
    return { rings: out, tolerance: eps };
}

/* ------------------------------------------------------------ SVG path flatten ------ */

const PATH_TOKEN = /([MmLlHhVvCcSsQqTtAaZz])|([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)/g;

function flattenCubic(out, p0, p1, p2, p3, tol, depth = 0) {
    // Flat enough when both control points lie within tolerance of the chord.
    const flat = perpDistance(p1, p0, p3) <= tol && perpDistance(p2, p0, p3) <= tol;
    if (flat || depth > 16) {
        out.push(p3);
        return;
    }
    const mid = (a, b) => [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
    const p01 = mid(p0, p1), p12 = mid(p1, p2), p23 = mid(p2, p3);
    const p012 = mid(p01, p12), p123 = mid(p12, p23), m = mid(p012, p123);
    flattenCubic(out, p0, p01, p012, m, tol, depth + 1);
    flattenCubic(out, m, p123, p23, p3, tol, depth + 1);
}

function flattenArc(out, p0, rx, ry, phiDeg, largeArc, sweep, p1, tol) {
    // SVG implementation notes, F.6.5: endpoint to centre parameterisation.
    if (p0[0] === p1[0] && p0[1] === p1[1]) return;
    rx = Math.abs(rx); ry = Math.abs(ry);
    if (rx === 0 || ry === 0) { out.push(p1); return; }
    const phi = phiDeg * DEG, cos = Math.cos(phi), sin = Math.sin(phi);
    const dx = (p0[0] - p1[0]) / 2, dy = (p0[1] - p1[1]) / 2;
    const x1 = cos * dx + sin * dy, y1 = -sin * dx + cos * dy;
    const lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) { rx *= Math.sqrt(lambda); ry *= Math.sqrt(lambda); }
    const num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    const den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    let coef = Math.sqrt(Math.max(0, num / den));
    if (largeArc === sweep) coef = -coef;
    const cxp = coef * (rx * y1) / ry, cyp = coef * -(ry * x1) / rx;
    const cx = cos * cxp - sin * cyp + (p0[0] + p1[0]) / 2;
    const cy = sin * cxp + cos * cyp + (p0[1] + p1[1]) / 2;
    const ang = (ux, uy, vx, vy) => {
        const a = Math.atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    const t1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
    let dt = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
    if (!sweep && dt > 0) dt -= 2 * Math.PI;
    if (sweep && dt < 0) dt += 2 * Math.PI;
    const r = Math.max(rx, ry);
    const step = tol >= r ? Math.PI / 2 : 2 * Math.acos(1 - tol / r);
    const n = Math.max(1, Math.ceil(Math.abs(dt) / step));
    for (let i = 1; i <= n; i++) {
        const t = t1 + (dt * i) / n;
        const ex = rx * Math.cos(t), ey = ry * Math.sin(t);
        out.push(i === n ? p1 : [cos * ex - sin * ey + cx, sin * ex + cos * ey + cy]);
    }
}

/**
 * Flatten an SVG path `d` into closed rings of straight edges, to within `tol` px.
 * Every subpath becomes a ring, open or not: the firmware has no strokes, only fills.
 * @throws {Error} on a malformed path.
 */
export function flattenPath(d, tol = 0.1) {
    const tokens = [];
    for (const m of d.matchAll(PATH_TOKEN)) tokens.push(m[1] ?? Number(m[2]));
    const leftovers = d.replace(PATH_TOKEN, '').replace(/[\s,]/g, '');
    if (leftovers.length > 0) throw new Error(`unexpected '${leftovers[0]}' in path`);

    const rings = [];
    let ring = null;
    let cur = [0, 0], start = [0, 0];
    let lastCtrl = null, lastCmd = '';
    let i = 0, cmd = null;

    const num = () => {
        if (typeof tokens[i] !== 'number') throw new Error(`path command ${cmd} is missing a number`);
        return tokens[i++];
    };
    const flag = () => {
        const v = num();
        if (v !== 0 && v !== 1) throw new Error('arc flags must be 0 or 1');
        return v === 1;
    };
    const finishRing = () => {
        if (ring && ring.length >= 3) rings.push(ring);
        ring = null;
    };

    while (i < tokens.length) {
        if (typeof tokens[i] === 'string') cmd = tokens[i++];
        else if (cmd === null) throw new Error('path must start with a command');
        else if (cmd === 'M') cmd = 'L';
        else if (cmd === 'm') cmd = 'l';

        const rel = cmd === cmd.toLowerCase() && cmd !== 'z';
        const C = cmd.toUpperCase();
        const abs = (x, y) => (rel ? [cur[0] + x, cur[1] + y] : [x, y]);

        if (C === 'Z') {
            cur = start;
            finishRing();
            lastCtrl = null;
            lastCmd = 'Z';
            if (typeof tokens[i] === 'number') throw new Error('numbers after Z');
            continue;
        }
        if (C === 'M') {
            finishRing();
            cur = abs(num(), num());
            start = cur;
            ring = [cur];
            lastCtrl = null;
        } else {
            if (ring === null) ring = [cur];
            if (C === 'L') {
                cur = abs(num(), num());
                ring.push(cur);
                lastCtrl = null;
            } else if (C === 'H') {
                cur = [rel ? cur[0] + num() : num(), cur[1]];
                ring.push(cur);
                lastCtrl = null;
            } else if (C === 'V') {
                cur = [cur[0], rel ? cur[1] + num() : num()];
                ring.push(cur);
                lastCtrl = null;
            } else if (C === 'C' || C === 'S') {
                let c1;
                if (C === 'C') c1 = abs(num(), num());
                else c1 = lastCtrl && /[CS]/.test(lastCmd) ? [2 * cur[0] - lastCtrl[0], 2 * cur[1] - lastCtrl[1]] : cur;
                const c2 = abs(num(), num());
                const end = abs(num(), num());
                flattenCubic(ring, cur, c1, c2, end, tol);
                lastCtrl = c2;
                cur = end;
            } else if (C === 'Q' || C === 'T') {
                let q;
                if (C === 'Q') q = abs(num(), num());
                else q = lastCtrl && /[QT]/.test(lastCmd) ? [2 * cur[0] - lastCtrl[0], 2 * cur[1] - lastCtrl[1]] : cur;
                const end = abs(num(), num());
                const c1 = [cur[0] + (2 / 3) * (q[0] - cur[0]), cur[1] + (2 / 3) * (q[1] - cur[1])];
                const c2 = [end[0] + (2 / 3) * (q[0] - end[0]), end[1] + (2 / 3) * (q[1] - end[1])];
                flattenCubic(ring, cur, c1, c2, end, tol);
                lastCtrl = q;
                cur = end;
            } else if (C === 'A') {
                const rx = num(), ry = num(), rot = num(), large = flag(), sweep = flag();
                const end = abs(num(), num());
                flattenArc(ring, cur, rx, ry, rot, large, sweep, end, tol);
                cur = end;
                lastCtrl = null;
            } else {
                throw new Error(`unsupported path command ${cmd}`);
            }
        }
        lastCmd = C;
    }
    finishRing();

    // Drop a closing vertex that repeats the first; the firmware closes rings implicitly.
    return rings.map((r) => {
        const out = r.filter((p, k) => k === 0 || !samePoint(p, r[k - 1]));
        if (out.length > 1 && Math.hypot(out[0][0] - out.at(-1)[0], out[0][1] - out.at(-1)[1]) < 1e-6) out.pop();
        return out;
    }).filter((r) => r.length >= 3);
}

/* ------------------------------------------------------------ built-in shapes ------- */

/** The rectangle a built-in tick of this length and width draws, as a shape polygon (§4.1). */
export function builtinTickPolygon(len, width) {
    const hw = width / 2;
    return [[-hw, 0], [hw, 0], [hw, len], [-hw, len]];
}

/**
 * The built-in needle for the current style, as shape parts pointing at 12 o'clock. Round caps
 * become circles, so a user can start from it and edit.
 */
export function builtinNeedleParts(m) {
    const n = m.needle;
    const L = needleLength(m);
    const hw = Math.max(1, n.width / 2);
    const t = n.tail;
    const q = quantize;
    const parts = [];
    const capsule = (y1, y2, w) => {
        const h = Math.max(0.5, w / 2);
        parts.push({ kind: 'polygon', points: [[-h, y1], [h, y1], [h, y2], [-h, y2]].map(([x, y]) => [q(x), q(y)]), color: null });
        parts.push({ kind: 'circle', cx: 0, cy: q(y1), r: q(h), color: null });
        parts.push({ kind: 'circle', cx: 0, cy: q(y2), r: q(h), color: null });
    };

    if (n.style === 'line') {
        capsule(-L, t, n.width);
    } else if (n.style === 'arrow') {
        const head = L * 0.25;
        capsule(-(L - head), t, Math.max(1, Math.trunc(n.width / 2)));
        parts.push({ kind: 'polygon', points: [[0, -L], [hw * 2, -(L - head)], [-hw * 2, -(L - head)]].map(([x, y]) => [q(x), q(y)]), color: null });
    } else {
        parts.push({ kind: 'polygon', points: [[0, -L], [hw, 0], [-hw, 0]].map(([x, y]) => [q(x), q(y)]), color: null });
        if (t > 0) capsule(0, t, n.width);
    }
    return parts.filter((p) => p.kind === 'circle' || Math.abs(polygonArea(p.points)) >= 0.25);
}
