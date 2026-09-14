/*
 * SVG preview of a face, following gauge_render.c (docs/gauge-xml-interface.md §5).
 *
 * Split into layers that match the firmware's stacking order, so the app can rebuild the static face
 * only when the model changes and redraw just the moving layers each frame:
 *
 *   face (background, bands, minor ticks, major ticks, labels, titles) < peak marker < needle < hub
 *   < readout < peak value
 *
 * Built-in geometry is rounded to whole pixels where the firmware rounds it. Text uses Montserrat,
 * the same family the device's fonts are cut from; its metrics will not match LVGL to the pixel.
 */

import { LIMITS, PANEL, fontSize, lineHeight } from './schema.js';
import {
    labelRadius, labelStep, lround, needleLength, polar, scaleRadius, shapeExtent, stepValues,
    tickRadius, valueToDeg,
} from './geometry.js';
import { formatNumber, truncateBytes } from './printf.js';

const { cx: CX, cy: CY } = PANEL;
const FONT_FAMILY = "Montserrat, 'Segoe UI', system-ui, sans-serif";

const n = (v) => (Number.isInteger(v) ? String(v) : v.toFixed(3).replace(/\.?0+$/, ''));

export function escapeXml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

/** Pixels from the bottom of an LVGL line box up to the baseline, approximated for Montserrat. */
function baseLine(font) {
    return Math.round(fontSize(font) * 0.19);
}

/**
 * A single-line label. `top` is the top of LVGL's line box; the text is horizontally centred on `x`.
 */
function text(str, x, top, font, color, extra = '') {
    const y = top + lineHeight(font) - baseLine(font);
    return `<text x="${n(x)}" y="${n(y)}" fill="${color}" font-family="${FONT_FAMILY}" font-weight="500" ` +
        `font-size="${fontSize(font)}" text-anchor="middle" xml:space="preserve"${extra}>${escapeXml(str)}</text>`;
}

/** draw_text(): centred on (x, y), with the box's top at y - h/2 in integer maths. */
function centredText(str, x, y, font, color, extra) {
    return text(str, x, y - Math.floor(lineHeight(font) / 2), font, color, extra);
}

/** An integer pixel position `r` out from the centre, as polar() in the firmware rounds it. */
function polarPx(r, deg) {
    const [dx, dy] = polar(r, deg);
    return [CX + lround(dx), CY + lround(dy)];
}

/* --------------------------------------------------------------------- shapes ------- */

function resolveColor(part, shape, inherited, alert) {
    if (part.color) return part.color;
    if (shape.color) return shape.color;
    return alert ?? inherited;
}

export function shapeParts(shape, inherited, alert = null) {
    return shape.parts.map((p) => {
        const fill = resolveColor(p, shape, inherited, alert);
        if (p.kind === 'circle') return `<circle cx="${n(p.cx)}" cy="${n(p.cy)}" r="${n(p.r)}" fill="${fill}"/>`;
        const pts = p.points.map(([x, y]) => `${n(x)},${n(y)}`).join(' ');
        return `<polygon points="${pts}" fill="${fill}" fill-rule="nonzero"/>`;
    }).join('');
}

/* ----------------------------------------------------------------------- face ------- */

function bandPath(m, b) {
    // lv_draw_arc() takes whole degrees; round in LVGL's frame (0 at 3 o'clock) as arc_deg() does.
    const a0 = lround(valueToDeg(m, b.from) - 90) + 90;
    const a1 = lround(valueToDeg(m, b.to) - 90) + 90;
    const extent = a1 - a0;
    if (extent <= 0 || b.width <= 0) return '';

    const R = scaleRadius(m);
    const r = Math.max(0, R - b.width);
    const pt = (rad, deg) => polar(rad, deg).map((v, i) => n(v + (i ? CY : CX))).join(' ');

    if (extent >= 360) {
        const ring = (rad) => `M ${pt(rad, 0)} A ${rad} ${rad} 0 1 1 ${pt(rad, 180)} A ${rad} ${rad} 0 1 1 ${pt(rad, 0)} Z`;
        return `<path d="${ring(R)} ${r > 0 ? ring(r) : ''}" fill="${b.color}" fill-rule="evenodd"/>`;
    }
    const large = extent > 180 ? 1 : 0;
    const d = `M ${pt(R, a0)} A ${R} ${R} 0 ${large} 1 ${pt(R, a1)} L ${pt(r, a1)} ` +
        (r > 0 ? `A ${r} ${r} 0 ${large} 0 ${pt(r, a0)} Z` : 'Z');
    return `<path d="${d}" fill="${b.color}"/>`;
}

