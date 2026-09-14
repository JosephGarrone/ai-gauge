/*
 * The gauge's runtime behaviour, from gauge_render_set_value() and its helpers: damping, peak-hold
 * and alerts (docs/gauge-xml-interface.md §5.6).
 *
 * Faithful on purpose, quirks included. When one alert hands over directly to another without the
 * value clearing both (possible with damping 0), the firmware keeps the first alert's flash rate
 * and needle colour, while the readout takes the colour of whichever alert currently holds.
 */

const f32 = Math.fround;

export function createSim(m) {
    return {
        displayed: m.source.min,
        target: m.source.min,
        peak: null,
        alertActive: false,
        activeIdx: -1,
        phase: false,
        flashPeriod: 0,
        flashElapsed: 0,
    };
}

function firing(a, v) {
    return (a.above !== null && v > a.above) || (a.below !== null && v < a.below);
}

/** Feed one sample, then advance the flash timer by `dtMs`. Mutates and returns `st`. */
export function stepSim(m, st, raw, dtMs) {
    const { min, max, damping } = m.source;
    const target = Math.min(max, Math.max(min, raw));
    st.target = target;

    st.displayed = damping > 0 ? f32(st.displayed + (target - st.displayed) * (1 - damping)) : target;

    // Peak-hold follows the undamped (but clamped) value.
    if (m.peak && (st.peak === null || target > st.peak)) st.peak = target;

    // evaluate_alerts(): only a change between "some alert" and "none" does anything.
    const idx = m.alerts.findIndex((a) => firing(a, st.displayed));
    const active = idx >= 0;
    if (active !== st.alertActive) {
        st.alertActive = active;
        st.activeIdx = idx;
        const hz = active ? m.alerts[idx].flashHz : 0;
        if (active && hz > 0) {
            st.flashPeriod = Math.trunc(500 / hz);
            st.phase = true;
        } else {
            st.flashPeriod = 0;
            st.phase = active;
        }
        st.flashElapsed = 0;
    }

    if (st.flashPeriod > 0) {
        st.flashElapsed += dtMs;
        while (st.flashElapsed >= st.flashPeriod) {
            st.phase = !st.phase;
            st.flashElapsed -= st.flashPeriod;
        }
    }
    return st;
}

export function resetPeak(st) {
    st.peak = null;
}

/** Colour inheriting needle parts take right now, or null for the needle's own. */
export function needleAlertColor(m, st) {
    return st.alertActive && st.phase && st.activeIdx >= 0 ? m.alerts[st.activeIdx].color : null;
}

/** apply_alert_style(): the readout takes the first alert that holds at the displayed value. */
export function readoutColor(m, st) {
    if (!m.readout) return null;
    if (st.alertActive && st.phase) {
        const a = m.alerts.find((al) => firing(al, st.displayed));
        if (a) return a.color;
    }
    return m.readout.color;
}
