/*
 * Constants of the gauge XML format and the device it targets.
 *
 * Every number here comes from docs/gauge-xml-interface.md, which is kept in step with the normative
 * docs/gauge-config-schema.md and with gauge_config.h. If the firmware changes a limit, change it here
 * in the same commit; the differential test (test/firmware-diff.test.mjs) catches parser drift.
 */

export const SCHEMA_VERSION = 1;

/** The Waveshare 1.75" round AMOLED. Faces are authored at this size (panel scaling is M8). */
export const PANEL = Object.freeze({
    width: 466,
    height: 466,
    cx: 233,
    cy: 233,
    /** Shorter side minus an 8px safe inset each side, halved: board_profile_max_radius(). */
    defaultRadius: 225,
});

/** Compiled-in fonts and their LVGL line heights, in px. */
export const FONTS = Object.freeze({
    montserrat_12: 15,
    montserrat_16: 18,
    montserrat_18: 21,
    montserrat_20: 22,
    montserrat_22: 24,
    montserrat_24: 27,
    montserrat_26: 29,
    montserrat_48: 52,
});

export const FONT_NAMES = Object.freeze(Object.keys(FONTS));

/** LVGL's fallback when a font name is unknown. Not selectable, but a preview may need it. */
export const FALLBACK_FONT = { size: 14, lineHeight: 16 };

export const NEEDLE_STYLES = Object.freeze(['taper', 'line', 'arrow']);
export const PANEL_SHAPES = Object.freeze(['round', 'square']);

/**
 * String limits are the firmware's buffer sizes minus the terminator, and are counted in UTF-8
 * bytes, because that is how the parser truncates.
 */
export const LIMITS = Object.freeze({
    fileBytes: 16384,
    id: 31,
    channel: 31,
    unit: 15,
    text: 31,       // title text, readout prefix/suffix, peak prefix
    format: 15,
    font: 23,
    labelText: 15,  // rendered label buffer
    readoutText: 95,
    peakText: 79,
    bands: 8,
    titles: 4,
    alerts: 4,
    deviceFaces: 12,
    shapeParts: 6,
    shapePoints: 64,
    shapeCoord: 2047,
    shapeScale: 16, // stored units per px
    px: 4096,
    coord: 4096,
    tickCount: 400,
    labelCount: 100,
});

/** Parser error strings, exactly as gauge_config_err_str() returns them. */
export const ERRORS = Object.freeze({
    NO_ROOT: 'no <gauge> root element',
    BAD_VERSION: 'missing or unsupported schema version',
    MISSING_REQUIRED: 'missing required attribute',
    BAD_RANGE: 'invalid value range (max must exceed min)',
});

/** Needle-shape guidance from §4.6, until the frame cost is measured on hardware. */
export const NEEDLE_SHAPE_GUIDE = Object.freeze({ maxParts: 3, maxWidth: 20, maxLength: 180 });

export const ID_PATTERN = /^[A-Za-z0-9_-]{1,31}$/;

const encoder = new TextEncoder();

export function utf8Length(s) {
    return encoder.encode(s).length;
}

export function lineHeight(font) {
    return FONTS[font] ?? FALLBACK_FONT.lineHeight;
}

/** Nominal pixel size of a font, e.g. 24 for montserrat_24. */
export function fontSize(font) {
    return FONTS[font] ? Number(font.slice('montserrat_'.length)) : FALLBACK_FONT.size;
}

/**
 * Schema defaults, as gauge_config_set_defaults() and the per-element parsers apply them.
 * The editor model uses the same shape as the parser's output; see parse.js.
 */
export function defaultTicks() {
    return {
        majorEvery: 10, minorEvery: 0,
        majorLen: 20, minorLen: 10,
        majorWidth: 4, minorWidth: 2,
        color: '#ffffff',
        majorShape: null, minorShape: null,
    };
}

export function defaultLabels() {
    return { every: 0, font: 'montserrat_24', color: '#ffffff', radius: 0, format: '%g' };
}

export function defaultNeedle() {
    return {
        style: 'taper', length: 0, width: 12, tail: 0,
        color: '#ff1744', pivotRadius: 16,
        shape: null, hub: null,
    };
}

export function defaultBand() {
    return { from: 0, to: 0, color: '#ffffff', width: 18 };
}

export function defaultTitle() {
    return { text: '', x: null, y: 0, font: 'montserrat_20', color: '#ffffff' };
}

export function defaultReadout() {
    return { x: null, y: 0, font: 'montserrat_48', format: '%.1f', prefix: '', suffix: '', color: '#ffffff' };
}

export function defaultPeak() {
    return {
        color: '#ffab00', length: 28, width: 5, showValue: true,
        valueY: null, font: 'montserrat_16', format: '%.1f', prefix: 'PEAK ',
    };
}

export function defaultAlert() {
    return { above: null, below: null, color: '#d50000', flashHz: 2, chime: false };
}

/** A complete model with every element at its default: what an empty file with only a source means. */
export function defaultModel() {
    return {
        version: SCHEMA_VERSION,
        id: '',
        panel: null,
        source: { channel: '', unit: '', min: 0, max: 100, damping: Math.fround(0.15) },
        face: { startAngle: 225, sweep: 270, background: '#000000', radius: 0 },
        bands: [],
        ticks: null,
        labels: null,
        needle: defaultNeedle(),
        titles: [],
        readout: null,
        peak: null,
        alerts: [],
    };
}
