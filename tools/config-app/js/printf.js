/*
 * The subset of C printf that gauge `format` attributes may use: literal text, `%%`, and exactly
 * one floating-point conversion (`%f`, `%e` or `%g`, either case) with optional flags, width and
 * precision. The firmware hands every format a double, so anything else is undefined behaviour on
 * the device; checkFormat() is how the editor refuses it.
 */

const CONVERSION = /%([-+ #0]*)(\d*)(?:\.(\d*))?([fFeEgG])/y;

/**
 * Validate a format string.
 * @returns {string|null} null when the format is safe, otherwise the reason it is not.
 */
export function checkFormat(fmt) {
    let conversions = 0;
    for (let i = 0; i < fmt.length; i++) {
        if (fmt[i] !== '%') continue;
        if (fmt[i + 1] === '%') {
            i++;
            continue;
        }
        CONVERSION.lastIndex = i;
        const m = CONVERSION.exec(fmt);
        if (m === null) {
            return `'${fmt.slice(i, i + 4)}' is not allowed: use a single %f, %e or %g, e.g. %.1f`;
        }
        conversions++;
        i = CONVERSION.lastIndex - 1;
    }
    if (conversions === 0) return 'needs one number conversion, e.g. %.1f or %g';
    if (conversions > 1) return 'only one number conversion is allowed';
    return null;
}

/** C's %f digits for a non-negative finite value: round to nearest, ties to even, as glibc and newlib do. */
function fixed(v, prec) {
    let s = v.toFixed(Math.min(prec, 100));
    // toFixed breaks exact binary ties upwards; C breaks them to even. Detect a true tie.
    if (prec < 70 && v < 1e15) {
        const wide = v.toFixed(prec + 30);
        const tail = wide.slice(wide.length - 30);
        if (tail === '5' + '0'.repeat(29)) {
            const down = wide.slice(0, wide.length - 30).replace(/\.$/, '');
            const lastDigit = Number(down[down.length - 1]);
            if (lastDigit % 2 === 0) s = down;
        }
    }
    return s;
}

/** C's %e digits: mantissa with `prec` decimals and an exponent of at least two digits. */
function exponential(v, prec) {
    let s = v.toExponential(Math.min(prec, 100));
    // As in fixed(): an exact tie rounds to even in C, but upwards in toExponential.
    if (prec < 70 && v !== 0) {
        const [wideMant, wideExp] = v.toExponential(prec + 30).split('e');
        const digits = wideMant.replace('.', '');
        if (digits.slice(prec + 1) === '5' + '0'.repeat(29) && Number(digits[prec]) % 2 === 0) {
            const kept = digits.slice(0, prec + 1);
            s = `${kept[0]}${prec > 0 ? '.' + kept.slice(1) : ''}e${wideExp}`;
        }
    }
    const [mant, exp] = s.split('e');
    const e = Number(exp);
    return `${mant}e${e < 0 ? '-' : '+'}${String(Math.abs(e)).padStart(2, '0')}`;
}

function convert(v, flags, prec, conv) {
    const lower = conv.toLowerCase();
    const alt = flags.includes('#');
    const neg = v < 0 || Object.is(v, -0);
    const a = Math.abs(v);
    let body;

    if (!Number.isFinite(a)) {
        body = Number.isNaN(a) ? 'nan' : 'inf';
    } else if (lower === 'f') {
        body = fixed(a, prec ?? 6);
        if (alt && !body.includes('.')) body += '.';
    } else if (lower === 'e') {
        body = exponential(a, prec ?? 6);
        if (alt && !body.includes('.')) body = body.replace('e', '.e');
    } else {
        let p = prec ?? 6;
        if (p === 0) p = 1;
        const x = a === 0 ? 0 : Number(exponential(a, p - 1).split('e')[1]);
        if (p > x && x >= -4) {
            body = fixed(a, p - 1 - x);
        } else {
            body = exponential(a, p - 1);
        }
        if (!alt) {
            body = body.replace(/(\.\d*?)0+(?=e|$)/, '$1').replace(/\.(?=e|$)/, '');
        } else if (!body.includes('.')) {
            body = body.replace(/(?=e|$)/, '.');
        }
    }

    if (conv !== lower) body = body.toUpperCase();

    let sign = '';
    if (neg && !Number.isNaN(v)) sign = '-';
    else if (flags.includes('+')) sign = '+';
    else if (flags.includes(' ')) sign = ' ';
    return { sign, body };
}

/**
 * printf(fmt, value) for a format that passed checkFormat(). An invalid format is returned as the
 * literal text, which is at least visibly wrong in a preview.
 */
export function formatNumber(fmt, value) {
    let out = '';
    for (let i = 0; i < fmt.length; i++) {
        if (fmt[i] !== '%') {
            out += fmt[i];
            continue;
        }
        if (fmt[i + 1] === '%') {
            out += '%';
            i++;
            continue;
        }
        CONVERSION.lastIndex = i;
        const m = CONVERSION.exec(fmt);
        if (m === null) {
            out += fmt[i];
            continue;
        }
        const [, flags, width, precision, conv] = m;
        const prec = precision === undefined ? null : Number(precision || 0);
        const { sign, body } = convert(value, flags, prec, conv);

        let text = sign + body;
        const w = Number(width || 0);
        if (text.length < w) {
            const pad = w - text.length;
            if (flags.includes('-')) {
                text += ' '.repeat(pad);
            } else if (flags.includes('0') && /^[0-9]/.test(body)) {
                text = sign + '0'.repeat(pad) + body;
            } else {
                text = ' '.repeat(pad) + text;
            }
        }
        out += text;
        i = CONVERSION.lastIndex - 1;
    }
    return out;
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();

/** Truncate to what snprintf leaves in a buffer of `maxBytes + 1`. */
export function truncateBytes(s, maxBytes) {
    const bytes = encoder.encode(s);
    return bytes.length <= maxBytes ? s : decoder.decode(bytes.subarray(0, maxBytes));
}
