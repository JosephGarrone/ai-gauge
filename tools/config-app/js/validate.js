/*
 * Editor-side checks (docs/gauge-xml-interface.md §2, §4.5, §6).
 *
 * The firmware forgives a lot: it clamps, drops and truncates, and only counts what it did. An
 * editor should prevent every one of those conditions instead, so each is reported here with the
 * reason and where to fix it:
 *
 *   error    the device would reject the file, change what was authored, or hit undefined behaviour
 *   warning  the device will do what was written, but it is probably not what was meant
 *   info     worth knowing; nothing is wrong
 *
 * As a backstop, the model is written out and re-read with the firmware-faithful parser. If the
 * device would report more warnings than the checks above explain, that is reported too, so a gap
 * in these checks cannot hide a warning.
 */

import {
    FONTS, ID_PATTERN, LIMITS, NEEDLE_SHAPE_GUIDE, PANEL, utf8Length,
} from './schema.js';
import {
    labelRadius, labelStep, needleLength, polygonArea, polygonSelfIntersects, quantize, scaleRadius,
    shapeBounds, shapeVertexCount, stepCount, stepValues, tickRadius, widestBand,
} from './geometry.js';
import { checkFormat, formatNumber } from './printf.js';
import { parseGauge } from './parse.js';
import { serializeGauge } from './serialize.js';

const COLOR = /^#[0-9a-f]{6}$/;

class Issues {
    constructor() {
        this.list = [];
        this.deviceWarnings = 0;
    }

    add(severity, section, message, { index = null, device = false } = {}) {
        this.list.push({ severity, section, index, message });
        if (device) this.deviceWarnings++;
    }

    error(section, message, opts) { this.add('error', section, message, opts); }
    warning(section, message, opts) { this.add('warning', section, message, opts); }
    info(section, message, opts) { this.add('info', section, message, opts); }
}

function checkString(is, section, label, value, maxBytes, opts = {}) {
    if (opts.required && value === '') {
        is.error(section, `${label} is required.`, opts);
        return;
    }
    const bytes = utf8Length(value);
    if (bytes > maxBytes) {
        is.error(section, `${label} is ${bytes} bytes; the device keeps only the first ${maxBytes}.`, opts);
    }
    if (/[&<]/.test(value) || (value.includes('"') && value.includes("'"))) {
        is.error(section, `${label} contains & < or both quote characters, which the file format cannot carry.`, opts);
    }
    const unusual = [...new Set([...value].filter((c) => c < ' ' || c > '~'))];
    if (unusual.length > 0) {
        is.warning(section, `${label} uses ${unusual.map((c) => `'${c}'`).join(', ')}; the device fonts only have plain ASCII, so it may not render.`, opts);
    }
}

function checkFont(is, section, label, font, opts) {
    if (!(font in FONTS)) {
        is.error(section, `${label} '${font}' is not a device font; it would fall back to montserrat_14.`, opts);
    }
}

function checkColor(is, section, label, value, opts = {}) {
    if (value !== null && !COLOR.test(value)) {
        is.error(section, `${label} '${value}' is not a #rrggbb colour.`, { ...opts, device: true });
    }
}

function checkRange(is, section, label, value, lo, hi, opts = {}) {
    if (!Number.isFinite(value)) {
        is.error(section, `${label} must be a number.`, opts);
    } else if (value < lo || value > hi) {
        is.error(section, `${label} ${value} is outside ${lo}…${hi}; the device clamps it.`, { ...opts, device: true });
    }
}

function checkFormatField(is, section, label, fmt, maxOutput, sampleValues, opts = {}) {
    const bad = checkFormat(fmt);
    if (bad) {
        is.error(section, `${label} '${fmt}': ${bad}.`, opts);
        return;
    }
    checkString(is, section, label, fmt, LIMITS.format, opts);
    for (const v of sampleValues) {
        const text = formatNumber(fmt, v);
        if (utf8Length(text) > maxOutput) {
            is.warning(section, `${label} turns ${v} into '${text}', longer than the ${maxOutput} characters the device shows.`, opts);
            break;
        }
    }
}

/* ------------------------------------------------------------------- shapes --------- */

