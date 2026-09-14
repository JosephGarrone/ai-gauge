/*
 * Editor model -> gauge XML, in the canonical order of docs/gauge-xml-interface.md §3.
 *
 * Rules the firmware's parser imposes (§2): <source> before <face>; no entities, so values are
 * written raw; numbers as plain decimals with at most 4 places; shape coordinates on the 1/16 px
 * grid. An attribute is written when it differs from its schema default, plus the ones a reader
 * would expect to see (ranges, colours, fonts), so files stay short but self-explanatory.
 */

import { quantize } from './geometry.js';

/** Plain decimal, at most 4 places, no trailing zeros, never "-0". */
export function formatNum(v) {
    if (Number.isInteger(v)) return String(v === 0 ? 0 : v);
    const s = v.toFixed(4).replace(/(\.\d*?)0+$/, '$1').replace(/\.$/, '');
    return s === '-0' ? '0' : s;
}

/**
 * An attribute value, raw. The parser does not decode entities, so the only thing that cannot be
 * written is a value holding both quote characters; the validator reports that, and here the
 * double quotes are dropped so the file still parses.
 */
function attr(name, value) {
    let v = String(value);
    if (!v.includes('"')) return ` ${name}="${v}"`;
    if (!v.includes("'")) return ` ${name}='${v}'`;
    v = v.replaceAll('"', '');
    return ` ${name}="${v}"`;
}

class Writer {
    constructor() {
        this.lines = [];
    }

    line(depth, text) {
        this.lines.push('  '.repeat(depth) + text);
    }
}

/** Build an attribute string from [name, value, include] triples. */
function attrs(list) {
    return list
        .filter(([, value, include = true]) => include && value !== null && value !== undefined)
        .map(([name, value]) => attr(name, typeof value === 'number' ? formatNum(value) : value))
        .join('');
}

function writeShape(w, depth, tag, shape) {
    if (!shape || shape.parts.length === 0) return;
    w.line(depth, `<${tag}${attrs([['color', shape.color]])}>`);
    for (const p of shape.parts) {
        if (p.kind === 'circle') {
            w.line(depth + 1, `<circle${attrs([
                ['cx', quantize(p.cx), quantize(p.cx) !== 0],
                ['cy', quantize(p.cy), quantize(p.cy) !== 0],
                ['r', quantize(p.r)],
                ['color', p.color],
            ])}/>`);
        } else {
            const points = p.points.map(([x, y]) => `${formatNum(quantize(x))},${formatNum(quantize(y))}`).join(' ');
            w.line(depth + 1, `<polygon${attrs([['points', points], ['color', p.color]])}/>`);
        }
    }
    w.line(depth, `</${tag}>`);
}

export function serializeGauge(m) {
    const w = new Writer();
    w.line(0, '<?xml version="1.0" encoding="UTF-8"?>');
    w.line(0, `<gauge${attrs([['version', 1], ['id', m.id]])}>`);

    if (m.panel) {
        w.line(1, `<panel${attrs([['shape', m.panel.shape], ['width', m.panel.width], ['height', m.panel.height]])}/>`);
    }

    const s = m.source;
    w.line(1, `<source${attrs([
        ['channel', s.channel],
        ['unit', s.unit, s.unit !== ''],
        ['min', s.min],
        ['max', s.max],
        ['damping', s.damping],
    ])}/>`);
    w.line(0, '');

    const f = m.face;
    w.line(1, `<face${attrs([
        ['start-angle', f.startAngle],
        ['sweep', f.sweep],
        ['background', f.background],
        ['radius', f.radius, f.radius !== 0],
    ])}>`);

    for (const b of m.bands) {
        w.line(2, `<band${attrs([['from', b.from], ['to', b.to], ['color', b.color], ['width', b.width, b.width !== 18]])}/>`);
    }

    if (m.ticks) {
        const t = m.ticks;
        const a = attrs([
            ['major-every', t.majorEvery],
            ['minor-every', t.minorEvery],
            ['major-len', t.majorLen, !t.majorShape && t.majorLen !== 20],
            ['major-width', t.majorWidth, !t.majorShape && t.majorWidth !== 4],
            ['minor-len', t.minorLen, !t.minorShape && t.minorLen !== 10],
            ['minor-width', t.minorWidth, !t.minorShape && t.minorWidth !== 2],
            ['color', t.color],
        ]);
        if (t.majorShape || t.minorShape) {
            w.line(2, `<ticks${a}>`);
            writeShape(w, 3, 'major-shape', t.majorShape);
            writeShape(w, 3, 'minor-shape', t.minorShape);
            w.line(2, '</ticks>');
        } else {
            w.line(2, `<ticks${a}/>`);
        }
    }

    if (m.labels) {
        const l = m.labels;
        w.line(2, `<labels${attrs([
            ['every', l.every],
            ['font', l.font],
            ['color', l.color],
            ['radius', l.radius, l.radius !== 0],
            ['format', l.format, l.format !== '%g'],
        ])}/>`);
    }
    w.line(1, '</face>');
    w.line(0, '');

    const n = m.needle;
    const na = attrs([
        ['style', n.style, !n.shape],
        ['length', n.length, !n.shape && n.length !== 0],
        ['width', n.width, !n.shape],
        ['tail', n.tail, !n.shape && n.tail !== 0],
        ['color', n.color],
        ['pivot-radius', n.pivotRadius, !n.hub],
    ]);
    if (n.shape || n.hub) {
        w.line(1, `<needle${na}>`);
        writeShape(w, 2, 'shape', n.shape);
        writeShape(w, 2, 'hub', n.hub);
        w.line(1, '</needle>');
    } else {
        w.line(1, `<needle${na}/>`);
    }
    w.line(0, '');

    for (const t of m.titles) {
        w.line(1, `<title${attrs([['text', t.text], ['x', t.x], ['y', t.y], ['font', t.font], ['color', t.color]])}/>`);
    }

    if (m.readout) {
        const o = m.readout;
        w.line(1, `<readout${attrs([
            ['x', o.x],
            ['y', o.y],
            ['font', o.font],
            ['format', o.format],
            ['prefix', o.prefix, o.prefix !== ''],
            ['suffix', o.suffix, o.suffix !== ''],
            ['color', o.color],
        ])}/>`);
    }

    if (m.peak) {
        const k = m.peak;
        w.line(1, `<peak${attrs([
            ['color', k.color],
            ['length', k.length],
            ['width', k.width],
            ['show-value', 'false', !k.showValue],
            ['value-y', k.valueY],
            ['font', k.font, k.font !== 'montserrat_16'],
            ['format', k.format, k.format !== '%.1f'],
            ['prefix', k.prefix, k.prefix !== 'PEAK '],
        ])}/>`);
    }

    for (const a of m.alerts) {
        w.line(1, `<alert${attrs([
            ['above', a.above],
            ['below', a.below],
            ['flash-hz', a.flashHz],
            ['color', a.color],
            ['chime', a.chime ? 'true' : 'false'],
        ])}/>`);
    }

    w.line(0, '</gauge>');
    return w.lines.join('\n').replace(/\n\n+/g, '\n\n') + '\n';
}
