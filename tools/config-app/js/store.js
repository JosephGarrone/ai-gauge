/*
 * The editor's face library, in localStorage.
 *
 * The device cannot hand a face's XML back (docs/gauge-xml-interface.md §8), so the browser keeps
 * its own copy of everything authored here. Faces are keyed by an internal key rather than by id,
 * because an id is edited freely and may be briefly empty or duplicated while typing.
 *
 * Storage can be unavailable (private windows, blocked site data); everything degrades to an
 * in-memory library, and saveLibrary() reports whether the write stuck.
 */

const KEY = 'ai-gauge-editor/v1';

export function emptyLibrary() {
    return { faces: {}, current: null, deviceHost: '', view: {} };
}

export function loadLibrary() {
    try {
        const raw = localStorage.getItem(KEY);
        if (!raw) return emptyLibrary();
        const lib = JSON.parse(raw);
        return { ...emptyLibrary(), ...lib, faces: lib.faces ?? {} };
    } catch {
        return emptyLibrary();
    }
}

export function saveLibrary(lib) {
    try {
        localStorage.setItem(KEY, JSON.stringify(lib));
        return true;
    } catch {
        return false;
    }
}

export function newKey() {
    return globalThis.crypto?.randomUUID?.() ?? `f${Date.now().toString(36)}${Math.random().toString(36).slice(2)}`;
}

/** Faces sorted most recently edited first. */
export function listFaces(lib) {
    return Object.entries(lib.faces)
        .map(([key, f]) => ({ key, ...f }))
        .sort((a, b) => b.updated - a.updated);
}

/** An id not yet used in the library, based on `base`. */
export function uniqueId(lib, base, exceptKey = null) {
    const taken = new Set(Object.entries(lib.faces).filter(([k]) => k !== exceptKey).map(([, f]) => f.model.id));
    const stem = (base || 'face').replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 27) || 'face';
    if (!taken.has(stem)) return stem;
    for (let i = 2; ; i++) {
        const id = `${stem}_${i}`;
        if (!taken.has(id)) return id;
    }
}
