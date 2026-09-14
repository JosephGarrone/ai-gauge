/*
 * Small DOM helpers and model-bound form fields.
 *
 * Fields write straight into the model through the app's commit(), which records undo history. They
 * are never rebuilt while being typed in: instead every field registers a sync function, and the
 * app calls Form.sync() after any change, so edits from elsewhere (dragging text in the preview,
 * undo) show up without stealing focus.
 */

import { FONT_NAMES, utf8Length } from './schema.js';
import { formatNum } from './serialize.js';

export function h(tag, props, ...children) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(props ?? {})) {
        if (v === undefined || v === null || v === false) continue;
        if (k === 'class') el.className = v;
        else if (k === 'dataset') Object.assign(el.dataset, v);
        else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
        else if (k === 'text') el.textContent = v;
        else if (v === true) el.setAttribute(k, '');
        else el.setAttribute(k, String(v));
    }
    for (const c of children.flat(Infinity)) {
        if (c === null || c === undefined || c === false) continue;
        el.append(c instanceof Node ? c : String(c));
    }
    return el;
}

let uid = 0;
const nextId = (stem) => `${stem}-${++uid}`;

export class Form {
    /** @param {(mutate: Function, opts?: object) => void} commit */
    constructor(commit) {
        this.commit = commit;
        this.syncers = [];
    }

    sync() {
        for (const s of this.syncers) s();
    }