function checkShape(is, section, label, shape, { needle = false } = {}) {
    if (!shape) return;
    if (shape.parts.length === 0) {
        is.info(section, `${label} has no parts, so the built-in drawing is used.`);
        return;
    }
    checkColor(is, section, `${label} colour`, shape.color);

    if (shape.parts.length > LIMITS.shapeParts) {
        is.error(section, `${label} has ${shape.parts.length} parts; the device drops everything after the first ${LIMITS.shapeParts}.`, { device: true });
    }
    const vertices = shapeVertexCount(shape);
    if (vertices > LIMITS.shapePoints) {
        is.error(section, `${label} has ${vertices} polygon vertices; the budget is ${LIMITS.shapePoints} per slot, and polygons past it are dropped. Circles are free.`, { device: true });
    }

    shape.parts.forEach((p, i) => {
        const what = `${label} part ${i + 1}`;
        checkColor(is, section, `${what} colour`, p.color);
        if (p.kind === 'circle') {
            if (!(quantize(p.r) > 0)) {
                is.error(section, `${what}: radius must be at least 1/32 px.`, { device: true });
            }
            if ([p.cx, p.cy, p.r].some((v) => Math.abs(v) > LIMITS.shapeCoord)) {
                is.error(section, `${what}: values are limited to ±${LIMITS.shapeCoord} px.`, { device: true });
            }
            return;
        }
        const pts = p.points.map(([x, y]) => [quantize(x), quantize(y)]);
        if (pts.length < 3) {
            is.error(section, `${what}: a polygon needs at least 3 vertices.`, { device: true });
            return;
        }
        if (pts.some(([x, y]) => Math.abs(x) > LIMITS.shapeCoord || Math.abs(y) > LIMITS.shapeCoord)) {
            is.error(section, `${what}: coordinates are limited to ±${LIMITS.shapeCoord} px.`, { device: true });
        }
        if (Math.abs(polygonArea(pts)) < 0.25) {
            is.error(section, `${what}: the polygon has almost no area, so the device drops it.`, { device: true });
        } else if (polygonSelfIntersects(pts)) {
            is.error(section, `${what}: edges cross. The device fills overlaps instead of cutting them out, so it would not look like this preview; untangle it or split it into parts.`);
        }
    });

    if (needle) {
        const b = shapeBounds(shape);
        if (shape.parts.length > NEEDLE_SHAPE_GUIDE.maxParts) {
            is.warning(section, `${label} has ${shape.parts.length} parts. Until shaped needles are measured on hardware, keep to ${NEEDLE_SHAPE_GUIDE.maxParts} or fewer: each part is re-rasterised on every visible move.`);
        }
        if (b && (b.x2 - b.x1 > NEEDLE_SHAPE_GUIDE.maxWidth + 0.5 || -b.y1 > NEEDLE_SHAPE_GUIDE.maxLength + 0.5)) {
            is.warning(section, `${label} is ${Math.round(b.x2 - b.x1)} px wide and reaches ${Math.round(-b.y1)} px from the pivot. The built-in needle's footprint (about ${NEEDLE_SHAPE_GUIDE.maxWidth} × ${NEEDLE_SHAPE_GUIDE.maxLength} px) is the tested limit; a bigger needle costs more per frame.`);
        }
    }
}

/** Half-width of a tick at its widest, for the neighbour-spacing check. */
function tickHalfWidth(shape, builtinWidth) {
    if (!shape) return builtinWidth / 2;
    let w = 0;
    for (const p of shape.parts) {
        if (p.kind === 'circle') w = Math.max(w, Math.abs(p.cx) + p.r);
        else for (const [x] of p.points) w = Math.max(w, Math.abs(x));
    }
    return w;
}

/* ------------------------------------------------------------------ sections -------- */

