/*
 * The face editor: wiring between the model, the inspector forms, the live preview and storage.
 * Everything format-related lives in the pure modules alongside (parse, serialize, validate,
 * geometry, render, sim), which are unit-tested under Node; this file is the browser shell.
 */

import { BLANK_XML, EXAMPLES } from './examples.js';
import { parseGauge } from './parse.js';
import {
    renderFace, renderGuides, renderHub, renderNeedle, renderPeakMarker, renderPeakText, renderReadout,
    renderStaticSvg,
} from './render.js';
import {
    LIMITS, PANEL, defaultAlert, defaultBand, defaultLabels, defaultPeak, defaultReadout, defaultTicks,
    defaultTitle, lineHeight,
} from './schema.js';
import { labelRadius, needleLength, scaleRadius, valueToDeg } from './geometry.js';
import { shapeEditor } from './shape-editor.js';
import { createSim, needleAlertColor, readoutColor, resetPeak, stepSim } from './sim.js';
import { emptyLibrary, listFaces, loadLibrary, newKey, saveLibrary, uniqueId } from './store.js';
import { validateModel } from './validate.js';
import {
    DeviceError, deleteDeviceFace, detectGauge, gaugeEditorUrl, getFaceInventory, getFaceXml, pageBlockReason, uploadFace,
    uploadPlan,
} from './device.js';
import { Form, ask, button, h, itemCard, presenceToggle, row, sectionHead, toast } from './ui.js';

const SECTIONS = [
    ['gauge', 'Gauge', 'face'],
    ['dial', 'Dial', 'face'],
    ['bands', 'Bands', 'face'],
    ['ticks', 'Ticks', 'face'],
    ['labels', 'Labels', 'face'],
    ['needle', 'Needle & hub', 'face'],
    ['titles', 'Titles', 'face'],
    ['readout', 'Readout', 'face'],
    ['peak', 'Peak hold', 'face'],
    ['alerts', 'Alerts', 'face'],
    ['library', 'My faces', 'manage'],
    ['device', 'Device', 'manage'],
];

const $ = (id) => document.getElementById(id);

const app = {
    lib: emptyLibrary(),
    key: null,
    model: null,
    undo: [],
    redo: [],
    coalesce: { key: null, at: 0 },
    section: 'gauge',
    /** The /api/status of the gauge that served this page, or null when it was not served by one. */
    gauge: null,
    form: null,
    editors: [],
    ui: { shapes: {} },
    validation: null,
    faceDirty: true,
    sim: null,
    input: { value: 0, sweep: false, sweepStart: 0 },
    showGuides: false,
    saveTimer: 0,
    validateQueued: false,
    storageOk: true,
};

/* ------------------------------------------------------------------ model changes --- */

/**
 * Apply a change to the model, with undo. Consecutive changes with the same `coalesce` key within a
 * second merge into one undo step, so typing or dragging is one step rather than hundreds.
 */
app.commit = (mutate, { coalesce = null, rebuild = false } = {}) => {
    const now = performance.now();
    const merge = coalesce !== null && app.coalesce.key === coalesce && now - app.coalesce.at < 1000;
    if (!merge) {
        app.undo.push(JSON.stringify(app.model));
        if (app.undo.length > 200) app.undo.shift();
    }
    app.coalesce = { key: coalesce, at: now };
    app.redo = [];
    mutate(app.model);
    changed({ rebuild });
};

app.rebuildInspector = () => renderInspector();

function changed({ rebuild = false } = {}) {
    app.faceDirty = true;
    if (rebuild) renderInspector();
    else {
        app.form?.sync();
        for (const ed of app.editors) ed.refresh();
    }
    queueValidate();
    queueSave();
    updateUndoButtons();
}

function undoRedo(from, to) {
    if (from.length === 0) return;
    to.push(JSON.stringify(app.model));
    app.model = JSON.parse(from.pop());
    app.coalesce = { key: null, at: 0 };
    app.sim = createSim(app.model);
    changed({ rebuild: true });
}

function updateUndoButtons() {
    $('btn-undo').disabled = app.undo.length === 0;
    $('btn-redo').disabled = app.redo.length === 0;
}

function queueSave() {
    clearTimeout(app.saveTimer);
    app.saveTimer = setTimeout(saveNow, 300);
}

function saveNow() {
    if (app.key) app.lib.faces[app.key] = { model: app.model, updated: Date.now() };
    app.lib.current = app.key;
    const ok = saveLibrary(app.lib);
    if (!ok && app.storageOk) toast('This browser is not letting the page save. Faces will be lost when the tab closes; download the XML to keep them.', 'error');
    app.storageOk = ok;
    renderFaceSelect();
}

/* ------------------------------------------------------------------- faces ---------- */

function openFace(key) {
    if (app.key && app.lib.faces[app.key]) saveNow();
    app.key = key;
    app.model = structuredClone(app.lib.faces[key].model);
    app.undo = [];
    app.redo = [];
    app.ui.shapes = {};
    app.sim = createSim(app.model);
    app.input.value = app.model.source.min;
    app.faceDirty = true;
    app.lib.current = key;
    saveLibrary(app.lib);
    renderFaceSelect();
    renderInspector();
    queueValidate();
    updateUndoButtons();
    syncValueControls();
}

function addFace(model) {
    const key = newKey();
    model.id = uniqueId(app.lib, model.id || 'face', key);
    app.lib.faces[key] = { model, updated: Date.now() };
    openFace(key);
    return key;
}

