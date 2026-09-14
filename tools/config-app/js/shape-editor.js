/*
 * Editor for one custom-shape slot: <major-shape>, <minor-shape>, the needle <shape> or the <hub>
 * (docs/gauge-xml-interface.md §4).
 *
 * Shapes are drawn at 12 o'clock around their origin, in SVG coordinates. The canvas shows exactly
 * that frame, with context: for ticks the tick circle and the neighbouring ticks, so crowding is
 * visible; for the needle the tested footprint; for the hub the needle it sits on.
 */

import {
    builtinNeedleParts, builtinTickPolygon, flattenPath, polygonSelfIntersects, quantize, scaleRadius,
    shapeBounds, shapeVertexCount, simplifyToBudget, tickRadius,
} from './geometry.js';
import { shapeParts } from './render.js';
import { LIMITS, NEEDLE_SHAPE_GUIDE } from './schema.js';
import { formatNum } from './serialize.js';
import { button, h, toast } from './ui.js';

const SVG = 'http://www.w3.org/2000/svg';

function presets(kind, m) {
    const t = m.ticks;
    if (kind === 'major' || kind === 'minor') {
        const len = kind === 'major' ? t.majorLen : t.minorLen;
        const w = kind === 'major' ? t.majorWidth : t.minorWidth;
        return [
            ['Built-in rectangle', () => [{ kind: 'polygon', points: builtinTickPolygon(len, w), color: null }]],
            ['Tapered', () => [{ kind: 'polygon', points: [[-w, 0], [w, 0], [w * 0.4, len], [-w * 0.4, len]], color: null }]],
            ['Wedge', () => [{ kind: 'polygon', points: [[-w, 0], [w, 0], [0, len]], color: null }]],
            ['Dot', () => [{ kind: 'circle', cx: 0, cy: quantize(Math.max(w, len / 3)), r: quantize(Math.max(1, w * 0.75)), color: null }]],
        ];
    }
    if (kind === 'needle') {
        return [
            ['Built-in style', () => builtinNeedleParts(m)],
            ['Sword with stripe', () => [
                { kind: 'polygon', points: [[0, -176], [3, -150], [7, -20], [9, 0], [6, 38], [-6, 38], [-9, 0], [-7, -20], [-3, -150]], color: null },
                { kind: 'polygon', points: [[-1, -150], [1, -150], [1.5, -30], [-1.5, -30]], color: '#ffffff' },
            ]],
            ['Slim with counterweight', () => [
                { kind: 'polygon', points: [[0, -180], [2.5, -20], [2.5, 20], [-2.5, 20], [-2.5, -20]], color: null },
                { kind: 'circle', cx: 0, cy: 30, r: 9, color: null },
            ]],
        ];
    }
    return [
        ['Built-in circle', () => [{ kind: 'circle', cx: 0, cy: 0, r: m.needle.pivotRadius || 16, color: null }]],
        ['Ring', () => [
            { kind: 'circle', cx: 0, cy: 0, r: 20, color: null },
            { kind: 'circle', cx: 0, cy: 0, r: 13, color: '#000000' },
        ]],
        ['Cap with centre dot', () => [
            { kind: 'circle', cx: 0, cy: 0, r: 22, color: '#303030' },
            { kind: 'circle', cx: 0, cy: 0, r: 8, color: null },
        ]],
    ];
}

/** Parse SVG `points` text the lenient way an editor field should: numbers in pairs. */
function parsePointsText(s) {
    const nums = s.trim() === '' ? [] : s.trim().split(/[\s,]+/).map(Number);
    if (nums.some((v) => !Number.isFinite(v))) return { error: 'Only numbers, separated by spaces or commas.' };
    if (nums.length % 2 !== 0) return { error: 'Coordinates come in x,y pairs; there is an odd number.' };
    const points = [];
    for (let i = 0; i < nums.length; i += 2) points.push([quantize(nums[i]), quantize(nums[i + 1])]);
    return { points };
}

const pointsText = (points) => points.map(([x, y]) => `${formatNum(x)},${formatNum(y)}`).join(' ');

/**
 * @param app  the editor app: model, commit(), ui state
 * @param spec { kind: 'major'|'minor'|'needle'|'hub', title, get(): shape|null, set(shape|null), inherited(): colour }
 */