function checkGauge(is, m) {
    if (!ID_PATTERN.test(m.id)) {
        is.error('gauge', m.id === ''
            ? 'The face needs an id: 1–31 letters, digits, _ or -. It is also the file name on the device.'
            : `Id '${m.id}' must be 1–31 letters, digits, _ or -.`);
    }
    if (m.panel) {
        if (m.panel.width !== PANEL.width || m.panel.height !== PANEL.height) {
            is.warning('gauge', `The panel is declared as ${m.panel.width} × ${m.panel.height}, but the device is ${PANEL.width} × ${PANEL.height} and does not scale faces yet (M8).`);
        }
    }

    const s = m.source;
    checkString(is, 'gauge', 'Channel', s.channel, LIMITS.channel, { required: true });
    checkString(is, 'gauge', 'Unit', s.unit, LIMITS.unit);
    if (!Number.isFinite(s.min) || !Number.isFinite(s.max)) {
        is.error('gauge', 'Min and max must be numbers.');
    } else if (!(s.max > s.min)) {
        is.error('gauge', 'Max must be greater than min, or the device rejects the whole file.');
    }
    checkRange(is, 'gauge', 'Damping', s.damping, 0, 1);
}

function checkDial(is, m) {
    const f = m.face;
    checkRange(is, 'dial', 'Sweep', f.sweep, 1, 360);
    if (!Number.isFinite(f.startAngle)) is.error('dial', 'Start angle must be a number.');
    checkColor(is, 'dial', 'Background', f.background);
    checkRange(is, 'dial', 'Radius', f.radius, 0, LIMITS.px);
    const R = scaleRadius(m);
    if (R > PANEL.cx) {
        is.warning('dial', `A ${R} px radius runs off the ${PANEL.width} px panel.`);
    } else if (R > PANEL.defaultRadius) {
        is.info('dial', `A ${R} px radius is inside the panel but outside its 8 px safe inset.`);
    }
}

function checkBands(is, m) {
    const { min, max } = m.source;
    if (m.bands.length > LIMITS.bands) {
        is.error('bands', `${m.bands.length} bands; the device keeps the first ${LIMITS.bands}.`, { device: true });
    }
    const R = scaleRadius(m);
    m.bands.forEach((b, index) => {
        const opts = { index };
        const n = `Band ${index + 1}`;
        if (!Number.isFinite(b.from) || !Number.isFinite(b.to)) {
            is.error('bands', `${n}: from and to must be numbers.`, opts);
            return;
        }
        if (b.from < min || b.from > max) is.error('bands', `${n}: from ${b.from} is outside the scale (${min}…${max}); the device clamps it.`, { index, device: true });
        if (b.to < min || b.to > max) is.error('bands', `${n}: to ${b.to} is outside the scale (${min}…${max}); the device clamps it.`, { index, device: true });
        if (Math.min(Math.max(b.to, min), max) <= Math.min(Math.max(b.from, min), max)) {
            is.error('bands', `${n}: to must be greater than from, or the device drops the band.`, { index, device: true });
        }
        checkColor(is, 'bands', `${n} colour`, b.color, opts);
        checkRange(is, 'bands', `${n} width`, b.width, 0, LIMITS.px, opts);
        if (b.width > R) is.warning('bands', `${n} is wider (${b.width} px) than the scale radius (${R} px).`, opts);
    });
}

function checkTicks(is, m) {
    const t = m.ticks;
    if (!t) return;
    const span = m.source.max - m.source.min;
    const radius = tickRadius(m);

    for (const [kind, every, len, width, shape] of [
        ['Major', t.majorEvery, t.majorLen, t.majorWidth, t.majorShape],
        ['Minor', t.minorEvery, t.minorLen, t.minorWidth, t.minorShape],
    ]) {
        checkRange(is, 'ticks', `${kind} tick interval`, every, 0, 1e6);
        if (!shape) {
            checkRange(is, 'ticks', `${kind} tick length`, len, 0, LIMITS.px);
            checkRange(is, 'ticks', `${kind} tick width`, width, 0, LIMITS.px);
        }
        if (!(every > 0) || !(span > 0)) continue;

        const count = stepCount(m, every);
        if (count > LIMITS.tickCount) {
            is.warning('ticks', `${kind} ticks every ${every} make ${count + 1} ticks; the device skips them all above ${LIMITS.tickCount + 1}.`);
            continue;
        }
        const spacing = (radius * m.face.sweep * Math.PI / 180 * every) / span;
        const halfWidth = tickHalfWidth(shape, width);
        if (halfWidth * 2 > spacing) {
            is.warning('ticks', `${kind} ticks are ${(halfWidth * 2).toFixed(1)} px wide but only ${spacing.toFixed(1)} px apart, so neighbours run into each other.`);
        }
    }
    if (t.majorEvery > 0 && span > 0 && Math.abs(span / t.majorEvery - Math.round(span / t.majorEvery)) > 1e-6) {
        is.info('ticks', `Ticks start at min and step by ${t.majorEvery}, so there is no major tick exactly at max (${m.source.max}).`);
    }
    if (radius <= 0) is.warning('ticks', `Bands leave no room for ticks: the tick circle radius is ${radius} px.`);

    checkColor(is, 'ticks', 'Tick colour', t.color);
    checkShape(is, 'ticks', 'Major tick shape', t.majorShape);
    checkShape(is, 'ticks', 'Minor tick shape', t.minorShape);
}