async function fetchExample(file) {
    const res = await fetch(`examples/${file}`, { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return res.text();
}

function describeWarnings(warnings) {
    return h('div', {},
        h('p', {}, `The device would report ${warnings.length} warning(s) for this file. It has been imported the way the device reads it, so check these parts:`),
        h('ul', { class: 'warning-list' }, warnings.map((w) => h('li', {}, w))));
}

/**
 * @param {{ fromGauge?: boolean }} opts fromGauge: the file came from a gauge, so it must keep its id.
 *   Renaming it would make a later upload add a second face rather than update the one it came from.
 */
async function importXml(text, name, { fromGauge = false } = {}) {
    const r = parseGauge(text);
    if (!r.ok) {
        await ask(`Cannot import ${name}`, h('p', {}, `The device would reject this file: ${r.error}.`));
        return;
    }
    const existing = listFaces(app.lib).find((f) => f.model.id === r.model.id);
    if (existing) {
        const choice = await ask(`A face called '${r.model.id}' already exists`,
            h('p', {}, fromGauge
                ? 'Replace the copy in this browser with the gauge\'s? Changes made here and not uploaded are lost.'
                : 'Replace the copy in this browser, or keep both?'),
            fromGauge
                ? [['Cancel', null], ['Replace', 'replace', 'danger']]
                : [['Cancel', null], ['Keep both', 'copy'], ['Replace', 'replace', 'danger']]);
        if (choice === null) return;
        if (choice === 'replace') {
            app.lib.faces[existing.key] = { model: r.model, updated: Date.now() };
            if (app.key === existing.key) app.key = null;
            openFace(existing.key);
        } else {
            addFace(r.model);
        }
    } else {
        addFace(r.model);
    }
    if (r.warnings.length) await ask(`Imported ${name} with warnings`, describeWarnings(r.warnings));
    else toast(`Imported ${r.model.id}.`);
}

function download(filename, text) {
    const url = URL.createObjectURL(new Blob([text], { type: 'application/xml' }));
    const a = h('a', { href: url, download: filename });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}

async function exportCurrent() {
    const v = validateModel(app.model);
    const errors = v.issues.filter((i) => i.severity === 'error');
    if (errors.length) {
        const go = await ask('This face has problems',
            h('div', {}, h('p', {}, `${errors.length} error(s) mean the device would reject the file or change it:`),
                h('ul', { class: 'warning-list' }, errors.slice(0, 8).map((e) => h('li', {}, e.message)))),
            [['Fix first', false], ['Download anyway', true, 'danger']]);
        if (!go) return;
    }
    download(`${app.model.id || 'face'}.xml`, v.xml);
}

async function newFaceDialog() {
    const choice = await ask('New face', h('div', { class: 'choices' },
        h('p', {}, 'Start from one of the faces the gauge ships with, or from scratch.')),
    [['Cancel', null], ['Duplicate current', 'dup'], ['Blank', 'blank'], ...EXAMPLES.map((e) => [e.title, e.file])]);
    if (choice === null) return;
    if (choice === 'dup') {
        const copy = structuredClone(app.model);
        addFace(copy);
        toast(`Duplicated as ${app.model.id}.`);
        return;
    }
    let xml = BLANK_XML;
    if (choice !== 'blank') {
        try {
            xml = await fetchExample(choice);
        } catch (err) {
            toast(`Could not load ${choice}: ${err.message}`, 'error');
            return;
        }
    }
    addFace(parseGauge(xml).model);
}

/* ------------------------------------------------------------------ validation ------ */

function queueValidate() {
    if (app.validateQueued) return;
    app.validateQueued = true;
    requestAnimationFrame(() => {
        app.validateQueued = false;
        app.validation = validateModel(app.model);
        renderIssues();
        renderNav();
        renderXml();
    });
}

function renderIssues() {
    const { issues, device, bytes } = app.validation;
    const counts = { error: 0, warning: 0, info: 0 };
    for (const i of issues) counts[i.severity]++;

    $('tab-issues-count').textContent = counts.error + counts.warning ? String(counts.error + counts.warning) : '';
    $('tab-issues-count').className = `count ${counts.error ? 'error' : counts.warning ? 'warning' : ''}`;

    const status = $('device-verdict');
    if (!device.ok) {
        status.className = 'verdict error';
        status.textContent = `Device would reject this file: ${device.error}`;
    } else if (counts.error) {
        status.className = 'verdict error';
        status.textContent = `${counts.error} error${counts.error > 1 ? 's' : ''} to fix`;
    } else if (counts.warning) {
        status.className = 'verdict warning';
        status.textContent = `Valid · ${counts.warning} warning${counts.warning > 1 ? 's' : ''}`;
    } else {
        status.className = 'verdict ok';
        status.textContent = 'Valid · device reports 0 warnings';
    }
    $('byte-count').textContent = `${bytes.toLocaleString()} / ${LIMITS.fileBytes.toLocaleString()} bytes`;

    const list = $('issues-list');
    list.replaceChildren(...(issues.length === 0
        ? [h('li', { class: 'empty' }, 'No issues. The device will read this face exactly as shown.')]
        : issues.map((i) => h('li', { class: i.severity },
            h('button', {
                type: 'button', onclick: () => {
                    if (i.section === 'file') selectDock('xml');
                    else selectSection(i.section);
                },
            },
            h('span', { class: 'sev' }, i.severity),
            h('span', { class: 'where' }, sectionLabel(i.section)),
            h('span', { class: 'msg' }, i.message))))));
}

function sectionLabel(id) {
    return SECTIONS.find(([s]) => s === id)?.[1] ?? 'File';
}

function renderXml() {
    $('xml-code').textContent = app.validation.xml;
}

/* ------------------------------------------------------------------- chrome --------- */

function renderNav() {
    const counts = {};
    for (const i of app.validation?.issues ?? []) {
        if (i.severity === 'info') continue;
        const c = counts[i.section] ??= { error: 0, warning: 0 };
        c[i.severity]++;
    }
    const nav = $('sections');
    const groups = { face: 'Face', manage: 'Manage' };
    nav.replaceChildren(...Object.entries(groups).map(([g, title]) => h('div', { class: 'nav-group' },
        h('h2', {}, title),
        h('ul', {}, SECTIONS.filter(([, , grp]) => grp === g).map(([id, label]) => {
            const c = counts[id];
            return h('li', {},
                h('button', {
                    type: 'button', class: id === app.section ? 'active' : '', 'aria-current': id === app.section ? 'page' : null,
                    onclick: () => selectSection(id),
                }, label, c ? h('span', { class: `count ${c.error ? 'error' : 'warning'}` }, String(c.error + c.warning)) : null));
        })))));
}

function renderFaceSelect() {
    const sel = $('face-select');
    const faces = listFaces(app.lib);
    sel.replaceChildren(...faces.map((f) => h('option', { value: f.key }, f.key === app.key ? (app.model.id || '(no id)') : (f.model.id || '(no id)'))));
    sel.value = app.key ?? '';
}

function selectSection(id) {
    app.section = id;
    app.lib.view = { ...app.lib.view, section: id };
    history.replaceState(null, '', `#${id}`);
    renderNav();
    renderInspector();
    $('inspector').scrollTop = 0;
    if (window.matchMedia('(max-width: 1100px)').matches) $('inspector').scrollIntoView({ behavior: 'smooth', block: 'start' });
}

function selectDock(tab) {
    for (const t of ['issues', 'xml']) {
        $(`tab-${t}`).setAttribute('aria-selected', String(t === tab));
        $(`pane-${t}`).hidden = t !== tab;
    }
}

/* ---------------------------------------------------------------- inspector --------- */

function renderInspector() {
    const form = new Form(app.commit);
    app.form = form;
    app.editors = [];
    const m = app.model;
    const build = INSPECTORS[app.section] ?? INSPECTORS.gauge;
    $('inspector').replaceChildren(...[build(form, m)].flat());
}

/** Move an item within a list and keep the selection sensible. */
const move = (list, i, d) => app.commit((mm) => {
    const arr = list(mm);
    [arr[i], arr[i + d]] = [arr[i + d], arr[i]];
}, { rebuild: true });
const remove = (list, i) => app.commit((mm) => list(mm).splice(i, 1), { rebuild: true });

const INSPECTORS = {
    gauge(form, m) {
        return [
            sectionHead('Gauge', 'What the face is called and which reading it shows.'),
            form.text('Id', () => m.id, (v) => { m.id = v; }, {
                maxBytes: LIMITS.id, pattern: /[^A-Za-z0-9_-]/g,
                help: 'Letters, digits, _ and -. It is also the file name on the gauge, and uploading replaces a face with the same id.',
            }),
            h('datalist', { id: 'channels' }, ['boost', 'egt'].map((c) => h('option', { value: c }))),
            row(
                form.text('Channel', () => m.source.channel, (v) => { m.source.channel = v; }, { maxBytes: LIMITS.channel, list: 'channels', help: 'The data channel this gauge reads, e.g. boost or egt.' }),
                form.text('Unit', () => m.source.unit, (v) => { m.source.unit = v; }, { maxBytes: LIMITS.unit, help: 'Informational. Put what the readout shows in its suffix.' })),
            row(
                form.number('Min', () => m.source.min, (v) => { m.source.min = v; }, { help: 'Value at the start of the scale.' }),
                form.number('Max', () => m.source.max, (v) => { m.source.max = v; }, { help: 'Must be greater than min.' })),
            form.slider('Damping', () => m.source.damping, (v) => { m.source.damping = Math.fround(v); }, {
                min: 0, max: 1, step: 0.01,
                help: 'Share of the remaining distance the needle keeps back at each sample: 0 tracks instantly, 0.9 is very smooth and slow. Try it with Sweep.',
            }),
            h('h3', { class: 'sub' }, 'Panel'),
            form.checkbox('Declare the panel this face was drawn for', () => !!m.panel,
                (on) => { m.panel = on ? { shape: 'round', width: PANEL.width, height: PANEL.height } : null; },
                { rebuild: true, help: 'Recommended. Lets future firmware scale the face to other panels (planned for M8).' }),
            m.panel ? row(
                form.select('Shape', () => m.panel.shape, (v) => { m.panel.shape = v; }, [['round', 'Round'], ['square', 'Square']]),
                form.number('Width', () => m.panel.width, (v) => { m.panel.width = v; }, { integer: true, unit: 'px' }),
                form.number('Height', () => m.panel.height, (v) => { m.panel.height = v; }, { integer: true, unit: 'px' })) : null,
        ];
    },

    dial(form, m) {
        const presets = [['Classic 270°', 225, 270], ['Top half', 270, 180], ['Right quarter', 0, 90], ['Full circle', 0, 360]];
        return [
            sectionHead('Dial', 'The scale\'s arc and the face behind it. Angles are degrees clockwise from 12 o\'clock.'),
            h('div', { class: 'chips' }, presets.map(([name, start, sweep]) => button(name, () => app.commit((mm) => {
                mm.face.startAngle = start;
                mm.face.sweep = sweep;
            }), { class: 'chip' }))),
            form.slider('Start angle', () => m.face.startAngle, (v) => { m.face.startAngle = v; }, { min: 0, max: 359, step: 1, unit: '°', help: 'Where min sits. 225 is 7:30.' }),
            form.slider('Sweep', () => m.face.sweep, (v) => { m.face.sweep = v; }, { min: 1, max: 360, step: 1, unit: '°', help: 'Clockwise span from min to max.' }),
            form.number('Scale radius', () => m.face.radius, (v) => { m.face.radius = v; }, {
                integer: true, min: 0, max: 233, unit: 'px', empty: 0, placeholder: `fit (${PANEL.defaultRadius})`,
                help: 'Outer edge of the bands and the peak marker. Leave blank to fit the panel with an 8 px margin.',
            }),
            form.color('Background', () => m.face.background, (v) => { m.face.background = v; }),
        ];
    },

    bands(form, m) {
        const list = (mm) => mm.bands;
        return [
            sectionHead('Bands', `Coloured arcs marking ranges of the scale, drawn in order. Up to ${LIMITS.bands}.`),
            m.bands.map((b, i) => itemCard(`Band ${i + 1}`, {
                swatch: b.color,
                onUp: i > 0 ? () => move(list, i, -1) : null,
                onDown: i < m.bands.length - 1 ? () => move(list, i, 1) : null,
                onRemove: () => remove(list, i),
            },
            row(
                form.number('From', () => m.bands[i].from, (v) => { m.bands[i].from = v; }, { unit: m.source.unit }),
                form.number('To', () => m.bands[i].to, (v) => { m.bands[i].to = v; }, { unit: m.source.unit })),
            row(
                form.color('Colour', () => m.bands[i].color, (v) => { m.bands[i].color = v; }),
                form.number('Width', () => m.bands[i].width, (v) => { m.bands[i].width = v; }, { integer: true, min: 0, unit: 'px', help: 'Measured inwards from the scale radius.' })))),
            button('+ Add band', () => app.commit((mm) => {
                const last = mm.bands.at(-1);
                const b = defaultBand();
                b.from = last ? last.to : mm.source.min;
                b.to = mm.source.max;
                b.width = last?.width ?? 18;
                b.color = ['#00c853', '#ffab00', '#d50000', '#2979ff'][mm.bands.length % 4];
                mm.bands.push(b);
            }, { rebuild: true }), { class: 'add', disabled: m.bands.length >= LIMITS.bands }),
        ];
    },

    ticks(form, m) {
        const on = !!m.ticks;
        const head = sectionHead('Ticks', 'Major and minor tick marks, stepped from min. They hang inside the widest band.',
            presenceToggle('Show ticks', on, (v) => app.commit((mm) => { mm.ticks = v ? defaultTicks() : null; }, { rebuild: true })));
        if (!on) return [head];
        const t = () => m.ticks;
        const major = shapeEditor(app, {
            kind: 'major', title: 'Major tick shape', get: () => m.ticks?.majorShape ?? null, set: (s) => { m.ticks.majorShape = s; },
            inherited: () => m.ticks.color,
            help: 'Replaces the length and width above with an outline drawn at 12 o\'clock. The origin sits on the tick circle; positive y points in towards the centre.',
        });
        const minor = shapeEditor(app, {
            kind: 'minor', title: 'Minor tick shape', get: () => m.ticks?.minorShape ?? null, set: (s) => { m.ticks.minorShape = s; },
            inherited: () => m.ticks.color, help: 'As above, for minor ticks. Keep it narrow: it is stamped at every tick.',
        });
        app.editors.push(major, minor);
        return [
            head,
            form.color('Colour', () => t().color, (v) => { t().color = v; }, { help: 'Also inherited by tick shapes.' }),
            h('h3', { class: 'sub' }, 'Major'),
            row(
                form.number('Every', () => t().majorEvery, (v) => { t().majorEvery = v; }, { min: 0, unit: m.source.unit, help: '0 for none.' }),
                form.number('Length', () => t().majorLen, (v) => { t().majorLen = v; }, { integer: true, min: 0, unit: 'px', disabled: !!t().majorShape }),
                form.number('Width', () => t().majorWidth, (v) => { t().majorWidth = v; }, { integer: true, min: 0, unit: 'px', disabled: !!t().majorShape })),
            h('h3', { class: 'sub' }, 'Minor'),
            row(
                form.number('Every', () => t().minorEvery, (v) => { t().minorEvery = v; }, { min: 0, unit: m.source.unit, help: '0 for none.' }),
                form.number('Length', () => t().minorLen, (v) => { t().minorLen = v; }, { integer: true, min: 0, unit: 'px', disabled: !!t().minorShape }),
                form.number('Width', () => t().minorWidth, (v) => { t().minorWidth = v; }, { integer: true, min: 0, unit: 'px', disabled: !!t().minorShape })),
            major.el,
            minor.el,
        ];
    },

    labels(form, m) {
        const on = !!m.labels;
        const head = sectionHead('Labels', 'Numbers around the scale.',
            presenceToggle('Show labels', on, (v) => app.commit((mm) => { mm.labels = v ? defaultLabels() : null; }, { rebuild: true })));
        if (!on) return [head];
        const l = () => m.labels;
        return [
            head,
            row(
                form.number('Every', () => l().every, (v) => { l().every = v; }, {
                    min: 0, unit: m.source.unit, empty: 0, placeholder: () => `major ticks (${m.ticks?.majorEvery ?? 10})`,
                }),
                form.number('Radius', () => l().radius, (v) => { l().radius = v; }, {
                    integer: true, min: 0, unit: 'px', empty: 0, placeholder: () => `auto (${labelRadius(m)})`,
                    help: 'Centre of each label. Auto sits one line inside the major ticks.',
                })),
            row(
                form.font('Font', () => l().font, (v) => { l().font = v; }),
                form.color('Colour', () => l().color, (v) => { l().color = v; })),
            form.format('Format', () => l().format, (v) => { l().format = v; }, { help: 'One %f, %e or %g. The device shows at most 15 characters per label.' }),
        ];
    },

    needle(form, m) {
        const n = () => m.needle;
        const shaped = !!m.needle.shape;
        const needle = shapeEditor(app, {
            kind: 'needle', title: 'Needle shape', get: () => m.needle.shape, set: (s) => { m.needle.shape = s; },
            inherited: () => m.needle.color,
            help: 'Replaces style, length, width and tail. Drawn pointing at 12 o\'clock with the pivot at the origin, so the tip has negative y. It is redrawn every time it moves: keep it within about 20 × 180 px and 3 parts until it has been measured on hardware.',
        });
        const hub = shapeEditor(app, {
            kind: 'hub', title: 'Hub shape', get: () => m.needle.hub, set: (s) => { m.needle.hub = s; },
            inherited: () => m.needle.color, help: 'Replaces the pivot circle. Drawn over the needle, centred on the pivot, and never rotated or flashed.',
        });
        app.editors.push(needle, hub);
        return [
            sectionHead('Needle & hub', 'The pointer, and the cap over its pivot.'),
            form.color('Colour', () => n().color, (v) => { n().color = v; }, { help: 'Flashes to the alert colour while an alert is active. Also inherited by needle and hub shapes.' }),
            shaped ? h('p', { class: 'note' }, 'A custom needle shape is set, so the built-in style settings are not used.') : null,
            row(
                form.select('Style', () => n().style, (v) => { n().style = v; }, [['taper', 'Taper'], ['line', 'Line'], ['arrow', 'Arrow']], { disabled: shaped }),
                form.number('Length', () => n().length, (v) => { n().length = v; }, {
                    integer: true, min: 0, unit: 'px', empty: 0, placeholder: () => `auto (${needleLength(m)})`, disabled: shaped,
                })),
            row(
                form.number('Width', () => n().width, (v) => { n().width = v; }, { integer: true, min: 0, unit: 'px', disabled: shaped }),
                form.number('Tail', () => n().tail, (v) => { n().tail = v; }, { integer: true, min: 0, unit: 'px', disabled: shaped, help: 'Counterweight behind the pivot.' })),
            form.number('Pivot radius', () => n().pivotRadius, (v) => { n().pivotRadius = v; }, {
                integer: true, min: 0, unit: 'px', disabled: !!m.needle.hub, help: m.needle.hub ? 'Not used: a hub shape is set.' : '0 for no hub.',
            }),
            needle.el,
            hub.el,
        ];
    },

    titles(form, m) {
        const list = (mm) => mm.titles;
        return [
            sectionHead('Titles', `Fixed text baked into the face. Up to ${LIMITS.titles}. Drag a title in the preview to move it.`),
            m.titles.map((t, i) => itemCard(`Title ${i + 1}`, {
                onUp: i > 0 ? () => move(list, i, -1) : null,
                onDown: i < m.titles.length - 1 ? () => move(list, i, 1) : null,
                onRemove: () => remove(list, i),
            },
            form.text('Text', () => m.titles[i].text, (v) => { m.titles[i].text = v; }, { maxBytes: LIMITS.text }),
            row(
                form.number('Centre x', () => m.titles[i].x, (v) => { m.titles[i].x = v; }, { integer: true, unit: 'px', empty: null, placeholder: 'centred' }),
                form.number('Centre y', () => m.titles[i].y, (v) => { m.titles[i].y = v; }, { integer: true, unit: 'px' })),
            row(
                form.font('Font', () => m.titles[i].font, (v) => { m.titles[i].font = v; }),
                form.color('Colour', () => m.titles[i].color, (v) => { m.titles[i].color = v; })))),
            button('+ Add title', () => app.commit((mm) => {
                const t = defaultTitle();
                t.text = mm.titles.length ? 'TEXT' : (mm.source.channel || 'TITLE').toUpperCase().slice(0, 31);
                t.y = 150 + mm.titles.length * 30;
                t.color = '#9e9e9e';
                mm.titles.push(t);
            }, { rebuild: true }), { class: 'add', disabled: m.titles.length >= LIMITS.titles }),
        ];
    },

    readout(form, m) {
        const on = !!m.readout;
        const head = sectionHead('Readout', 'The live number. Drag it in the preview to move it.',
            presenceToggle('Show readout', on, (v) => app.commit((mm) => {
                if (v) {
                    mm.readout = defaultReadout();
                    mm.readout.y = 300;
                    mm.readout.suffix = mm.source.unit ? ` ${mm.source.unit}` : '';
                } else {
                    mm.readout = null;
                }
            }, { rebuild: true })));
        if (!on) return [head];
        const o = () => m.readout;
        return [
            head,
            row(
                form.number('Centre x', () => o().x, (v) => { o().x = v; }, { integer: true, unit: 'px', empty: null, placeholder: 'centred' }),
                form.number('Top y', () => o().y, (v) => { o().y = v; }, { integer: true, unit: 'px', help: 'Top edge of the text, unlike titles.' })),
            row(
                form.font('Font', () => o().font, (v) => { o().font = v; }),
                form.color('Colour', () => o().color, (v) => { o().color = v; }, { help: 'Alerts replace it while flashing.' })),
            form.format('Format', () => o().format, (v) => { o().format = v; }),
            row(
                form.text('Prefix', () => o().prefix, (v) => { o().prefix = v; }, { maxBytes: LIMITS.text }),
                form.text('Suffix', () => o().suffix, (v) => { o().suffix = v; }, { maxBytes: LIMITS.text, help: 'Include a leading space, e.g. " psi".' })),
        ];
    },

    peak(form, m) {
        const on = !!m.peak;
        const head = sectionHead('Peak hold', 'A marker that stays at the highest raw reading, with an optional value. Tapping the dial clears it.',
            presenceToggle('Peak hold on', on, (v) => app.commit((mm) => { mm.peak = v ? defaultPeak() : null; }, { rebuild: true })));
        if (!on) return [head];
        const k = () => m.peak;
        return [
            head,
            row(
                form.color('Colour', () => k().color, (v) => { k().color = v; }),
                form.number('Length', () => k().length, (v) => { k().length = v; }, { integer: true, min: 0, unit: 'px', help: 'Inwards from the scale radius.' }),
                form.number('Width', () => k().width, (v) => { k().width = v; }, { integer: true, min: 0, unit: 'px' })),
            form.checkbox('Show the peak value', () => k().showValue, (v) => { k().showValue = v; }, { rebuild: true }),
            k().showValue ? [
                row(
                    form.number('Top y', () => k().valueY, (v) => { k().valueY = v; }, {
                        integer: true, unit: 'px', empty: null,
                        placeholder: () => `below readout (${m.readout ? m.readout.y + lineHeight(m.readout.font) + 2 : PANEL.cy + 60})`,
                        help: 'Always centred horizontally.',
                    }),
                    form.font('Font', () => k().font, (v) => { k().font = v; })),
                row(
                    form.format('Format', () => k().format, (v) => { k().format = v; }),
                    form.text('Prefix', () => k().prefix, (v) => { k().prefix = v; }, { maxBytes: LIMITS.text })),
            ] : null,
        ];
    },

    alerts(form, m) {
        const list = (mm) => mm.alerts;
        return [
            sectionHead('Alerts', `Thresholds that recolour the readout and needle. The first alert that holds wins. Up to ${LIMITS.alerts}. They compare the damped value.`),
            m.alerts.map((a, i) => itemCard(`Alert ${i + 1}`, {
                swatch: a.color,
                onUp: i > 0 ? () => move(list, i, -1) : null,
                onDown: i < m.alerts.length - 1 ? () => move(list, i, 1) : null,
                onRemove: () => remove(list, i),
            },
            row(
                form.number('Above', () => m.alerts[i].above, (v) => { m.alerts[i].above = v; }, { empty: null, placeholder: 'none', unit: m.source.unit, help: 'Fires while strictly greater.' }),
                form.number('Below', () => m.alerts[i].below, (v) => { m.alerts[i].below = v; }, { empty: null, placeholder: 'none', unit: m.source.unit, help: 'Fires while strictly less.' })),
            row(
                form.color('Colour', () => m.alerts[i].color, (v) => { m.alerts[i].color = v; }),
                form.number('Flash rate', () => m.alerts[i].flashHz, (v) => { m.alerts[i].flashHz = v; }, { min: 0, max: 20, step: 0.5, unit: 'Hz', help: '0 for a steady colour.' })),
            form.checkbox('Chime on entry', () => m.alerts[i].chime, (v) => { m.alerts[i].chime = v; }, { help: 'Only if chimes are enabled on the gauge.' }))),
            button('+ Add alert', () => app.commit((mm) => {
                const a = defaultAlert();
                const span = mm.source.max - mm.source.min;
                a.above = Math.round((mm.source.min + span * 0.85) * 100) / 100;
                mm.alerts.push(a);
            }, { rebuild: true }), { class: 'add', disabled: m.alerts.length >= LIMITS.alerts }),
        ];
    },

    library() {
        const faces = listFaces(app.lib);
        return [
            sectionHead('My faces', 'Faces are kept in this browser only. The gauge cannot send a face back, so download anything you want to keep safe.'),
            h('div', { class: 'toolbar' },
                button('New…', newFaceDialog, { class: 'primary' }),
                button('Import XML…', () => $('file-input').click())),
            h('ul', { class: 'library' }, faces.map((f, i) => {
                const current = f.key === app.key;
                const model = current ? app.model : f.model;
                const thumb = h('div', { class: 'thumb' });
                try {
                    thumb.innerHTML = renderStaticSvg(model, model.source.min + (model.source.max - model.source.min) * 0.6, { prefix: `lib${i}`, size: 88 });
                } catch {
                    thumb.textContent = '?';
                }
                return h('li', { class: current ? 'current' : '' },
                    thumb,
                    h('div', { class: 'lib-meta' },
                        h('strong', {}, model.id || '(no id)'),
                        h('span', {}, `${model.source.channel || '—'} · ${model.source.min}–${model.source.max} ${model.source.unit}`),
                        h('span', { class: 'muted' }, current ? 'Open now' : `Edited ${new Date(f.updated).toLocaleString()}`)),
                    h('div', { class: 'lib-actions' },
                        current ? null : button('Open', () => openFace(f.key)),
                        button('Download', () => download(`${model.id || 'face'}.xml`, validateModel(model).xml)),
                        button('Delete', async () => {
                            const ok = await ask(`Delete ${model.id || 'this face'}?`, h('p', {}, 'It is removed from this browser. This cannot be undone.'), [['Cancel', false], ['Delete', true, 'danger']]);
                            if (!ok) return;
                            delete app.lib.faces[f.key];
                            if (current) {
                                const next = listFaces(app.lib)[0];
                                app.key = null;
                                if (next) openFace(next.key);
                                else addFace(parseGauge(BLANK_XML).model);
                            }
                            saveLibrary(app.lib);
                            renderFaceSelect();
                            renderInspector();
                        }, { class: 'danger' })));
            })),
        ];
    },

    device() {
        // Served by a gauge, requests go to it at the page's own origin ('' as the host).
        const gauge = app.gauge;
        const blocked = pageBlockReason();
        // Kept across re-renders: opening a face re-renders this section.
        const dev = app.ui.device ??= {
            log: h('div', { class: 'device-log', 'aria-live': 'polite' }),
            inventory: null,
            // Every request to the gauge runs in turn. Overlapping ones let an older face list land
            // after a newer one, showing a face that was just deleted or hiding one just added.
            queue: Promise.resolve(),
            refreshQueued: false,
        };
        const host = h('input', { type: 'text', placeholder: 'ai-gauge-1a2b.local or 192.168.1.50', spellcheck: 'false', value: app.lib.deviceHost ?? '' });
        const facesBox = h('div', { class: 'gauge-faces' });
        const actionBox = h('div', { class: 'toolbar' });

        const report = (title, body, kind = 'info') => dev.log.prepend(h('div', { class: `log-entry ${kind}` }, h('strong', {}, title), body ? h('div', {}, body) : null));
        const run = (label, fn) => () => {
            if (blocked) {
                report(label, blocked, 'error');
                return dev.queue;
            }
            if (!gauge && !host.value.trim()) {
                report(label, 'Enter the gauge\'s address first. It is shown on the gauge\'s settings page.', 'warning');
                return dev.queue;
            }
            const addr = gauge ? '' : host.value.trim();
            dev.queue = dev.queue.then(async () => {
                try {
                    await fn(addr);
                } catch (err) {
                    report(`${label} failed`, err instanceof DeviceError ? err.message : String(err), 'error');
                }
            });
            return dev.queue;
        };

        const describe = (s) => `${s.channel}, ${s.min}–${s.max} ${s.unit}${s.warnings ? ` · ${s.warnings} warning(s)` : ''}`;
        // replaceChildren() with h()'s rules: it would otherwise print null as text and not flatten arrays.
        const fill = (el, ...children) => el.replaceChildren(...children.flat(Infinity).filter((c) => c !== null && c !== undefined && c !== false));

        const renderActions = () => {
            const id = app.model.id;
            const plan = dev.inventory ? uploadPlan(dev.inventory, id) : null;
            fill(actionBox,
                button(plan === 'update' ? `Update '${id}' on gauge` : plan === 'add' ? `Add '${id}' to gauge` : `Upload '${id}'`, upload,
                    { class: 'primary', disabled: plan === 'full' }),
                plan === 'full' ? h('span', { class: 'face-meta' }, `The gauge is full (${dev.inventory.max} faces). Delete one to add this face.`) : null);
        };

        const renderFaces = () => {
            renderActions();
            if (!dev.inventory) {
                fill(facesBox);
                return;
            }
            const { faces, active, max } = dev.inventory;
            fill(facesBox,
                h('div', { class: 'face-rows-head' },
                    h('strong', {}, `${faces.length} of ${max} faces on the gauge`),
                    button('Refresh', refresh)),
                faces.length ? null : h('p', { class: 'face-meta' }, 'None stored, so the gauge is showing its built-in face.'),
                faces.map((f) => h('div', { class: 'face-row' },
                    h('div', { class: 'face-name' },
                        h('strong', {}, f.id),
                        f.id === active ? h('span', { class: 'face-badge on' }, 'On screen') : null,
                        f.id === app.model.id ? h('span', { class: 'face-badge' }, 'Open here') : null,
                        h('div', { class: 'face-meta' }, f.error ? `The gauge cannot load it: ${f.error}` : describe(f.summary))),
                    h('div', { class: 'face-actions' },
                        button('Edit', run(`Open '${f.id}'`, async (addr) => {
                            await importXml(await getFaceXml(addr, f.id), `${f.id}.xml`, { fromGauge: true });
                        })),
                        button('Delete', run(`Delete '${f.id}'`, (addr) => removeFace(addr, f.id)), { class: 'danger' })))));
        };

        const readGauge = run('Read the gauge', async (addr) => {
            dev.refreshQueued = false;
            try {
                dev.inventory = await getFaceInventory(addr);
            } catch (err) {
                dev.inventory = null;
                throw err;
            } finally {
                renderFaces();
            }
        });
        // A refresh already waiting in the queue will read the gauge's latest state; another adds nothing.
        const refresh = () => {
            if (dev.refreshQueued) return dev.queue;
            dev.refreshQueued = true;
            return readGauge();
        };

        const upload = run('Upload', async (addr) => {
            const id = app.model.id;
            const v = validateModel(app.model);
            if (v.issues.some((i) => i.severity === 'error')) {
                report('Not uploaded', 'Fix the errors in the Issues panel first. The gauge would reject or change this face.', 'warning');
                return;
            }
            // Decide on the gauge's current state, not on a list read earlier.
            dev.inventory = await getFaceInventory(addr);
            renderFaces();
            const plan = uploadPlan(dev.inventory, id);
            if (plan === 'full') {
                report('Not uploaded', `The gauge is full (${dev.inventory.max} faces). Delete one to add '${id}', or change this face's id to one already on the gauge to replace it.`, 'warning');
                return;
            }
            const onScreen = dev.inventory.active === id;
            if (plan === 'update') {
                const ok = await ask(`Replace '${id}' on the gauge?`,
                    h('p', {}, `The gauge's copy is overwritten with this one.${onScreen ? ' It is on screen, so the gauge redraws it straight away.' : ''}`),
                    [['Cancel', false], ['Replace', true, 'danger']]);
                if (!ok) return;
            }
            const summary = await uploadFace(addr, id, v.xml);
            const what = plan === 'update' ? 'Updated' : 'Added';
            if (summary.warnings) {
                report(`${what}, with warnings`, `The gauge reports ${summary.warnings} warning(s), but this editor predicted none. Please report this as a bug: the editor and firmware disagree.`, 'warning');
            } else {
                report(`${what} '${id}'`, `${describe(summary)}. 0 warnings. ${onScreen ? 'The gauge has redrawn it.' : 'Pick it in the gauge\'s settings to show it.'}`, 'ok');
            }
            dev.inventory = await getFaceInventory(addr);
            renderFaces();
        });

        const removeFace = async (addr, id) => {
            const inv = dev.inventory;
            const onScreen = inv?.active === id;
            const lastOne = (inv?.faces.length ?? 0) <= 1;
            const ok = await ask(`Delete '${id}' from the gauge?`, h('div', {},
                h('p', {}, 'Faces kept in this browser are not affected.'),
                onScreen ? h('p', {}, lastOne
                    ? 'It is on screen and is the only face stored, so the gauge switches to its built-in face.'
                    : 'It is on screen, so the gauge switches to another of its faces.') : null),
            [['Cancel', false], ['Delete', true, 'danger']]);
            if (!ok) return;
            await deleteDeviceFace(addr, id);
            dev.inventory = await getFaceInventory(addr);
            renderFaces();
            const now = dev.inventory.active;
            report(`Deleted '${id}'`, onScreen ? `The gauge now shows ${now ? `'${now}'` : 'its built-in face'}.` : null, 'ok');
        };

        // What this page can and cannot do, and the way round it when it cannot.
        const curl = h('code', {}, `curl -X PUT --data-binary @${app.model.id || 'face'}.xml http://<gauge>/api/config/${app.model.id || '<id>'}`);
        let callout;
        if (gauge) {
            callout = h('div', { class: 'callout ok' },
                h('strong', {}, `Connected to ${gauge.wifi.hostname}`),
                h('p', {}, `This editor was served by the gauge, firmware ${gauge.version}, so it can upload to it directly.`));
        } else if (blocked) {
            const link = h('a', { href: '#', target: '_blank', rel: 'noopener' });
            const syncLink = () => {
                let url = '';
                try {
                    url = host.value.trim() ? gaugeEditorUrl(host.value) : '';
                } catch {
                    // Not a valid address yet.
                }
                link.textContent = url || 'http://<gauge>/editor/';
                link.href = url || '#';
            };
            syncLink();
            host.addEventListener('input', syncLink);
            callout = h('div', { class: 'callout warning' },
                h('strong', {}, 'Open the editor from your gauge to upload'),
                h('p', {}, blocked),
                h('p', {}, 'Enter its address and open ', link, '. Faces kept in this browser do not carry over, so download this one first and import it there.'),
                h('p', {}, 'Or upload the downloaded file with ', curl, '.'));
        } else {
            callout = h('div', { class: 'callout warning' },
                h('strong', {}, 'Connecting from this page'),
                h('p', {}, 'The gauge answers other pages only when they are served from http://localhost, like the development server. From anywhere else, open the editor on the gauge itself at http://<gauge>/editor/.'));
        }
        host.addEventListener('input', () => { app.lib.deviceHost = host.value.trim(); saveLibrary(app.lib); });

        renderFaces();
        // On a gauge, always show its current faces; elsewhere, wait for an address and Connect.
        if (gauge) refresh();

        return [
            sectionHead('Device', 'Add, update and remove the faces stored on a gauge.'),
            callout,
            gauge ? null : h('div', { class: 'field wide' }, h('label', { class: 'field-label' }, 'Gauge address'), host),
            gauge || blocked ? null : h('div', { class: 'toolbar' }, button('Connect', refresh)),
            blocked ? null : h('h3', { class: 'device-heading' }, 'The face open here'),
            blocked ? null : actionBox,
            blocked ? null : facesBox,
            dev.log,
        ];
    },
};

/* ------------------------------------------------------------------ preview --------- */

const layers = {};

function setupPreview() {
    for (const id of ['face', 'peak', 'needle', 'hub', 'readout', 'peaktext', 'guides']) layers[id] = $(`layer-${id}`);

    const slider = $('value-slider');
    const number = $('value-number');
    const setValue = (v) => {
        app.input.value = v;
        app.input.sweep = false;
        $('btn-sweep').setAttribute('aria-pressed', 'false');
        syncValueControls();
    };
    slider.addEventListener('input', () => setValue(Number(slider.value)));
    number.addEventListener('input', () => Number.isFinite(Number(number.value)) && number.value !== '' && setValue(Number(number.value)));
    $('btn-sweep').addEventListener('click', () => {
        app.input.sweep = !app.input.sweep;
        app.input.sweepStart = performance.now() - sweepPhaseFor(app.input.value) * 6000;
        $('btn-sweep').setAttribute('aria-pressed', String(app.input.sweep));
    });
    $('btn-peak-reset').addEventListener('click', () => resetPeak(app.sim));
    $('toggle-guides').addEventListener('change', (e) => {
        app.showGuides = e.target.checked;
        app.faceDirty = true;
    });

    // Drag titles, the readout and the peak value; click anything else to open its section.
    const svg = $('preview');
    let drag = null;
    const toSvg = (e) => {
        const p = svg.createSVGPoint();
        p.x = e.clientX;
        p.y = e.clientY;
        const q = p.matrixTransform(svg.getScreenCTM().inverse());
        return [q.x, q.y];
    };
    svg.addEventListener('pointerdown', (e) => {
        const el = e.target.closest('[data-section]');
        if (!el) return;
        const [x, y] = toSvg(e);
        const kind = el.dataset.drag;
        const index = Number(el.dataset.index ?? 0);
        const m = app.model;
        const start = kind === 'title' ? [m.titles[index].x ?? PANEL.cx, m.titles[index].y]
            : kind === 'readout' ? [m.readout.x ?? PANEL.cx, m.readout.y]
                : kind === 'peak' ? [PANEL.cx, peakTop(m)] : null;
        drag = { el, kind, index, x, y, start, moved: false, key: `preview-drag-${Date.now()}` };
        if (kind) svg.setPointerCapture(e.pointerId);
    });
    svg.addEventListener('pointermove', (e) => {
        if (!drag?.kind) return;
        const [x, y] = toSvg(e);
        const dx = Math.round(x - drag.x), dy = Math.round(y - drag.y);
        if (!drag.moved && Math.hypot(dx, dy) < 3) return;
        drag.moved = true;
        const nx = drag.start[0] + dx, ny = drag.start[1] + dy;
        const snapX = Math.abs(nx - PANEL.cx) <= 4 ? null : nx; // snap to centred
        app.commit((m) => {
            if (drag.kind === 'title') { m.titles[drag.index].x = snapX; m.titles[drag.index].y = ny; }
            else if (drag.kind === 'readout') { m.readout.x = snapX; m.readout.y = ny; }
            else if (drag.kind === 'peak') m.peak.valueY = ny;
        }, { coalesce: drag.key });
    });
    svg.addEventListener('pointerup', () => {
        if (drag && !drag.moved) {
            const section = drag.el.dataset.section;
            if (section !== app.section) selectSection(section);
        }
        drag = null;
    });

    requestAnimationFrame(frame);
}

function peakTop(m) {
    return m.peak.valueY ?? (m.readout ? m.readout.y + lineHeight(m.readout.font) + 2 : PANEL.cy + 60);
}

function sweepPhaseFor(value) {
    const { min, max } = app.model.source;
    const f = max > min ? Math.min(1, Math.max(0, (value - min) / (max - min))) : 0;
    return Math.acos(1 - 2 * f) / (2 * Math.PI);
}

function syncValueControls() {
    const { min, max } = app.model.source;
    const slider = $('value-slider');
    slider.min = String(min);
    slider.max = String(max);
    slider.step = String((max - min) / 1000 || 'any');
    slider.value = String(app.input.value);
    if (document.activeElement !== $('value-number')) $('value-number').value = String(Math.round(app.input.value * 100) / 100);
}

let lastFrame = performance.now();
let lastDynamic = '';

function frame(now) {
    const dt = Math.min(100, now - lastFrame);
    lastFrame = now;
    const m = app.model;

    if (m) {
        if (app.faceDirty) {
            try {
                layers.face.innerHTML = renderFace(m, 'pv');
                layers.hub.innerHTML = renderHub(m);
                layers.guides.innerHTML = app.showGuides ? renderGuides(m) : '';
            } catch (err) {
                console.error(err);
            }
            app.faceDirty = false;
            lastDynamic = '';
            syncValueControls();
        }

        const { min, max } = m.source;
        if (app.input.sweep && max > min) {
            const t = ((now - app.input.sweepStart) / 6000) % 1;
            app.input.value = min + (max - min) * (0.5 - 0.5 * Math.cos(t * 2 * Math.PI));
            syncValueControls();
        }

        stepSim(m, app.sim, app.input.value, dt);
        const st = app.sim;
        const deg = valueToDeg(m, st.displayed);
        const dynamic = `${deg.toFixed(3)}|${st.peak}|${needleAlertColor(m, st)}|${readoutColor(m, st)}|${st.displayed}`;
        if (dynamic !== lastDynamic) {
            try {
                layers.needle.innerHTML = renderNeedle(m, deg, needleAlertColor(m, st));
                layers.peak.innerHTML = renderPeakMarker(m, st.peak);
                layers.readout.innerHTML = renderReadout(m, st.displayed, readoutColor(m, st));
                layers.peaktext.innerHTML = renderPeakText(m, st.peak);
            } catch (err) {
                console.error(err);
            }
            lastDynamic = dynamic;
            $('sim-state').textContent = st.alertActive
                ? `Alert ${st.activeIdx + 1} active${m.alerts[st.activeIdx].chime ? ' · chime' : ''}`
                : '';
        }
    }
    requestAnimationFrame(frame);
}

/* --------------------------------------------------------------------- start -------- */

async function start() {
    app.lib = loadLibrary();
    // Before the first render: the Device section and the default face depend on it.
    app.gauge = await detectGauge();
    // #needle opens a section; ?example=boost_custom.xml opens a shipped face. Both are linkable.
    const linked = location.hash.slice(1);
    const known = (id) => SECTIONS.some(([s]) => s === id);
    app.section = known(linked) ? linked : known(app.lib.view?.section) ? app.lib.view.section : 'gauge';
    const example = new URLSearchParams(location.search).get('example');

    $('btn-new').addEventListener('click', newFaceDialog);
    $('btn-import').addEventListener('click', () => $('file-input').click());
    $('file-input').addEventListener('change', async (e) => {
        for (const file of e.target.files) await importXml(await file.text(), file.name);
        e.target.value = '';
    });
    $('btn-export').addEventListener('click', exportCurrent);
    $('btn-undo').addEventListener('click', () => undoRedo(app.undo, app.redo));
    $('btn-redo').addEventListener('click', () => undoRedo(app.redo, app.undo));
    $('face-select').addEventListener('change', (e) => openFace(e.target.value));
    $('btn-copy-xml').addEventListener('click', async () => {
        try {
            await navigator.clipboard.writeText(app.validation.xml);
            toast('XML copied.');
        } catch {
            toast('The browser did not allow copying; select the text instead.', 'warning');
        }
    });
    for (const t of ['issues', 'xml']) $(`tab-${t}`).addEventListener('click', () => selectDock(t));

    document.addEventListener('keydown', (e) => {
        const typing = e.target.closest('input, textarea, select');
        if (!(e.ctrlKey || e.metaKey) || typing) return;
        const k = e.key.toLowerCase();
        if (k === 'z' && !e.shiftKey) { e.preventDefault(); undoRedo(app.undo, app.redo); }
        else if ((k === 'z' && e.shiftKey) || k === 'y') { e.preventDefault(); undoRedo(app.redo, app.undo); }
        else if (k === 's') { e.preventDefault(); exportCurrent(); }
    });

    // Drop .xml files anywhere to import them.
    document.addEventListener('dragover', (e) => { e.preventDefault(); document.body.classList.add('dropping'); });
    document.addEventListener('dragleave', (e) => { if (!e.relatedTarget) document.body.classList.remove('dropping'); });
    document.addEventListener('drop', async (e) => {
        e.preventDefault();
        document.body.classList.remove('dropping');
        for (const file of e.dataTransfer.files) {
            if (/\.xml$/i.test(file.name) || file.type.includes('xml')) await importXml(await file.text(), file.name);
            else toast(`${file.name} is not an XML file.`, 'warning');
        }
    });
    window.addEventListener('beforeunload', saveNow);

    const faces = listFaces(app.lib);
    const current = app.lib.current && app.lib.faces[app.lib.current] ? app.lib.current : faces[0]?.key;
    let opened = false;
    if (example && EXAMPLES.some((e) => e.file === example)) {
        try {
            const r = parseGauge(await fetchExample(example));
            const existing = faces.find((f) => f.model.id === r.model.id);
            if (existing) openFace(existing.key);
            else addFace(r.model);
            opened = true;
        } catch {
            toast(`Could not load the example ${example}.`, 'error');
        }
        // Drop the query so a reload does not open it again.
        history.replaceState(null, '', location.pathname + location.hash);
    }
    if (opened) {
        // The linked example is open.
    } else if (current) {
        openFace(current);
    } else {
        let xml = BLANK_XML;
        try {
            xml = await fetchExample('boost.xml');
        } catch {
            // Offline or served without examples: the blank face will do.
        }
        addFace(parseGauge(xml).model);
    }
    setupPreview();
    renderNav();
}

start();