function ticksLayer(m, prefix) {
    const t = m.ticks;
    if (!t) return { defs: '', body: '' };
    const outer = tickRadius(m);
    let defs = '';
    let body = '';

    for (const [key, every, len, width, shape] of [
        ['minor', t.minorEvery, t.minorLen, t.minorWidth, t.minorShape],
        ['major', t.majorEvery, t.majorLen, t.majorWidth, t.majorShape],
    ]) {
        const values = stepValues(m, every, LIMITS.tickCount);
        if (values.length === 0) continue;
        body += `<g data-section="ticks" data-part="${key}">`;
        if (shape) {
            const id = `${prefix}-${key}-tick`;
            defs += `<g id="${id}">${shapeParts(shape, t.color)}</g>`;
            for (const v of values) {
                const deg = valueToDeg(m, v);
                const [dx, dy] = polar(outer, deg);
                body += `<use href="#${id}" transform="translate(${n(CX + dx)} ${n(CY + dy)}) rotate(${n(deg)})"/>`;
            }
        } else if (width > 0) {
            for (const v of values) {
                const deg = valueToDeg(m, v);
                const [x1, y1] = polarPx(outer, deg);
                const [x2, y2] = polarPx(outer - len, deg);
                body += `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${t.color}" stroke-width="${width}" stroke-linecap="butt"/>`;
            }
        }
        body += '</g>';
    }
    return { defs, body };
}

function labelsLayer(m) {
    const l = m.labels;
    if (!l) return '';
    const step = labelStep(m);
    const values = stepValues(m, step, LIMITS.labelCount);
    const r = labelRadius(m);
    let out = '<g data-section="labels">';
    for (const v of values) {
        const str = truncateBytes(formatNumber(l.format, v), LIMITS.labelText);
        const [x, y] = polarPx(r, valueToDeg(m, v));
        out += centredText(str, x, y, l.font, l.color);
    }
    return out + '</g>';
}

/** The static dial: everything baked into the device's pre-rendered background. */
export function renderFace(m, prefix = 'face') {
    const bands = m.bands.map((b, i) => `<g data-section="bands" data-index="${i}">${bandPath(m, b)}</g>`).join('');
    const ticks = ticksLayer(m, prefix);
    const titles = m.titles.map((t, i) =>
        centredText(t.text, t.x ?? CX, t.y, t.font, t.color, ` data-section="titles" data-index="${i}" data-drag="title"`)).join('');

    return `<defs>${ticks.defs}</defs>` +
        `<rect width="${PANEL.width}" height="${PANEL.height}" fill="${m.face.background}" data-section="dial"/>` +
        bands + ticks.body + labelsLayer(m) + titles;
}

/* --------------------------------------------------------------------- moving ------- */

/** The needle at schema angle `deg`. `alert` recolours parts that inherit the needle colour. */
export function renderNeedle(m, deg, alert = null) {
    const nd = m.needle;
    if (nd.shape) {
        return `<g transform="translate(${CX} ${CY}) rotate(${n(deg)})">${shapeParts(nd.shape, nd.color, alert)}</g>`;
    }

    const color = alert ?? nd.color;
    const rad = (deg * Math.PI) / 180;
    const dx = Math.sin(rad), dy = -Math.cos(rad);
    const px = Math.cos(rad), py = Math.sin(rad);
    const len = needleLength(m);
    const hw = Math.max(1, nd.width * 0.5);
    const tail = nd.tail;
    const P = (x, y) => [lround(x), lround(y)];
    const line = (a, b, w) => (w > 0
        ? `<line x1="${a[0]}" y1="${a[1]}" x2="${b[0]}" y2="${b[1]}" stroke="${color}" stroke-width="${w}" stroke-linecap="round"/>`
        : '');
    const tri = (pts) => `<polygon points="${pts.map((p) => p.join(',')).join(' ')}" fill="${color}"/>`;
    const tip = P(CX + dx * len, CY + dy * len);
    const back = P(CX - dx * tail, CY - dy * tail);

    if (nd.style === 'line') return line(tip, back, nd.width);
    if (nd.style === 'arrow') {
        const head = len * 0.25;
        const bx = CX + dx * (len - head), by = CY + dy * (len - head);
        return line(P(bx, by), back, Math.max(1, Math.trunc(nd.width / 2))) +
            tri([tip, P(bx + px * hw * 2, by + py * hw * 2), P(bx - px * hw * 2, by - py * hw * 2)]);
    }
    return (tail > 0 ? line([CX, CY], back, nd.width) : '') +
        tri([tip, P(CX + px * hw, CY + py * hw), P(CX - px * hw, CY - py * hw)]);
}