function checkLabels(is, m) {
    const l = m.labels;
    if (!l) return;
    checkRange(is, 'labels', 'Label interval', l.every, 0, 1e6);
    checkFont(is, 'labels', 'Label font', l.font);
    checkColor(is, 'labels', 'Label colour', l.color);
    checkRange(is, 'labels', 'Label radius', l.radius, 0, LIMITS.px);

    const step = labelStep(m);
    if (!(step > 0)) {
        is.warning('labels', 'Labels follow the major tick interval, which is 0, so none are drawn.');
        return;
    }
    const count = stepCount(m, step);
    if (count > LIMITS.labelCount) {
        is.warning('labels', `Labels every ${step} make ${count + 1} labels; the device skips them all above ${LIMITS.labelCount + 1}.`);
        return;
    }
    checkFormatField(is, 'labels', 'Label format', l.format, LIMITS.labelText, stepValues(m, step, LIMITS.labelCount));
    const r = labelRadius(m);
    if (r <= 0) is.warning('labels', `The computed label radius is ${r} px, so labels land on the far side of the dial. Set a radius.`);
}

function checkNeedle(is, m) {
    const n = m.needle;
    checkColor(is, 'needle', 'Needle colour', n.color);
    if (!n.shape) {
        for (const [label, v] of [['Length', n.length], ['Width', n.width], ['Tail', n.tail]]) {
            checkRange(is, 'needle', `Needle ${label.toLowerCase()}`, v, 0, LIMITS.px);
        }
        if (needleLength(m) > scaleRadius(m)) {
            is.info('needle', `The needle (${needleLength(m)} px) reaches past the scale radius (${scaleRadius(m)} px).`);
        }
    }
    if (!n.hub) checkRange(is, 'needle', 'Pivot radius', n.pivotRadius, 0, LIMITS.px);
    checkShape(is, 'needle', 'Needle shape', n.shape, { needle: true });
    checkShape(is, 'needle', 'Hub shape', n.hub);
}

function checkTitles(is, m) {
    if (m.titles.length > LIMITS.titles) {
        is.error('titles', `${m.titles.length} titles; the device keeps the first ${LIMITS.titles}.`, { device: true });
    }
    m.titles.forEach((t, index) => {
        const opts = { index };
        const n = `Title ${index + 1}`;
        if (t.text === '') is.error('titles', `${n} has no text; an empty title is written without it and dropped.`, opts);
        checkString(is, 'titles', `${n} text`, t.text, LIMITS.text, opts);
        checkFont(is, 'titles', `${n} font`, t.font, opts);
        checkColor(is, 'titles', `${n} colour`, t.color, opts);
        if (t.x !== null) checkRange(is, 'titles', `${n} x`, t.x, -LIMITS.coord, LIMITS.coord, opts);
        checkRange(is, 'titles', `${n} y`, t.y, -LIMITS.coord, LIMITS.coord, opts);
    });
}