    #field(label, control, { unit, help, wide, id } = {}) {
        return h('div', { class: `field${wide ? ' wide' : ''}` },
            h('label', { class: 'field-label', for: id }, label),
            h('div', { class: 'field-control' }, control, unit ? h('span', { class: 'unit' }, unit) : null),
            help ? h('p', { class: 'help' }, help) : null);
    }

    /**
     * A number. `empty` is the model value a blank input means (null, or 0 for "auto"); without it a
     * blank input is simply not applied.
     */
    number(label, get, set, opts = {}) {
        const { min, max, step = 'any', integer = false, placeholder, empty, disabled } = opts;
        const id = nextId('num');
        const input = h('input', {
            id, type: 'number', inputmode: integer ? 'numeric' : 'decimal', min, max,
            step: integer && step === 'any' ? 1 : step, placeholder: typeof placeholder === 'function' ? placeholder() : placeholder, disabled,
        });
        const key = nextId('coalesce');
        const read = () => {
            const v = get();
            input.value = v === null || v === undefined || (empty !== undefined && empty !== null && v === empty) ? '' : formatNum(v);
            if (typeof placeholder === 'function') input.placeholder = placeholder();
            input.classList.remove('invalid');
        };
        read();
        input.addEventListener('input', () => {
            const raw = input.value.trim();
            let v;
            if (raw === '') {
                if (empty === undefined) {
                    input.classList.add('invalid');
                    return;
                }
                v = empty;
            } else {
                v = Number(raw);
                if (!Number.isFinite(v)) {
                    input.classList.add('invalid');
                    return;
                }
                if (integer) v = Math.round(v);
            }
            input.classList.remove('invalid');
            this.commit(() => set(v), { coalesce: key });
        });
        input.addEventListener('blur', read);
        this.syncers.push(() => document.activeElement !== input && read());
        return this.#field(label, input, { ...opts, id });
    }

    /** A number with a range slider alongside, for values that are nicer to scrub. */
    slider(label, get, set, opts) {
        const f = this.number(label, get, set, opts);
        const range = h('input', { type: 'range', min: opts.min, max: opts.max, step: opts.sliderStep ?? opts.step ?? 'any', 'aria-label': label });
        const key = nextId('coalesce');
        const read = () => { range.value = String(get() ?? opts.min); };
        read();
        range.addEventListener('input', () => {
            let v = Number(range.value);
            if (opts.integer) v = Math.round(v);
            this.commit(() => set(v), { coalesce: key });
        });
        this.syncers.push(read);
        f.querySelector('.field-control').prepend(range);
        f.classList.add('with-slider');
        return f;
    }

    /**
     * Free text. The file format cannot carry & or < and the writer uses double quotes, so those are
     * removed as they are typed; the byte counter shows the firmware limit.
     */
    text(label, get, set, opts = {}) {
        const { maxBytes, placeholder, list, pattern } = opts;
        const id = nextId('text');
        const input = h('input', { id, type: 'text', placeholder, list, spellcheck: 'false', autocomplete: 'off' });
        const counter = maxBytes ? h('span', { class: 'counter' }) : null;
        const key = nextId('coalesce');
        const forbidden = pattern ?? /[&<"]/g;
        const update = () => {
            if (!counter) return;
            const n = utf8Length(input.value);
            counter.textContent = `${n}/${maxBytes}`;
            counter.classList.toggle('over', n > maxBytes);
        };
        const read = () => {
            input.value = get();
            update();
        };
        read();
        input.addEventListener('input', () => {
            const cleaned = input.value.replace(forbidden, '');
            if (cleaned !== input.value) {
                const at = input.selectionStart - (input.value.length - cleaned.length);
                input.value = cleaned;
                input.setSelectionRange(at, at);
                input.classList.add('rejected');
                setTimeout(() => input.classList.remove('rejected'), 400);
            }
            update();
            this.commit(() => set(input.value), { coalesce: key });
        });
        this.syncers.push(() => document.activeElement !== input && read());
        return this.#field(label, h('span', { class: 'text-wrap' }, input, counter), { ...opts, id });
    }

    /** A printf format, with quick picks. */
    format(label, get, set, opts = {}) {
        const f = this.text(label, get, set, { maxBytes: 15, ...opts, pattern: /[&<"]/g });
        const input = f.querySelector('input');
        const picks = h('span', { class: 'chips' }, ['%.0f', '%.1f', '%g'].map((fmt) => h('button', {
            type: 'button', class: 'chip', onclick: () => {
                input.value = fmt;
                input.dispatchEvent(new Event('input'));
            },
        }, fmt)));
        f.append(picks);
        return f;
    }

    /** A colour. With `optional`, unticking "own colour" writes null so the part inherits. */
    color(label, get, set, opts = {}) {
        const { optional = false, inherited } = opts;
        const id = nextId('color');
        const picker = h('input', { id, type: 'color' });
        const hex = h('input', { type: 'text', class: 'hex', maxlength: 7, spellcheck: 'false', 'aria-label': `${label} hex` });
        const own = optional ? h('input', { type: 'checkbox', title: 'Use its own colour instead of inheriting' }) : null;
        const key = nextId('coalesce');
        const read = () => {
            const v = get();
            const shown = v ?? (inherited ? inherited() : '#ffffff');
            picker.value = shown;
            if (document.activeElement !== hex) hex.value = v ?? '';
            hex.placeholder = v === null ? `inherits ${shown}` : '';
            if (own) {
                own.checked = v !== null;
                picker.disabled = v === null;
                hex.disabled = v === null;
            }
        };
        read();
        picker.addEventListener('input', () => this.commit(() => set(picker.value), { coalesce: key }));
        hex.addEventListener('input', () => {
            const v = hex.value.trim().toLowerCase();
            const full = /^#?[0-9a-f]{6}$/.test(v) ? `#${v.replace('#', '')}` : /^#?[0-9a-f]{3}$/.test(v) ? `#${v.replace('#', '').replace(/./g, (c) => c + c)}` : null;
            hex.classList.toggle('invalid', full === null);
            if (full) this.commit(() => set(full), { coalesce: key });
        });
        hex.addEventListener('blur', read);
        own?.addEventListener('change', () => {
            this.commit(() => set(own.checked ? (inherited ? inherited() : '#ffffff') : null));
            read();
        });
        this.syncers.push(read);
        const control = h('span', { class: 'color-wrap' }, own ? h('label', { class: 'own' }, own, 'own') : null, picker, hex);
        return this.#field(label, control, { ...opts, id });
    }

    select(label, get, set, options, opts = {}) {
        const id = nextId('select');
        const sel = h('select', { id, disabled: opts.disabled },
            options.map(([value, text]) => h('option', { value }, text)));
        const read = () => {
            const v = get();
            if (![...sel.options].some((o) => o.value === v)) sel.append(h('option', { value: v }, `${v} (unknown)`));
            sel.value = v;
        };
        read();
        sel.addEventListener('change', () => this.commit(() => set(sel.value), { rebuild: opts.rebuild }));
        this.syncers.push(read);
        return this.#field(label, sel, { ...opts, id });
    }

    font(label, get, set, opts) {
        return this.select(label, get, set, FONT_NAMES.map((f) => [f, `${f.replace('montserrat_', 'Montserrat ')} px`]), opts);
    }

    checkbox(label, get, set, opts = {}) {
        const id = nextId('check');
        const box = h('input', { id, type: 'checkbox' });
        const read = () => { box.checked = !!get(); };
        read();
        box.addEventListener('change', () => this.commit(() => set(box.checked), { rebuild: opts.rebuild }));
        this.syncers.push(read);
        return h('div', { class: 'field check' },
            h('label', { for: id }, box, ' ', label),
            opts.help ? h('p', { class: 'help' }, opts.help) : null);
    }
}

export function row(...children) {
    return h('div', { class: 'row' }, ...children);
}

export function sectionHead(title, description, toggle) {
    return h('header', { class: 'section-head' },
        h('div', { class: 'section-title' }, h('h2', {}, title), toggle ?? null),
        description ? h('p', { class: 'lede' }, description) : null);
}

/** An on/off switch for an optional element, e.g. whether the face has ticks at all. */
export function presenceToggle(label, present, onChange) {
    const box = h('input', { type: 'checkbox', role: 'switch' });
    box.checked = present;
    box.addEventListener('change', () => onChange(box.checked));
    return h('label', { class: 'switch' }, box, h('span', {}, label));
}

export function button(label, onclick, opts = {}) {
    return h('button', { type: 'button', class: opts.class ?? '', title: opts.title, disabled: opts.disabled, onclick }, label);
}

/** A card for one item of a repeated element (a band, a title, an alert). */
export function itemCard(title, { onUp, onDown, onRemove, swatch }, ...body) {
    return h('div', { class: 'card' },
        h('div', { class: 'card-head' },
            swatch ? h('span', { class: 'swatch', style: `background:${swatch}` }) : null,
            h('h3', {}, title),
            h('span', { class: 'card-tools' },
                onUp ? button('↑', onUp, { title: 'Move up (drawn earlier)', class: 'icon' }) : null,
                onDown ? button('↓', onDown, { title: 'Move down (drawn later)', class: 'icon' }) : null,
                onRemove ? button('✕', onRemove, { title: 'Remove', class: 'icon danger' }) : null)),
        h('div', { class: 'card-body' }, ...body));
}

/** Modal message with buttons; resolves to the chosen button's value. */
export function ask(title, body, buttons = [['OK', true]]) {
    return new Promise((resolve) => {
        const dlg = h('dialog', { class: 'modal' },
            h('form', { method: 'dialog' },
                h('h2', {}, title),
                h('div', { class: 'modal-body' }, body),
                h('div', { class: 'modal-actions' }, buttons.map(([text, value, cls], i) =>
                    h('button', { value: String(i), class: cls ?? (i === buttons.length - 1 ? 'primary' : '') }, text)))));
        dlg.addEventListener('close', () => {
            const i = Number(dlg.returnValue);
            resolve(Number.isInteger(i) && buttons[i] ? buttons[i][1] : null);
            dlg.remove();
        });
        document.body.append(dlg);
        dlg.showModal();
    });
}

export function toast(message, kind = 'info') {
    const host = document.getElementById('toasts');
    const t = h('div', { class: `toast ${kind}`, role: 'status' }, message);
    host.append(t);
    setTimeout(() => t.classList.add('leaving'), 3200);
    setTimeout(() => t.remove(), 3700);
}