export function shapeEditor(app, spec) {
    const m = app.model;
    const shape = spec.get();
    const state = app.ui.shapes[spec.kind] ??= { part: 0, vertex: null, snap: 0.5 };
    if (shape && state.part >= shape.parts.length) state.part = Math.max(0, shape.parts.length - 1);

    const commitShape = (mutate, opts) => app.commit(() => mutate(spec.get()), opts);
    const structural = (mutate) => commitShape(mutate, { rebuild: true });

    const toggle = h('input', { type: 'checkbox', role: 'switch' });
    toggle.checked = !!shape;
    toggle.addEventListener('change', () => {
        app.commit(() => spec.set(toggle.checked ? { color: null, parts: presets(spec.kind, m)[0][1]() } : null), { rebuild: true });
    });

    const wrap = h('section', { class: 'shape-editor' },
        h('div', { class: 'shape-head' },
            h('h3', {}, spec.title),
            h('label', { class: 'switch' }, toggle, h('span', {}, 'Custom shape'))),
        h('p', { class: 'help' }, spec.help));

    if (!shape) return { el: wrap, refresh() {} };

    /* --- toolbar --- */
    const presetSel = h('select', { 'aria-label': 'Replace with a preset' },
        h('option', { value: '' }, 'Replace with preset…'),
        presets(spec.kind, m).map(([name], i) => h('option', { value: String(i) }, name)));
    presetSel.addEventListener('change', () => {
        const p = presets(spec.kind, m)[Number(presetSel.value)];
        if (p) structural((s) => { s.parts = p[1](); state.part = 0; state.vertex = null; });
    });
    const snapSel = h('select', { 'aria-label': 'Snap dragged points to' },
        [[1, 'Snap 1 px'], [0.5, 'Snap ½ px'], [0.0625, 'Snap 1/16 px']].map(([v, t]) => h('option', { value: String(v) }, t)));
    snapSel.value = String(state.snap);
    snapSel.addEventListener('change', () => { state.snap = Number(snapSel.value); });

    const full = shape.parts.length >= LIMITS.shapeParts;
    const toolbar = h('div', { class: 'toolbar' },
        button('+ Polygon', () => structural((s) => {
            const b = shapeBounds(s);
            const cy = b ? (b.y1 + b.y2) / 2 : 0;
            s.parts.push({ kind: 'polygon', points: [[-4, cy - 4], [4, cy - 4], [4, cy + 4], [-4, cy + 4]], color: null });
            state.part = s.parts.length - 1;
        }), { disabled: full, title: full ? `A slot holds at most ${LIMITS.shapeParts} parts` : 'Add a polygon part' }),
        button('+ Circle', () => structural((s) => {
            s.parts.push({ kind: 'circle', cx: 0, cy: 0, r: 4, color: null });
            state.part = s.parts.length - 1;
        }), { disabled: full, title: 'Add a circle part (uses no vertex budget)' }),
        presetSel, snapSel);

    /* --- canvas --- */
    const svg = document.createElementNS(SVG, 'svg');
    svg.setAttribute('class', 'shape-canvas');
    svg.setAttribute('role', 'img');
    svg.setAttribute('aria-label', `${spec.title} editing canvas`);
    let view = null;
    let drag = null;

    const meter = h('p', { class: 'meter' });

    function computeView(s) {
        const b = shapeBounds(s) ?? { x1: -10, y1: -10, x2: 10, y2: 10 };
        let { x1, y1, x2, y2 } = b;
        x1 = Math.min(x1, 0); x2 = Math.max(x2, 0); y1 = Math.min(y1, 0); y2 = Math.max(y2, 0);
        if (spec.kind === 'needle') { x1 = Math.min(x1, -NEEDLE_SHAPE_GUIDE.maxWidth); x2 = Math.max(x2, NEEDLE_SHAPE_GUIDE.maxWidth); }
        const w = Math.max(x2 - x1, 8), hgt = Math.max(y2 - y1, 8);
        const pad = Math.max(w, hgt) * 0.18 + 4;
        const cxv = (x1 + x2) / 2;
        const half = Math.max(w / 2, (hgt * 0.75) / 2) + pad;
        // The canvas is 4:3 tall; widen the view to match.
        const vw = half * 2, vh = Math.max(hgt + pad * 2, vw * 0.75);
        return { x: cxv - vw / 2, y: (y1 + y2) / 2 - vh / 2, w: vw, h: vh };
    }

    function contextLayer(s) {
        const unit = view.w / 300; // one screen-ish px
        let out = '';
        if (spec.kind === 'major' || spec.kind === 'minor') {
            const R = tickRadius(m);
            const every = spec.kind === 'major' ? m.ticks.majorEvery : m.ticks.minorEvery;
            const span = m.source.max - m.source.min;
            out += `<circle cx="0" cy="${R}" r="${R}" fill="none" stroke="#ffd54f" stroke-width="${unit}" stroke-dasharray="${unit * 4} ${unit * 3}" opacity="0.7"/>`;
            if (every > 0 && span > 0 && R > 0) {
                const step = (m.face.sweep * every) / span;
                for (const k of [-2, -1, 1, 2]) {
                    out += `<g transform="translate(0 ${R}) rotate(${step * k}) translate(0 ${-R})" opacity="0.28">${shapeParts(s, spec.inherited())}</g>`;
                }
            }
            out += `<text x="${view.x + unit * 6}" y="${view.y + unit * 14}" class="canvas-note" font-size="${unit * 11}">↑ rim · ↓ centre</text>`;
        } else if (spec.kind === 'needle') {
            const { maxWidth: w, maxLength: L } = NEEDLE_SHAPE_GUIDE;
            out += `<rect x="${-w / 2}" y="${-L}" width="${w}" height="${L}" fill="none" stroke="#ff8a80" stroke-width="${unit}" stroke-dasharray="${unit * 5} ${unit * 4}"><title>Tested needle footprint</title></rect>`;
            const R = scaleRadius(m);
            out += `<line x1="${-w * 1.5}" x2="${w * 1.5}" y1="${-R}" y2="${-R}" stroke="#00b8d4" stroke-width="${unit}" stroke-dasharray="${unit * 2} ${unit * 3}"/>`;
            out += `<text x="${view.x + unit * 6}" y="${view.y + unit * 14}" class="canvas-note" font-size="${unit * 11}">↑ tip · origin = pivot</text>`;
        } else {
            const needle = m.needle.shape ? { color: m.needle.shape.color, parts: m.needle.shape.parts } : { color: null, parts: builtinNeedleParts(m) };
            out += `<g opacity="0.3">${shapeParts(needle, m.needle.color)}</g>`;
        }
        return out;
    }

    function draw() {
        const s = spec.get();
        if (!s) return;
        if (!drag) view = computeView(s);
        svg.setAttribute('viewBox', `${view.x} ${view.y} ${view.w} ${view.h}`);
        const unit = view.w / 300;
        const sel = s.parts[state.part];

        let grid = '';
        const gridStep = view.w > 120 ? 10 : view.w > 40 ? 5 : 1;
        for (let x = Math.ceil(view.x / gridStep) * gridStep; x < view.x + view.w; x += gridStep) {
            grid += `<line x1="${x}" x2="${x}" y1="${view.y}" y2="${view.y + view.h}" class="${x === 0 ? 'axis' : 'grid'}" stroke-width="${unit}"/>`;
        }
        for (let y = Math.ceil(view.y / gridStep) * gridStep; y < view.y + view.h; y += gridStep) {
            grid += `<line y1="${y}" y2="${y}" x1="${view.x}" x2="${view.x + view.w}" class="${y === 0 ? 'axis' : 'grid'}" stroke-width="${unit}"/>`;
        }

        let parts = '';
        s.parts.forEach((p, i) => {
            const one = shapeParts({ color: s.color, parts: [p] }, spec.inherited());
            parts += `<g data-part="${i}" class="part${i === state.part ? ' selected' : ''}">${one}</g>`;
        });

        let handles = '';
        if (sel?.kind === 'polygon') {
            const bad = sel.points.length >= 3 && polygonSelfIntersects(sel.points);
            const pts = sel.points.map(([x, y]) => `${x},${y}`).join(' ');
            handles += `<polygon points="${pts}" fill="none" class="outline${bad ? ' bad' : ''}" stroke-width="${unit * 1.5}" data-part="${state.part}" data-edge="1"/>`;
            sel.points.forEach(([x, y], k) => {
                handles += `<circle cx="${x}" cy="${y}" r="${unit * 5}" class="handle${k === state.vertex ? ' active' : ''}" stroke-width="${unit * 1.5}" data-vertex="${k}"><title>${formatNum(x)}, ${formatNum(y)}</title></circle>`;
            });
        } else if (sel?.kind === 'circle') {
            handles += `<circle cx="${sel.cx}" cy="${sel.cy}" r="${sel.r}" fill="none" class="outline" stroke-width="${unit * 1.5}"/>`;
            handles += `<circle cx="${sel.cx}" cy="${sel.cy}" r="${unit * 5}" class="handle" stroke-width="${unit * 1.5}" data-handle="centre"><title>centre</title></circle>`;
            handles += `<rect x="${sel.cx + sel.r - unit * 4}" y="${sel.cy - unit * 4}" width="${unit * 8}" height="${unit * 8}" class="handle" stroke-width="${unit * 1.5}" data-handle="radius"><title>radius ${formatNum(sel.r)}</title></rect>`;
        }
        const origin = `<circle cx="0" cy="0" r="${unit * 3}" class="origin"/>`;
        svg.innerHTML = `<g class="grid-layer">${grid}</g>${contextLayer(s)}${parts}${origin}${handles}`;

        const verts = shapeVertexCount(s);
        meter.textContent = `${s.parts.length}/${LIMITS.shapeParts} parts · ${verts}/${LIMITS.shapePoints} polygon vertices (circles are free)`;
        meter.classList.toggle('over', verts > LIMITS.shapePoints || s.parts.length > LIMITS.shapeParts);
    }

    const toLocal = (e) => {
        const p = svg.createSVGPoint();
        p.x = e.clientX;
        p.y = e.clientY;
        const q = p.matrixTransform(svg.getScreenCTM().inverse());
        return [q.x, q.y];
    };
    const snap = (v) => Math.round(v / state.snap) * state.snap;

    svg.addEventListener('pointerdown', (e) => {
        const t = e.target;
        const s = spec.get();
        if (t.dataset.vertex !== undefined) {
            state.vertex = Number(t.dataset.vertex);
            drag = { kind: 'vertex', index: state.vertex };
        } else if (t.dataset.handle) {
            drag = { kind: t.dataset.handle };
        } else if (t.closest('[data-part]') && !t.dataset.edge) {
            const i = Number(t.closest('[data-part]').dataset.part);
            if (i !== state.part) {
                state.part = i;
                state.vertex = null;
                app.rebuildInspector();
            }
            return;
        } else {
            return;
        }
        e.preventDefault();
        svg.setPointerCapture(e.pointerId);
        drag.key = `shape-drag-${spec.kind}-${Date.now()}`;
        drag.part = s.parts[state.part];
        draw();
    });
    svg.addEventListener('pointermove', (e) => {
        if (!drag) return;
        const [x, y] = toLocal(e).map(snap);
        commitShape((s) => {
            const p = s.parts[state.part];
            if (drag.kind === 'vertex') p.points[drag.index] = [quantize(x), quantize(y)];
            else if (drag.kind === 'centre') { p.cx = quantize(x); p.cy = quantize(y); }
            else if (drag.kind === 'radius') p.r = quantize(Math.max(1 / 16, Math.hypot(x - p.cx, y - p.cy)));
        }, { coalesce: drag.key });
    });
    const endDrag = () => {
        if (!drag) return;
        drag = null;
        draw();
    };
    svg.addEventListener('pointerup', endDrag);
    svg.addEventListener('pointercancel', endDrag);

    // Double-click an edge of the selected polygon to insert a vertex there.
    svg.addEventListener('dblclick', (e) => {
        const s = spec.get();
        const p = s.parts[state.part];
        if (p?.kind !== 'polygon') return;
        if (shapeVertexCount(s) >= LIMITS.shapePoints) {
            toast(`The slot already uses all ${LIMITS.shapePoints} vertices.`, 'warning');
            return;
        }
        const [x, y] = toLocal(e).map(snap);
        let best = 0, bestD = Infinity;
        p.points.forEach((a, i) => {
            const b = p.points[(i + 1) % p.points.length];
            const dx = b[0] - a[0], dy = b[1] - a[1];
            const t = Math.max(0, Math.min(1, ((x - a[0]) * dx + (y - a[1]) * dy) / (dx * dx + dy * dy || 1)));
            const d = Math.hypot(a[0] + t * dx - x, a[1] + t * dy - y);
            if (d < bestD) { bestD = d; best = i; }
        });
        structural((sh) => {
            sh.parts[state.part].points.splice(best + 1, 0, [quantize(x), quantize(y)]);
            state.vertex = best + 1;
        });
    });

    /* --- parts list and selected part --- */
    const partsList = h('ol', { class: 'parts' }, shape.parts.map((p, i) => {
        const label = p.kind === 'polygon' ? `Polygon · ${p.points.length} vertices` : `Circle · r ${formatNum(p.r)}`;
        const color = p.color ?? shape.color ?? spec.inherited();
        return h('li', { class: i === state.part ? 'selected' : '' },
            h('button', { type: 'button', class: 'part-pick', onclick: () => { state.part = i; state.vertex = null; app.rebuildInspector(); } },
                h('span', { class: `swatch${p.color || shape.color ? '' : ' inherits'}`, style: `background:${color}` }), label),
            h('span', { class: 'card-tools' },
                button('↑', () => structural((s) => { [s.parts[i - 1], s.parts[i]] = [s.parts[i], s.parts[i - 1]]; state.part = i - 1; }), { class: 'icon', disabled: i === 0, title: 'Draw earlier (underneath)' }),
                button('↓', () => structural((s) => { [s.parts[i + 1], s.parts[i]] = [s.parts[i], s.parts[i + 1]]; state.part = i + 1; }), { class: 'icon', disabled: i === shape.parts.length - 1, title: 'Draw later (on top)' }),
                button('✕', () => structural((s) => { s.parts.splice(i, 1); state.vertex = null; }), { class: 'icon danger', title: 'Remove part' })));
    }));

    const form = app.form;
    const sel = shape.parts[state.part];
    const detail = h('div', { class: 'part-detail' });
    const partGet = (key) => () => spec.get()?.parts[state.part]?.[key];
    const partSet = (key) => (v) => { spec.get().parts[state.part][key] = v; };

    if (sel) {
        detail.append(form.color('Part colour', partGet('color'), partSet('color'), {
            optional: true, inherited: () => spec.get().color ?? spec.inherited(),
            help: spec.kind === 'needle' ? 'Parts that inherit the needle colour flash with alerts; parts with their own colour do not.' : null,
        }));
        if (sel.kind === 'circle') {
            detail.append(h('div', { class: 'row three' },
                form.number('cx', partGet('cx'), (v) => partSet('cx')(quantize(v)), { unit: 'px' }),
                form.number('cy', partGet('cy'), (v) => partSet('cy')(quantize(v)), { unit: 'px' }),
                form.number('r', partGet('r'), (v) => partSet('r')(quantize(v)), { unit: 'px', min: 0.0625 })));
        } else {
            const area = h('textarea', { rows: 3, spellcheck: 'false', class: 'points', 'aria-label': 'Polygon points' });
            const note = h('p', { class: 'help' }, 'x,y pairs in px. Drag handles on the canvas; double-click an edge to add a vertex.');
            const read = () => {
                const p = spec.get()?.parts[state.part];
                if (p?.kind === 'polygon' && document.activeElement !== area) area.value = pointsText(p.points);
            };
            read();
            area.addEventListener('input', () => {
                const r = parsePointsText(area.value);
                area.classList.toggle('invalid', !!r.error);
                note.textContent = r.error ?? 'x,y pairs in px. Drag handles on the canvas; double-click an edge to add a vertex.';
                if (!r.error) commitShape((s) => { s.parts[state.part].points = r.points; }, { coalesce: `points-${spec.kind}` });
            });
            area.addEventListener('blur', read);
            form.syncers.push(read);
            detail.append(h('div', { class: 'field wide' }, h('label', { class: 'field-label' }, 'Points'), area, note),
                h('div', { class: 'toolbar' },
                    button('Delete selected vertex', () => structural((s) => {
                        s.parts[state.part].points.splice(state.vertex, 1);
                        state.vertex = null;
                    }), { disabled: state.vertex === null || sel.points.length <= 3 }),
                    button('Mirror left ↔ right', () => structural((s) => {
                        s.parts[state.part].points = s.parts[state.part].points.map(([x, y]) => [x === 0 ? 0 : -x, y]).reverse();
                    }))));
        }
    }

    /* --- SVG path import --- */
    const pathArea = h('textarea', { rows: 3, spellcheck: 'false', placeholder: 'M0,-170 C 8,-100 10,-20 0,30 C -10,-20 -8,-100 0,-170 Z   — or paste a whole <svg>' });
    const scale = h('input', { type: 'number', value: '1', step: 'any', 'aria-label': 'Scale' });
    const dx = h('input', { type: 'number', value: '0', step: 'any', 'aria-label': 'Offset x' });
    const dy = h('input', { type: 'number', value: '0', step: 'any', 'aria-label': 'Offset y' });
    const importPath = () => {
        let src = pathArea.value.trim();
        const ds = [...src.matchAll(/\sd\s*=\s*["']([^"']+)["']/g)].map((mm) => mm[1]);
        const pointsAttrs = [...src.matchAll(/\spoints\s*=\s*["']([^"']+)["']/g)].map((mm) => `M${mm[1]}Z`);
        if (ds.length || pointsAttrs.length) src = [...ds, ...pointsAttrs].join(' ');
        let rings;
        try {
            rings = flattenPath(src, 0.1);
        } catch (err) {
            toast(`Could not read the path: ${err.message}`, 'error');
            return;
        }
        if (rings.length === 0) {
            toast('The path has no closed areas to fill.', 'warning');
            return;
        }
        const s = spec.get();
        const room = LIMITS.shapeParts - s.parts.length;
        if (rings.length > room) {
            toast(`The path has ${rings.length} separate areas but only ${room} part slot(s) are free.`, 'error');
            return;
        }
        const k = Number(scale.value) || 1, ox = Number(dx.value) || 0, oy = Number(dy.value) || 0;
        const placed = rings.map((r) => r.map(([x, y]) => [x * k + ox, y * k + oy]));
        const budget = LIMITS.shapePoints - shapeVertexCount(s);
        const { rings: fitted, tolerance } = simplifyToBudget(placed, budget, 0.1);
        const polys = fitted.map((r) => r.map(([x, y]) => [quantize(x), quantize(y)])).filter((r) => r.length >= 3);
        const used = polys.reduce((acc, r) => acc + r.length, 0);
        if (used > budget) {
            toast(`Even simplified, the path needs ${used} vertices and ${budget} are free.`, 'error');
            return;
        }
        structural((sh) => {
            for (const r of polys) sh.parts.push({ kind: 'polygon', points: r, color: null });
            state.part = sh.parts.length - 1;
        });
        toast(`Added ${polys.length} polygon(s), ${used} vertices, flattened to within ${tolerance.toFixed(2)} px.`, tolerance > 0.5 ? 'warning' : 'info');
    };
    const importer = h('details', { class: 'importer' },
        h('summary', {}, 'Import an SVG path'),
        h('p', { class: 'help' }, 'Curves and arcs are flattened to straight edges (0.1 px), then simplified only as far as needed to fit the vertex budget. Draw at 12 o\'clock with the origin at the ' + (spec.kind === 'needle' || spec.kind === 'hub' ? 'pivot.' : 'tick circle.')),
        pathArea,
        h('div', { class: 'row three' },
            h('label', { class: 'mini' }, 'Scale', scale), h('label', { class: 'mini' }, 'Offset x', dx), h('label', { class: 'mini' }, 'Offset y', dy)),
        button('Add as parts', importPath, { class: 'primary' }));

    wrap.append(
        form.color('Slot colour', () => spec.get()?.color ?? null, (v) => { spec.get().color = v; }, {
            optional: true, inherited: spec.inherited, help: 'Used by parts without their own colour.',
        }),
        toolbar, svg, meter, partsList, detail, importer);

    draw();
    return { el: wrap, refresh: draw };
}