function checkReadout(is, m) {
    const o = m.readout;
    if (!o) return;
    checkFont(is, 'readout', 'Readout font', o.font);
    checkColor(is, 'readout', 'Readout colour', o.color);
    checkString(is, 'readout', 'Prefix', o.prefix, LIMITS.text);
    checkString(is, 'readout', 'Suffix', o.suffix, LIMITS.text);
    if (o.x !== null) checkRange(is, 'readout', 'Readout x', o.x, -LIMITS.coord, LIMITS.coord);
    checkRange(is, 'readout', 'Readout y', o.y, -LIMITS.coord, LIMITS.coord);
    checkFormatField(is, 'readout', 'Readout format', o.format, 31, [m.source.min, m.source.max]);
}

function checkPeak(is, m) {
    const k = m.peak;
    if (!k) return;
    checkColor(is, 'peak', 'Peak colour', k.color);
    checkRange(is, 'peak', 'Marker length', k.length, 0, LIMITS.px);
    checkRange(is, 'peak', 'Marker width', k.width, 0, LIMITS.px);
    if (k.showValue) {
        checkFont(is, 'peak', 'Peak font', k.font);
        checkString(is, 'peak', 'Peak prefix', k.prefix, LIMITS.text);
        if (k.valueY !== null) checkRange(is, 'peak', 'Peak value y', k.valueY, -LIMITS.coord, LIMITS.coord);
        checkFormatField(is, 'peak', 'Peak format', k.format, 31, [m.source.min, m.source.max]);
    }
}

function checkAlerts(is, m) {
    const { min, max } = m.source;
    if (m.alerts.length > LIMITS.alerts) {
        is.error('alerts', `${m.alerts.length} alerts; the device keeps the first ${LIMITS.alerts}.`, { device: true });
    }
    m.alerts.forEach((a, index) => {
        const opts = { index };
        const n = `Alert ${index + 1}`;
        if (a.above === null && a.below === null) {
            is.error('alerts', `${n} needs an above or below threshold, or the device drops it.`, { index, device: true });
            return;
        }
        checkRange(is, 'alerts', `${n} flash rate`, a.flashHz, 0, 20, opts);
        checkColor(is, 'alerts', `${n} colour`, a.color, opts);
        if (a.above !== null && a.above >= max) {
            is.warning('alerts', `${n} can never fire: it needs the value strictly above ${a.above}, but the needle stops at max (${max}).`, opts);
        }
        if (a.below !== null && a.below <= min) {
            is.warning('alerts', `${n} can never fire: it needs the value strictly below ${a.below}, but the needle stops at min (${min}).`, opts);
        }
        if (a.above !== null && a.below !== null && a.above < a.below) {
            is.warning('alerts', `${n} fires at every value, because above (${a.above}) is less than below (${a.below}).`, opts);
        }
    });
    if (m.alerts.some((a) => a.chime)) {
        is.info('alerts', 'Chimes sound only if they are enabled in the device settings, and need a speaker fitted.');
    }
}

/**
 * Check a model.
 * @returns {{issues: Array<{severity, section, index, message}>, xml: string, bytes: number,
 *            device: {ok: boolean, error?: string, warnings?: string[]}}}
 */
export function validateModel(m) {
    const is = new Issues();

    checkGauge(is, m);
    checkDial(is, m);
    checkBands(is, m);
    checkTicks(is, m);
    checkLabels(is, m);
    checkNeedle(is, m);
    checkTitles(is, m);
    checkReadout(is, m);
    checkPeak(is, m);
    checkAlerts(is, m);

    const xml = serializeGauge(m);
    const bytes = utf8Length(xml);
    if (bytes > LIMITS.fileBytes) {
        is.error('file', `The file is ${bytes} bytes; the device accepts at most ${LIMITS.fileBytes}.`);
    }

    // Backstop: what the device itself would say about the file this model writes.
    const device = parseGauge(xml);
    if (!device.ok) {
        is.error('file', `The device would reject this file: ${device.error}.`);
    } else if (device.warnings.length > is.deviceWarnings) {
        is.error('file', `The device would report ${device.warnings.length} warning(s) for this file: ${device.warnings.join('; ')}.`);
    }

    const order = { error: 0, warning: 1, info: 2 };
    is.list.sort((a, b) => order[a.severity] - order[b.severity]);
    return { issues: is.list, xml, bytes, device };
}
