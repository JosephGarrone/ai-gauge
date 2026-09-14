/*
 * Gauge XML parser: a line-for-line port of firmware/components/gauge_config/gauge_config.c.
 *
 * It exists so the editor can say what the *device* will make of a file, not what a browser's XML
 * parser would: the same subset, the same clamps, the same truncation and the same warning count.
 * That is deliberately not DOMParser, which decodes entities, rejects things the firmware accepts
 * and accepts nothing the firmware rejects in the same way.
 *
 * Keep it in step with the C. test/firmware-diff.test.mjs runs both parsers over a corpus and
 * compares the results field by field; CI builds the C side.
 *
 * Numbers follow the firmware's float32 storage (Math.fround) and its integer casts (truncation),
 * so a clamped or rounded value here is the value the device will hold.
 */

import {
    ERRORS, LIMITS, SCHEMA_VERSION, defaultAlert, defaultBand, defaultLabels, defaultModel,
    defaultPeak, defaultReadout, defaultTicks, defaultTitle,
} from './schema.js';

const f32 = Math.fround;
const encoder = new TextEncoder();
const decoder = new TextDecoder();

/* ------------------------------------------------------------------ XML scanning ---- */

function isSpace(c) {
    return c === ' ' || c === '\t' || c === '\r' || c === '\n';
}

function isNameChar(c) {
    return !isSpace(c) && c !== '>' && c !== '/' && c !== '=' && c !== '<';
}

/** next_tag(): the next element tag at or after `pos`, skipping text, comments, prolog and doctype. */
function nextTag(xml, pos) {
    const end = xml.length;
    let p = pos;

    while (p < end) {
        while (p < end && xml[p] !== '<') p++;
        if (p >= end) return null;

        if (xml.startsWith('<!--', p)) {
            const close = xml.indexOf('-->', p + 4);
            if (close < 0) return null;
            p = close + 3;
            continue;
        }
        if (xml[p + 1] === '?') {
            const close = xml.indexOf('?>', p + 2);
            if (close < 0) return null;
            p = close + 2;
            continue;
        }
        if (xml[p + 1] === '!') {
            const close = xml.indexOf('>', p);
            if (close < 0) return null;
            p = close + 1;
            continue;
        }
        break;
    }
    if (p >= end) return null;

    p++; // past '<'
    let isClose = false;
    if (p < end && xml[p] === '/') {
        isClose = true;
        p++;
    }

    const nameStart = p;
    while (p < end && isNameChar(xml[p])) p++;
    if (p === nameStart) return null;
    const name = xml.slice(nameStart, p);

    // Attributes run up to '>', but quoted values may contain '>'.
    const attrsStart = p;
    let quote = null;
    while (p < end) {
        const c = xml[p];
        if (quote !== null) {
            if (c === quote) quote = null;
        } else if (c === '"' || c === "'") {
            quote = c;
        } else if (c === '>') {
            break;
        }
        p++;
    }
    if (p >= end) return null; // unterminated tag

    let attrsEnd = p;
    let selfClosing = false;
    if (attrsEnd > attrsStart && xml[attrsEnd - 1] === '/') {
        selfClosing = true;
        attrsEnd--;
    }

    return { name, attrs: xml.slice(attrsStart, attrsEnd), selfClosing, isClose, next: p + 1 };
}

/** attr_find(): the raw value of the first attribute called `name`, or null. */
function attrFind(tag, name) {
    const s = tag.attrs;
    const end = s.length;
    let p = 0;

    while (p < end) {
        while (p < end && isSpace(s[p])) p++;
        if (p >= end) break;

        const keyStart = p;
        while (p < end && isNameChar(s[p])) p++;
        if (p === keyStart) {
            p++; // not a name character; skip it and resynchronise
            continue;
        }
        const key = s.slice(keyStart, p);

        while (p < end && isSpace(s[p])) p++;
        if (p >= end || s[p] !== '=') continue;
        p++;
        while (p < end && isSpace(s[p])) p++;
        if (p >= end || (s[p] !== '"' && s[p] !== "'")) continue;

        const q = s[p++];
        const valStart = p;
        while (p < end && s[p] !== q) p++;
        const val = s.slice(valStart, p);
        if (p < end) p++;

        if (key === name) return val;
    }
    return null;
}