/** The hub. It never takes the alert colour. */
export function renderHub(m) {
    const nd = m.needle;
    if (nd.hub) return `<g transform="translate(${CX} ${CY})">${shapeParts(nd.hub, nd.color)}</g>`;
    if (nd.pivotRadius > 0) return `<circle cx="${CX}" cy="${CY}" r="${nd.pivotRadius}" fill="${nd.color}"/>`;
    return '';
}

export function renderPeakMarker(m, peak) {
    if (!m.peak || peak === null || m.peak.width <= 0) return '';
    const deg = valueToDeg(m, peak);
    const R = scaleRadius(m);
    const [x1, y1] = polarPx(R, deg);
    const [x2, y2] = polarPx(R - m.peak.length, deg);
    return `<line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" stroke="${m.peak.color}" stroke-width="${m.peak.width}" stroke-linecap="round"/>`;
}

export function readoutText(m, value) {
    const o = m.readout;
    const num = truncateBytes(formatNumber(o.format, value), 31);
    return truncateBytes(o.prefix + num + o.suffix, LIMITS.readoutText);
}

export function renderReadout(m, value, color) {
    if (!m.readout) return '';
    const o = m.readout;
    return text(readoutText(m, value), o.x ?? CX, o.y, o.font, color ?? o.color, ' data-section="readout" data-drag="readout"');
}

/** Where the peak value's top edge sits: explicit, or just below the readout. */
export function peakValueTop(m) {
    if (m.peak.valueY !== null) return m.peak.valueY;
    return m.readout ? m.readout.y + lineHeight(m.readout.font) + 2 : CY + 60;
}

export function renderPeakText(m, peak) {
    const k = m.peak;
    if (!k || !k.showValue) return '';
    const num = peak === null ? '--' : truncateBytes(formatNumber(k.format, peak), 31);
    const str = truncateBytes(k.prefix + num, LIMITS.peakText);
    return text(str, CX, peakValueTop(m), k.font, k.color, ' data-section="peak" data-drag="peak"');
}

/* --------------------------------------------------------------------- guides ------- */

/** Construction lines for the designer: panel inset, scale, tick and label circles, needle reach. */
export function renderGuides(m) {
    const circle = (r, color, dash, label) => (r > 0
        ? `<circle cx="${CX}" cy="${CY}" r="${r}" fill="none" stroke="${color}" stroke-width="1" stroke-dasharray="${dash}" vector-effect="non-scaling-stroke"><title>${label}: ${r} px</title></circle>`
        : '');
    let out = circle(PANEL.defaultRadius, '#5f6b7a', '2 4', 'Safe inset');
    out += circle(scaleRadius(m), '#00b8d4', '6 4', 'Scale radius');
    if (m.ticks) out += circle(tickRadius(m), '#ffd54f', '3 3', 'Tick circle');
    if (m.labels) out += circle(labelRadius(m), '#b388ff', '1 3', 'Label radius');
    const reach = m.needle.shape ? shapeExtent(m.needle.shape) : needleLength(m);
    out += circle(Math.round(reach), '#ff8a80', '8 6', 'Needle reach');
    out += `<path d="M ${CX - 8} ${CY} H ${CX + 8} M ${CX} ${CY - 8} V ${CY + 8}" stroke="#5f6b7a" stroke-width="1" vector-effect="non-scaling-stroke"/>`;

    // Start and end of the scale.
    for (const v of [m.source.min, m.source.max]) {
        const [x, y] = polar(scaleRadius(m) + 6, valueToDeg(m, v));
        out += `<line x1="${CX}" y1="${CY}" x2="${n(CX + x)}" y2="${n(CY + y)}" stroke="#00b8d4" stroke-width="1" stroke-dasharray="2 3" vector-effect="non-scaling-stroke"/>`;
    }
    return out;
}

/** A complete, self-contained SVG of the face at `value`, for thumbnails and tests. */
export function renderStaticSvg(m, value, { prefix = 'thumb', size = PANEL.width } = {}) {
    const deg = valueToDeg(m, value);
    return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${PANEL.width} ${PANEL.height}" width="${size}" height="${size}">` +
        `<clipPath id="${prefix}-clip"><circle cx="${CX}" cy="${CY}" r="${PANEL.cx}"/></clipPath>` +
        `<g clip-path="url(#${prefix}-clip)">${renderFace(m, prefix)}${renderNeedle(m, deg)}${renderHub(m)}` +
        `${m.readout ? renderReadout(m, value) : ''}</g></svg>`;
}