/* ------------------------------------------------------- attribute value helpers ---- */

/** copy_str(): truncate to a C buffer of `size` bytes, by bytes, as the firmware does. */
function copyStr(value, size) {
    const bytes = encoder.encode(value);
    return bytes.length < size ? value : decoder.decode(bytes.subarray(0, size - 1));
}

/**
 * strtod() on the attribute's first 31 bytes: the longest numeric prefix, or null when there is
 * none. Covers what C accepts: leading whitespace, decimal with exponent, hex floats, inf and nan.
 */
function strtod(s) {
    const m = /^[ \t\n\v\f\r]*([+-]?)(?:0[xX]([0-9a-fA-F]+\.?[0-9a-fA-F]*|\.[0-9a-fA-F]+)(?:[pP]([+-]?\d+))?|((?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)|([iI][nN][fF]|[nN][aA][nN]))/.exec(s);
    if (m === null) return null;

    const sign = m[1] === '-' ? -1 : 1;
    if (m[4] !== undefined) return sign * Number(m[4]);
    if (m[5] !== undefined) return m[5].toLowerCase() === 'nan' ? NaN : sign * Infinity;

    const [intPart, fracPart = ''] = m[2].split('.');
    let v = intPart === '' ? 0 : parseInt(intPart, 16);
    for (let i = 0; i < fracPart.length; i++) {
        v += parseInt(fracPart[i], 16) / 16 ** (i + 1);
    }
    return sign * v * 2 ** Number(m[3] ?? 0);
}

class Reader {
    constructor() {
        this.warnings = [];
    }

    warn(message) {
        this.warnings.push(message);
    }

    str(tag, name, size) {
        const v = attrFind(tag, name);
        return v === null ? null : copyStr(v, size);
    }

    float(tag, name) {
        const v = attrFind(tag, name);
        if (v === null || v.length === 0) return null;
        const d = strtod(copyStr(v, 32));
        return d === null || !Number.isFinite(d) ? null : f32(d);
    }

    bool(tag, name) {
        const v = this.str(tag, name, 8);
        if (v === 'true' || v === '1') return true;
        if (v === 'false' || v === '0') return false;
        return null;
    }

    clamp(v, lo, hi, what) {
        if (v < lo) {
            this.warn(`${what}: ${v} is below ${lo}; clamped`);
            return f32(lo);
        }
        if (v > hi) {
            this.warn(`${what}: ${v} is above ${hi}; clamped`);
            return f32(hi);
        }
        return v;
    }

    /** attr_color(): a colour, or `fallback` (with a warning) when present but malformed. */
    color(tag, name, fallback, what) {
        const v = attrFind(tag, name);
        if (v === null) return fallback;
        const c = parseColor(v);
        if (c === null) {
            this.warn(`${what}: '${v}' is not a colour; default kept`);
            return fallback;
        }
        return c;
    }

    /** attr_px(): a non-negative pixel size, clamped to 0..4096 and truncated to an integer. */
    px(tag, name, fallback, what) {
        const v = this.float(tag, name);
        return v === null ? fallback : Math.trunc(this.clamp(v, 0, LIMITS.px, `${what} ${name}`));
    }

    /** attr_coord(): a signed position, clamped to +/-4096 and truncated to an integer. */
    coord(tag, name, fallback, what) {
        const v = this.float(tag, name);
        return v === null ? fallback : Math.trunc(this.clamp(v, -LIMITS.coord, LIMITS.coord, `${what} ${name}`));
    }
}

function hexVal(c) {
    const v = parseInt(c, 16);
    return /^[0-9a-fA-F]$/.test(c) ? v : -1;
}

/** parse_color(): "#rrggbb" or "#rgb" (the '#' is optional) to lowercase "#rrggbb", or null. */
export function parseColor(s) {
    if (s.length > 0 && s[0] === '#') s = s.slice(1);
    if (s.length !== 6 && s.length !== 3) return null;
    for (const c of s) {
        if (hexVal(c) < 0) return null;
    }
    const full = s.length === 3 ? s.replace(/./g, (c) => c + c) : s;
    return '#' + full.toLowerCase();
}

/* ------------------------------------------------------------------- shapes --------- */

function isDigit(c) {
    return c >= '0' && c <= '9';
}

/**
 * scan_number(): one number from an SVG-style list, using the same double arithmetic as the C so
 * the rounding of the stored 1/16 px value matches.
 */
function scanNumber(s, pos) {
    let p = pos;
    const end = s.length;
    let sign = 1;
    let mant = 0;
    let digits = false;

    if (p < end && (s[p] === '+' || s[p] === '-')) {
        sign = s[p] === '-' ? -1 : 1;
        p++;
    }
    while (p < end && isDigit(s[p])) {
        mant = mant * 10 + (s.charCodeAt(p) - 48);
        digits = true;
        p++;
    }
    if (p < end && s[p] === '.') {
        let scale = 0.1;
        p++;
        while (p < end && isDigit(s[p])) {
            mant += (s.charCodeAt(p) - 48) * scale;
            scale *= 0.1;
            digits = true;
            p++;
        }
    }
    if (!digits) return null;

    if (p < end && (s[p] === 'e' || s[p] === 'E')) {
        let q = p + 1;
        let esign = 1;
        let exp = 0;
        let edig = false;
        if (q < end && (s[q] === '+' || s[q] === '-')) {
            esign = s[q] === '-' ? -1 : 1;
            q++;
        }
        while (q < end && isDigit(s[q])) {
            if (exp < 1000) exp = exp * 10 + (s.charCodeAt(q) - 48);
            edig = true;
            q++;
        }
        if (edig) {
            mant *= Math.pow(10, esign * exp);
            p = q;
        }
    }

    const v = sign * mant;
    if (!Number.isFinite(v)) return null;
    return { value: f32(v), next: p };
}

function isListSep(c) {
    return isSpace(c) || c === ',';
}

/** lroundf(): round half away from zero. */
function lround(v) {
    return Math.sign(v) * Math.round(Math.abs(v));
}

/** Pixels to stored 1/16 px units, clamping to the coordinate limit. Returns units. */
function toShapeUnits(px, clampWarn) {
    const lim = LIMITS.shapeCoord;
    let v = px;
    if (v < -lim) {
        clampWarn();
        v = -lim;
    } else if (v > lim) {
        clampWarn();
        v = lim;
    }
    return lround(f32(v) * LIMITS.shapeScale);
}

const unitsToPx = (u) => u / LIMITS.shapeScale;

/** attr_opt_color(): a part or slot colour, or null to inherit. Malformed warns and inherits. */
function optColor(r, tag, what) {
    const v = attrFind(tag, 'color');
    if (v === null) return null;
    const c = parseColor(v);
    if (c === null) r.warn(`${what}: '${v}' is not a colour; inherits instead`);
    return c;
}

/** Shape slots while parsing: the model's shape plus the shared point-pool count. */
function openShape(r, tag, what) {
    return { shape: { color: optColor(r, tag, what), parts: [] }, points: 0 };
}

function parsePolygon(r, tag, slot, what) {
    const reject = (why) => r.warn(`${what} polygon dropped: ${why}`);

    if (slot.shape.parts.length >= LIMITS.shapeParts) return reject(`more than ${LIMITS.shapeParts} parts`);
    const s = attrFind(tag, 'points');
    if (s === null) return reject('no points attribute');

    let clampWarnings = 0;
    const clampWarn = () => clampWarnings++;
    const units = [];
    let p = 0;

    for (;;) {
        while (p < s.length && isListSep(s[p])) p++;
        if (p >= s.length) break;
        const x = scanNumber(s, p);
        if (x === null) return reject('malformed points list');
        p = x.next;
        while (p < s.length && isListSep(s[p])) p++;
        const y = scanNumber(s, p);
        if (y === null) return reject('malformed points list, or an odd number of coordinates');
        p = y.next;
        if (slot.points + units.length >= LIMITS.shapePoints) {
            return reject(`the slot's ${LIMITS.shapePoints}-vertex budget is exceeded`);
        }
        units.push([toShapeUnits(x.value, clampWarn), toShapeUnits(y.value, clampWarn)]);
    }

    if (units.length < 3) return reject('fewer than 3 vertices');

    let twiceArea = 0;
    for (let i = 0; i < units.length; i++) {
        const a = units[i];
        const b = units[(i + 1) % units.length];
        twiceArea += a[0] * b[1] - b[0] * a[1];
    }
    if (Math.abs(twiceArea) < (LIMITS.shapeScale * LIMITS.shapeScale) / 2) {
        return reject('area under 0.25 px²');
    }

    const color = optColor(r, tag, what);
    slot.points += units.length;
    slot.shape.parts.push({
        kind: 'polygon',
        points: units.map(([x, y]) => [unitsToPx(x), unitsToPx(y)]),
        color,
    });
    for (let i = 0; i < clampWarnings; i++) r.warn(`${what} polygon: a coordinate beyond ±${LIMITS.shapeCoord} px was clamped`);
}

function parseCircle(r, tag, slot, what) {
    const rad = r.float(tag, 'r');
    if (slot.shape.parts.length >= LIMITS.shapeParts || rad === null || !(rad > 0)) {
        r.warn(slot.shape.parts.length >= LIMITS.shapeParts
            ? `${what} circle dropped: more than ${LIMITS.shapeParts} parts`
            : `${what} circle dropped: r missing or not greater than 0`);
        return;
    }
    const cx = r.float(tag, 'cx') ?? 0;
    const cy = r.float(tag, 'cy') ?? 0;
    const clampWarn = () => r.warn(`${what} circle: a value beyond ±${LIMITS.shapeCoord} px was clamped`);

    const part = {
        kind: 'circle',
        cx: unitsToPx(toShapeUnits(cx, clampWarn)),
        cy: unitsToPx(toShapeUnits(cy, clampWarn)),
        r: unitsToPx(toShapeUnits(rad, clampWarn)),
        color: null,
    };
    if (part.r <= 0) {
        r.warn(`${what} circle dropped: r rounds to 0 at 1/16 px`);
        return;
    }
    part.color = optColor(r, tag, what);
    slot.shape.parts.push(part);
}

/* --------------------------------------------------------------- element parsers ---- */

function parsePanel(r, tag, m) {
    // The shape resets on every <panel>, but a repeated one keeps the earlier width and height.
    const panel = { shape: 'round', width: m.panel?.width ?? 0, height: m.panel?.height ?? 0 };
    const shape = r.str(tag, 'shape', 16);
    if (shape === 'square') {
        panel.shape = 'square';
    } else if (shape !== null && shape !== 'round') {
        r.warn(`panel shape '${shape}' is unknown; round kept`);
    }
    panel.width = r.px(tag, 'width', panel.width, 'panel');
    panel.height = r.px(tag, 'height', panel.height, 'panel');
    m.panel = panel;
}

function parseSource(r, tag, m) {
    const channel = r.str(tag, 'channel', 32);
    if (channel === null) return ERRORS.MISSING_REQUIRED;
    m.source.channel = channel;
    m.source.unit = r.str(tag, 'unit', 16) ?? m.source.unit;

    const min = r.float(tag, 'min');
    if (min === null) return ERRORS.MISSING_REQUIRED;
    m.source.min = min;
    const max = r.float(tag, 'max');
    if (max === null) return ERRORS.MISSING_REQUIRED;
    m.source.max = max;

    if (!(m.source.max > m.source.min)) return ERRORS.BAD_RANGE;

    const damping = r.float(tag, 'damping');
    if (damping !== null) m.source.damping = r.clamp(damping, 0, 1, 'source damping');
    return null;
}

function parseFace(r, tag, m) {
    const start = r.float(tag, 'start-angle');
    if (start !== null) m.face.startAngle = f32(start % 360);
    const sweep = r.float(tag, 'sweep');
    if (sweep !== null) m.face.sweep = r.clamp(sweep, 1, 360, 'face sweep');
    m.face.background = r.color(tag, 'background', m.face.background, 'face background');
    m.face.radius = r.px(tag, 'radius', m.face.radius, 'face');
}

function parseBand(r, tag, m) {
    const n = m.bands.length + 1;
    if (m.bands.length >= LIMITS.bands) {
        r.warn(`band ${n} dropped: more than ${LIMITS.bands} bands`);
        return;
    }
    const band = defaultBand();
    const from = r.float(tag, 'from');
    const to = from === null ? null : r.float(tag, 'to');
    if (from === null || to === null) {
        r.warn(`band ${n} dropped: from or to missing`);
        return;
    }
    band.from = r.clamp(from, m.source.min, m.source.max, `band ${n} from`);
    band.to = r.clamp(to, m.source.min, m.source.max, `band ${n} to`);
    if (band.to <= band.from) {
        r.warn(`band ${n} dropped: to is not greater than from`);
        return;
    }
    band.color = r.color(tag, 'color', band.color, `band ${n} color`);
    band.width = r.px(tag, 'width', band.width, `band ${n}`);
    m.bands.push(band);
}

function parseTicks(r, tag, m) {
    // A repeated <ticks> keeps the earlier values, as the firmware's single struct does.
    const t = m.ticks ?? defaultTicks();
    const every = (name, key) => {
        const v = r.float(tag, name);
        if (v !== null) t[key] = r.clamp(v, 0, 1e6, `ticks ${name}`);
    };
    every('major-every', 'majorEvery');
    every('minor-every', 'minorEvery');
    t.majorLen = r.px(tag, 'major-len', t.majorLen, 'ticks');
    t.minorLen = r.px(tag, 'minor-len', t.minorLen, 'ticks');
    t.majorWidth = r.px(tag, 'major-width', t.majorWidth, 'ticks');
    t.minorWidth = r.px(tag, 'minor-width', t.minorWidth, 'ticks');
    t.color = r.color(tag, 'color', t.color, 'ticks color');
    m.ticks = t;
}

function parseLabels(r, tag, m) {
    const l = m.labels ?? defaultLabels();
    const every = r.float(tag, 'every');
    if (every !== null) l.every = r.clamp(every, 0, 1e6, 'labels every');
    l.font = r.str(tag, 'font', 24) ?? l.font;
    l.format = r.str(tag, 'format', 16) ?? l.format;
    l.color = r.color(tag, 'color', l.color, 'labels color');
    l.radius = r.px(tag, 'radius', l.radius, 'labels');
    m.labels = l;
}

function parseNeedle(r, tag, m) {
    const n = m.needle;
    const style = r.str(tag, 'style', 16);
    if (style === 'line' || style === 'arrow' || style === 'taper') {
        n.style = style;
    } else if (style !== null) {
        r.warn(`needle style '${style}' is unknown; kept ${n.style}`);
    }
    n.length = r.px(tag, 'length', n.length, 'needle');
    n.width = r.px(tag, 'width', n.width, 'needle');
    n.pivotRadius = r.px(tag, 'pivot-radius', n.pivotRadius, 'needle');
    n.tail = r.px(tag, 'tail', n.tail, 'needle');
    n.color = r.color(tag, 'color', n.color, 'needle color');
}

function parseTitle(r, tag, m) {
    const n = m.titles.length + 1;
    if (m.titles.length >= LIMITS.titles) {
        r.warn(`title ${n} dropped: more than ${LIMITS.titles} titles`);
        return;
    }
    const t = defaultTitle();
    const text = r.str(tag, 'text', 32);
    if (text === null) {
        r.warn(`title ${n} dropped: no text attribute`);
        return;
    }
    t.text = text;
    t.x = r.coord(tag, 'x', t.x, `title ${n}`);
    t.y = r.coord(tag, 'y', t.y, `title ${n}`);
    t.font = r.str(tag, 'font', 24) ?? t.font;
    t.color = r.color(tag, 'color', t.color, `title ${n} color`);
    m.titles.push(t);
}

function parseReadout(r, tag, m) {
    const o = m.readout ?? defaultReadout();
    o.x = r.coord(tag, 'x', o.x, 'readout');
    o.y = r.coord(tag, 'y', o.y, 'readout');
    o.font = r.str(tag, 'font', 24) ?? o.font;
    o.format = r.str(tag, 'format', 16) ?? o.format;
    o.prefix = r.str(tag, 'prefix', 32) ?? o.prefix;
    o.suffix = r.str(tag, 'suffix', 32) ?? o.suffix;
    o.color = r.color(tag, 'color', o.color, 'readout color');
    m.readout = o;
}

function parsePeak(r, tag, m) {
    const k = m.peak ?? defaultPeak();
    k.color = r.color(tag, 'color', k.color, 'peak color');
    k.length = r.px(tag, 'length', k.length, 'peak');
    k.width = r.px(tag, 'width', k.width, 'peak');
    k.showValue = r.bool(tag, 'show-value') ?? k.showValue;
    k.valueY = r.coord(tag, 'value-y', k.valueY, 'peak');
    k.font = r.str(tag, 'font', 24) ?? k.font;
    k.format = r.str(tag, 'format', 16) ?? k.format;
    k.prefix = r.str(tag, 'prefix', 32) ?? k.prefix;
    m.peak = k;
}

function parseAlert(r, tag, m) {
    const n = m.alerts.length + 1;
    if (m.alerts.length >= LIMITS.alerts) {
        r.warn(`alert ${n} dropped: more than ${LIMITS.alerts} alerts`);
        return;
    }
    const a = defaultAlert();
    a.above = r.float(tag, 'above');
    a.below = r.float(tag, 'below');
    if (a.above === null && a.below === null) {
        r.warn(`alert ${n} dropped: neither above nor below is set`);
        return;
    }
    const hz = r.float(tag, 'flash-hz');
    if (hz !== null) a.flashHz = r.clamp(hz, 0, 20, `alert ${n} flash-hz`);
    a.color = r.color(tag, 'color', a.color, `alert ${n} color`);
    a.chime = r.bool(tag, 'chime') ?? a.chime;
    m.alerts.push(a);
}

/* ------------------------------------------------------------------ entry point ----- */

/**
 * Parse gauge XML exactly as the firmware would.
 *
 * @returns {{ok: true, model: object, warnings: string[]} | {ok: false, error: string}}
 *   `warnings.length` is the count the device reports; the strings explain each one.
 */
export function parseGauge(xml) {
    const r = new Reader();
    const m = defaultModel();

    let tag = null;
    let pos = 0;
    let foundRoot = false;
    while ((tag = nextTag(xml, pos)) !== null) {
        pos = tag.next;
        if (tag.isClose) continue;
        foundRoot = tag.name === 'gauge';
        break;
    }
    if (!foundRoot) return { ok: false, error: ERRORS.NO_ROOT };

    const version = r.float(tag, 'version');
    if (version === null || version < 1 || version > SCHEMA_VERSION) {
        return { ok: false, error: ERRORS.BAD_VERSION };
    }
    m.version = Math.trunc(version);

    const id = r.str(tag, 'id', 32);
    if (id === null) return { ok: false, error: ERRORS.MISSING_REQUIRED };
    m.id = id;

    let haveSource = false;
    let ctx = null;  // 'ticks' | 'needle' | null
    let slot = null; // the shape <polygon>/<circle> currently add to

    // Shapes are assembled with their point budgets, then attached when their slot opens.
    const openSlot = (owner, key, what) => {
        const s = openShape(r, tag, what);
        owner[key] = s.shape;
        return tag.selfClosing ? null : s;
    };

    while ((tag = nextTag(xml, pos)) !== null) {
        pos = tag.next;
        const name = tag.name;

        if (tag.isClose) {
            if (name === 'gauge') break;
            if (name === 'ticks' || name === 'needle') {
                ctx = null;
                slot = null;
            } else if (name === 'major-shape' || name === 'minor-shape' || name === 'shape' || name === 'hub') {
                slot = null;
            }
            continue;
        }

        if (name === 'panel') {
            parsePanel(r, tag, m);
        } else if (name === 'source') {
            const err = parseSource(r, tag, m);
            if (err !== null) return { ok: false, error: err };
            haveSource = true;
        } else if (name === 'face') {
            parseFace(r, tag, m);
        } else if (name === 'band') {
            parseBand(r, tag, m);
        } else if (name === 'ticks') {
            parseTicks(r, tag, m);
            ctx = tag.selfClosing ? null : 'ticks';
            slot = null;
        } else if (name === 'labels') {
            parseLabels(r, tag, m);
        } else if (name === 'needle') {
            parseNeedle(r, tag, m);
            ctx = tag.selfClosing ? null : 'needle';
            slot = null;
        } else if (ctx === 'ticks' && name === 'major-shape') {
            slot = openSlot(m.ticks, 'majorShape', 'major tick shape');
        } else if (ctx === 'ticks' && name === 'minor-shape') {
            slot = openSlot(m.ticks, 'minorShape', 'minor tick shape');
        } else if (ctx === 'needle' && name === 'shape') {
            slot = openSlot(m.needle, 'shape', 'needle shape');
        } else if (ctx === 'needle' && name === 'hub') {
            slot = openSlot(m.needle, 'hub', 'hub shape');
        } else if (slot !== null && name === 'polygon') {
            parsePolygon(r, tag, slot, slotName(m, slot));
        } else if (slot !== null && name === 'circle') {
            parseCircle(r, tag, slot, slotName(m, slot));
        } else if (name === 'title') {
            parseTitle(r, tag, m);
        } else if (name === 'readout') {
            parseReadout(r, tag, m);
        } else if (name === 'peak') {
            parsePeak(r, tag, m);
        } else if (name === 'alert') {
            parseAlert(r, tag, m);
        }
        // Unknown elements are ignored: forward compatibility.
    }

    if (!haveSource) return { ok: false, error: ERRORS.MISSING_REQUIRED };

    // A slot left with no valid parts means the built-in drawing, which the model spells null.
    if (m.ticks !== null) {
        if (m.ticks.majorShape?.parts.length === 0) m.ticks.majorShape = null;
        if (m.ticks.minorShape?.parts.length === 0) m.ticks.minorShape = null;
    }
    if (m.needle.shape?.parts.length === 0) m.needle.shape = null;
    if (m.needle.hub?.parts.length === 0) m.needle.hub = null;

    return { ok: true, model: m, warnings: r.warnings };
}

function slotName(m, slot) {
    if (m.ticks?.majorShape === slot.shape) return 'major tick shape';
    if (m.ticks?.minorShape === slot.shape) return 'minor tick shape';
    if (m.needle.shape === slot.shape) return 'needle shape';
    return 'hub shape';
}
