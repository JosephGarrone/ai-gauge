/*
 * Gauge XML parser.
 *
 * Schema: docs/gauge-config-schema.md (normative).
 *
 * A hand-written pull parser over the small XML subset the schema needs -- start tags,
 * attributes, self-closing tags, comments and the prolog. It deliberately avoids pulling in
 * a general XML library: this component must build on a host with no dependencies so the
 * schema can be tested without hardware, and the subset is small enough that a library would
 * cost more than it saves.
 *
 * Behaviour is strict-but-forgiving. Unknown elements and attributes are ignored so a newer
 * authoring tool cannot brick older firmware; out-of-range numbers clamp and count a warning;
 * only a missing required attribute or an impossible range rejects the file.
 */

#include "gauge_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ XML scanning ---- */

typedef struct {
    const char *name;
    size_t      name_len;
    const char *attrs;
    size_t      attrs_len;
    bool        self_closing;
    bool        is_close;
} xml_tag_t;

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_name_char(char c)
{
    return !is_space(c) && c != '>' && c != '/' && c != '=' && c != '<';
}

/* Find `needle` within [p, end), or NULL. */
static const char *find_str(const char *p, const char *end, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || (size_t)(end - p) < n) {
        return NULL;
    }
    for (const char *c = p; c <= end - n; c++) {
        if (memcmp(c, needle, n) == 0) {
            return c;
        }
    }
    return NULL;
}

/*
 * Advance to the next element tag, skipping text, comments, the prolog and doctypes.
 * Returns false at end of input or on a tag that never closes.
 */
static bool next_tag(const char **pp, const char *end, xml_tag_t *tag)
{
    const char *p = *pp;

    while (p < end) {
        /* Skip character data between tags. */
        while (p < end && *p != '<') {
            p++;
        }
        if (p >= end) {
            return false;
        }

        /* Comment, prolog or doctype -- skip wholesale. */
        if ((size_t)(end - p) >= 4 && memcmp(p, "<!--", 4) == 0) {
            const char *close = find_str(p + 4, end, "-->");
            if (close == NULL) {
                return false;
            }
            p = close + 3;
            continue;
        }
        if ((size_t)(end - p) >= 2 && p[1] == '?') {
            const char *close = find_str(p + 2, end, "?>");
            if (close == NULL) {
                return false;
            }
            p = close + 2;
            continue;
        }
        if ((size_t)(end - p) >= 2 && p[1] == '!') {
            const char *close = memchr(p, '>', (size_t)(end - p));
            if (close == NULL) {
                return false;
            }
            p = close + 1;
            continue;
        }

        break;
    }

    if (p >= end) {
        return false;
    }

    memset(tag, 0, sizeof(*tag));
    p++; /* past '<' */

    if (p < end && *p == '/') {
        tag->is_close = true;
        p++;
    }

    /* Element name. */
    tag->name = p;
    while (p < end && is_name_char(*p)) {
        p++;
    }
    tag->name_len = (size_t)(p - tag->name);
    if (tag->name_len == 0) {
        return false;
    }

    /* Attributes run up to '>' -- but quoted values may legally contain '>'. */
    const char *attrs_start = p;
    bool in_quote = false;
    char quote_ch = '\0';
    while (p < end) {
        char c = *p;
        if (in_quote) {
            if (c == quote_ch) {
                in_quote = false;
            }
        } else if (c == '"' || c == '\'') {
            in_quote = true;
            quote_ch = c;
        } else if (c == '>') {
            break;
        }
        p++;
    }
    if (p >= end) {
        return false; /* unterminated tag */
    }

    const char *attrs_end = p;
    if (attrs_end > attrs_start && attrs_end[-1] == '/') {
        tag->self_closing = true;
        attrs_end--;
    }

    tag->attrs     = attrs_start;
    tag->attrs_len = (size_t)(attrs_end - attrs_start);

    *pp = p + 1; /* past '>' */
    return true;
}

static bool tag_is(const xml_tag_t *tag, const char *name)
{
    size_t n = strlen(name);
    return tag->name_len == n && memcmp(tag->name, name, n) == 0;
}

/* Locate an attribute's value within a tag's attribute text. */
static bool attr_find(const xml_tag_t *tag, const char *name,
                      const char **out_val, size_t *out_len)
{
    const char *p    = tag->attrs;
    const char *end  = tag->attrs + tag->attrs_len;
    size_t      nlen = strlen(name);

    while (p < end) {
        while (p < end && is_space(*p)) {
            p++;
        }
        if (p >= end) {
            break;
        }

        const char *key = p;
        while (p < end && is_name_char(*p)) {
            p++;
        }
        size_t key_len = (size_t)(p - key);
        if (key_len == 0) {
            p++; /* not a name character; skip it and resynchronise */
            continue;
        }

        while (p < end && is_space(*p)) {
            p++;
        }
        if (p >= end || *p != '=') {
            continue; /* valueless attribute -- not in this schema, ignore */
        }
        p++;
        while (p < end && is_space(*p)) {
            p++;
        }
        if (p >= end || (*p != '"' && *p != '\'')) {
            continue;
        }

        char        quote = *p++;
        const char *val   = p;
        while (p < end && *p != quote) {
            p++;
        }
        size_t val_len = (size_t)(p - val);
        if (p < end) {
            p++; /* past closing quote */
        }

        if (key_len == nlen && memcmp(key, name, nlen) == 0) {
            *out_val = val;
            *out_len = val_len;
            return true;
        }
    }

    return false;
}

/* ------------------------------------------------------- attribute value helpers ---- */

static void copy_str(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    if (src_len >= dst_size) {
        src_len = dst_size - 1;
    }
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

static bool attr_str(const xml_tag_t *tag, const char *name, char *dst, size_t dst_size)
{
    const char *val;
    size_t      len;
    if (!attr_find(tag, name, &val, &len)) {
        return false;
    }
    copy_str(dst, dst_size, val, len);
    return true;
}

static bool attr_float(const xml_tag_t *tag, const char *name, float *out)
{
    const char *val;
    size_t      len;
    if (!attr_find(tag, name, &val, &len) || len == 0) {
        return false;
    }

    char buf[32];
    copy_str(buf, sizeof(buf), val, len);

    char  *endp = NULL;
    double v    = strtod(buf, &endp);
    if (endp == buf || !isfinite(v)) {
        return false;
    }

    *out = (float)v;
    return true;
}

static bool attr_bool(const xml_tag_t *tag, const char *name, bool *out)
{
    char buf[8];
    if (!attr_str(tag, name, buf, sizeof(buf))) {
        return false;
    }
    if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(buf, "false") == 0 || strcmp(buf, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse "#rrggbb" or "#rgb". */
static bool parse_color(const char *s, size_t len, gauge_color_t *out)
{
    if (len > 0 && s[0] == '#') {
        s++;
        len--;
    }

    int v[6];
    if (len == 6) {
        for (int i = 0; i < 6; i++) {
            if ((v[i] = hex_val(s[i])) < 0) {
                return false;
            }
        }
        out->r = (uint8_t)((v[0] << 4) | v[1]);
        out->g = (uint8_t)((v[2] << 4) | v[3]);
        out->b = (uint8_t)((v[4] << 4) | v[5]);
        return true;
    }

    if (len == 3) {
        for (int i = 0; i < 3; i++) {
            if ((v[i] = hex_val(s[i])) < 0) {
                return false;
            }
        }
        /* Expand each nibble so #fff maps to #ffffff, not #f0f0f0. */
        out->r = (uint8_t)((v[0] << 4) | v[0]);
        out->g = (uint8_t)((v[1] << 4) | v[1]);
        out->b = (uint8_t)((v[2] << 4) | v[2]);
        return true;
    }

    return false;
}

/* Reads a colour attribute; leaves *out untouched and counts a warning if malformed. */
static void attr_color(const xml_tag_t *tag, const char *name, gauge_color_t *out,
                       uint16_t *warnings)
{
    const char *val;
    size_t      len;
    if (!attr_find(tag, name, &val, &len)) {
        return;
    }
    if (!parse_color(val, len, out)) {
        (*warnings)++;
    }
}

static float clampf(float v, float lo, float hi, uint16_t *warnings)
{
    if (v < lo) {
        (*warnings)++;
        return lo;
    }
    if (v > hi) {
        (*warnings)++;
        return hi;
    }
    return v;
}

/* Reads a non-negative pixel dimension, clamping absurd values rather than rejecting. */
static void attr_px(const xml_tag_t *tag, const char *name, uint16_t *out, uint16_t *warnings)
{
    float v;
    if (attr_float(tag, name, &v)) {
        *out = (uint16_t)clampf(v, 0.0f, 4096.0f, warnings);
    }
}

static void attr_coord(const xml_tag_t *tag, const char *name, int16_t *out,
                       uint16_t *warnings)
{
    float v;
    if (attr_float(tag, name, &v)) {
        *out = (int16_t)clampf(v, -4096.0f, 4096.0f, warnings);
    }
}

/* ------------------------------------------------------------------- defaults ------- */

void gauge_config_set_defaults(gauge_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    cfg->version = GAUGE_CONFIG_SCHEMA_VERSION;

    cfg->source.min     = 0.0f;
    cfg->source.max     = 100.0f;
    cfg->source.damping = 0.15f;

    cfg->face.start_angle = 225.0f;
    cfg->face.sweep       = 270.0f;
    cfg->face.background  = (gauge_color_t){0x00, 0x00, 0x00};
    cfg->face.radius_px   = 0; /* fit the panel */

    cfg->face.ticks.major_every     = 10.0f;
    cfg->face.ticks.minor_every     = 0.0f;
    cfg->face.ticks.major_len_px    = 20;
    cfg->face.ticks.minor_len_px    = 10;
    cfg->face.ticks.major_width_px  = 4;
    cfg->face.ticks.minor_width_px  = 2;
    cfg->face.ticks.color           = (gauge_color_t){0xff, 0xff, 0xff};

    cfg->face.labels.every  = 0.0f; /* follow major_every */
    cfg->face.labels.color  = (gauge_color_t){0xff, 0xff, 0xff};
    snprintf(cfg->face.labels.font, sizeof(cfg->face.labels.font), "montserrat_24");
    snprintf(cfg->face.labels.format, sizeof(cfg->face.labels.format), "%%g");

    cfg->needle.style           = GAUGE_NEEDLE_TAPER;
    cfg->needle.length_px       = 0; /* derive from face radius */
    cfg->needle.width_px        = 12;
    cfg->needle.color           = (gauge_color_t){0xff, 0x17, 0x44};
    cfg->needle.pivot_radius_px = 16;
    cfg->needle.tail_px         = 0;

    cfg->readout.x     = INT16_MIN; /* centre horizontally */
    cfg->readout.y     = 0;
    cfg->readout.color = (gauge_color_t){0xff, 0xff, 0xff};
    snprintf(cfg->readout.font, sizeof(cfg->readout.font), "montserrat_48");
    snprintf(cfg->readout.format, sizeof(cfg->readout.format), "%%.1f");

    cfg->peak.color      = (gauge_color_t){0xff, 0xab, 0x00};
    cfg->peak.length_px  = 28;
    cfg->peak.width_px   = 5;
    cfg->peak.show_value = true;
    cfg->peak.value_y    = INT16_MIN;
    snprintf(cfg->peak.font, sizeof(cfg->peak.font), "montserrat_16");
    snprintf(cfg->peak.format, sizeof(cfg->peak.format), "%%.1f");
    snprintf(cfg->peak.prefix, sizeof(cfg->peak.prefix), "PEAK ");
}

/* --------------------------------------------------------------- element parsers ---- */

static void parse_panel(const xml_tag_t *tag, gauge_config_t *cfg)
{
    char shape[16];

    cfg->panel.specified = true;
    cfg->panel.shape     = GAUGE_PANEL_ROUND;

    if (attr_str(tag, "shape", shape, sizeof(shape))) {
        if (strcmp(shape, "square") == 0) {
            cfg->panel.shape = GAUGE_PANEL_SQUARE;
        } else if (strcmp(shape, "round") != 0) {
            cfg->warning_count++;
        }
    }

    attr_px(tag, "width", &cfg->panel.width_px, &cfg->warning_count);
    attr_px(tag, "height", &cfg->panel.height_px, &cfg->warning_count);
}

static gauge_config_err_t parse_source(const xml_tag_t *tag, gauge_config_t *cfg)
{
    if (!attr_str(tag, "channel", cfg->source.channel, sizeof(cfg->source.channel))) {
        return GAUGE_CONFIG_ERR_MISSING_REQUIRED;
    }

    attr_str(tag, "unit", cfg->source.unit, sizeof(cfg->source.unit));

    if (!attr_float(tag, "min", &cfg->source.min) ||
        !attr_float(tag, "max", &cfg->source.max)) {
        return GAUGE_CONFIG_ERR_MISSING_REQUIRED;
    }

    /* An inverted or empty scale has no sensible rendering, so this is a hard error. */
    if (!(cfg->source.max > cfg->source.min)) {
        return GAUGE_CONFIG_ERR_BAD_RANGE;
    }

    float damping;
    if (attr_float(tag, "damping", &damping)) {
        cfg->source.damping = clampf(damping, 0.0f, 1.0f, &cfg->warning_count);
    }

    return GAUGE_CONFIG_OK;
}

static void parse_face(const xml_tag_t *tag, gauge_config_t *cfg)
{
    float v;

    if (attr_float(tag, "start-angle", &v)) {
        cfg->face.start_angle = fmodf(v, 360.0f);
    }
    if (attr_float(tag, "sweep", &v)) {
        cfg->face.sweep = clampf(v, 1.0f, 360.0f, &cfg->warning_count);
    }

    attr_color(tag, "background", &cfg->face.background, &cfg->warning_count);
    attr_px(tag, "radius", &cfg->face.radius_px, &cfg->warning_count);
}

static void parse_band(const xml_tag_t *tag, gauge_config_t *cfg)
{
    if (cfg->face.band_count >= GAUGE_CONFIG_MAX_BANDS) {
        cfg->warning_count++;
        return;
    }

    gauge_band_t band = {
        .color    = {0xff, 0xff, 0xff},
        .width_px = 18,
    };

    if (!attr_float(tag, "from", &band.from) || !attr_float(tag, "to", &band.to)) {
        cfg->warning_count++;
        return;
    }

    /* Bands outside the scale would render off the arc; clamp rather than reject the file. */
    band.from = clampf(band.from, cfg->source.min, cfg->source.max, &cfg->warning_count);
    band.to   = clampf(band.to, cfg->source.min, cfg->source.max, &cfg->warning_count);
    if (band.to <= band.from) {
        cfg->warning_count++;
        return;
    }

    attr_color(tag, "color", &band.color, &cfg->warning_count);
    attr_px(tag, "width", &band.width_px, &cfg->warning_count);

    cfg->face.bands[cfg->face.band_count++] = band;
}

static void parse_ticks(const xml_tag_t *tag, gauge_config_t *cfg)
{
    float v;

    cfg->face.ticks.present = true;

    if (attr_float(tag, "major-every", &v)) {
        cfg->face.ticks.major_every = clampf(v, 0.0f, 1e6f, &cfg->warning_count);
    }
    if (attr_float(tag, "minor-every", &v)) {
        cfg->face.ticks.minor_every = clampf(v, 0.0f, 1e6f, &cfg->warning_count);
    }

    attr_px(tag, "major-len", &cfg->face.ticks.major_len_px, &cfg->warning_count);
    attr_px(tag, "minor-len", &cfg->face.ticks.minor_len_px, &cfg->warning_count);
    attr_px(tag, "major-width", &cfg->face.ticks.major_width_px, &cfg->warning_count);
    attr_px(tag, "minor-width", &cfg->face.ticks.minor_width_px, &cfg->warning_count);
    attr_color(tag, "color", &cfg->face.ticks.color, &cfg->warning_count);
}

static void parse_labels(const xml_tag_t *tag, gauge_config_t *cfg)
{
    float v;

    cfg->face.labels.present = true;

    if (attr_float(tag, "every", &v)) {
        cfg->face.labels.every = clampf(v, 0.0f, 1e6f, &cfg->warning_count);
    }

    attr_str(tag, "font", cfg->face.labels.font, sizeof(cfg->face.labels.font));
    attr_str(tag, "format", cfg->face.labels.format, sizeof(cfg->face.labels.format));
    attr_color(tag, "color", &cfg->face.labels.color, &cfg->warning_count);
    attr_px(tag, "radius", &cfg->face.labels.radius_px, &cfg->warning_count);
}

static void parse_needle(const xml_tag_t *tag, gauge_config_t *cfg)
{
    char style[16];

    if (attr_str(tag, "style", style, sizeof(style))) {
        if (strcmp(style, "line") == 0) {
            cfg->needle.style = GAUGE_NEEDLE_LINE;
        } else if (strcmp(style, "arrow") == 0) {
            cfg->needle.style = GAUGE_NEEDLE_ARROW;
        } else if (strcmp(style, "taper") == 0) {
            cfg->needle.style = GAUGE_NEEDLE_TAPER;
        } else {
            cfg->warning_count++;
        }
    }

    attr_px(tag, "length", &cfg->needle.length_px, &cfg->warning_count);
    attr_px(tag, "width", &cfg->needle.width_px, &cfg->warning_count);
    attr_px(tag, "pivot-radius", &cfg->needle.pivot_radius_px, &cfg->warning_count);
    attr_px(tag, "tail", &cfg->needle.tail_px, &cfg->warning_count);
    attr_color(tag, "color", &cfg->needle.color, &cfg->warning_count);
}

/* ------------------------------------------------------------------- shapes --------- */

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/*
 * Read one number from an SVG-style list. Bounded by `end` because attribute values are not
 * NUL-terminated, which rules out strtod(). Accepts what drawing tools emit: an optional sign,
 * digits, a decimal point and an exponent -- including SVG's "1-2" and ".5.5" run-togethers.
 */
static bool scan_number(const char **pp, const char *end, float *out)
{
    const char *p      = *pp;
    double      sign   = 1.0;
    double      mant   = 0.0;
    bool        digits = false;

    if (p < end && (*p == '+' || *p == '-')) {
        sign = (*p == '-') ? -1.0 : 1.0;
        p++;
    }
    while (p < end && is_digit(*p)) {
        mant   = mant * 10.0 + (*p - '0');
        digits = true;
        p++;
    }
    if (p < end && *p == '.') {
        double scale = 0.1;
        p++;
        while (p < end && is_digit(*p)) {
            mant  += (*p - '0') * scale;
            scale *= 0.1;
            digits = true;
            p++;
        }
    }
    if (!digits) {
        return false;
    }

    if (p < end && (*p == 'e' || *p == 'E')) {
        const char *q     = p + 1;
        int         esign = 1;
        int         exp   = 0;
        bool        edig  = false;
        if (q < end && (*q == '+' || *q == '-')) {
            esign = (*q == '-') ? -1 : 1;
            q++;
        }
        while (q < end && is_digit(*q)) {
            if (exp < 1000) {
                exp = exp * 10 + (*q - '0');
            }
            edig = true;
            q++;
        }
        /* A bare 'e' is not part of the number; leave it for the caller to reject. */
        if (edig) {
            mant *= pow(10.0, esign * exp);
            p     = q;
        }
    }

    double v = sign * mant;
    if (!isfinite(v)) {
        return false;
    }

    *out = (float)v;
    *pp  = p;
    return true;
}

static bool is_list_sep(char c)
{
    return is_space(c) || c == ',';
}

/* Pixels to stored 1/16 px, clamping to what an int16 can hold. */
static int16_t to_shape_units(float px, uint16_t *warnings)
{
    float lim = (float)GAUGE_SHAPE_COORD_LIMIT_PX;
    return (int16_t)lroundf(clampf(px, -lim, lim, warnings) * GAUGE_SHAPE_COORD_SCALE);
}

/* An optional colour: sets *has only when present and well-formed; malformed counts a warning. */
static void attr_opt_color(const xml_tag_t *tag, bool *has, gauge_color_t *color,
                           uint16_t *warnings)
{
    const char *val;
    size_t      len;
    if (!attr_find(tag, "color", &val, &len)) {
        return;
    }
    if (parse_color(val, len, color)) {
        *has = true;
    } else {
        (*warnings)++;
    }
}

/*
 * Start a shape container. A repeated container replaces the earlier one rather than merging
 * with it, so a file cannot quietly accumulate parts from two definitions.
 *
 * Returns the shape that subsequent <polygon> and <circle> elements add to, or NULL for a
 * self-closing (empty) container.
 */
static gauge_shape_t *open_shape(const xml_tag_t *tag, gauge_shape_t *shape, uint16_t *warnings)
{
    memset(shape, 0, sizeof(*shape));
    attr_opt_color(tag, &shape->has_color, &shape->color, warnings);
    return tag->self_closing ? NULL : shape;
}

static void parse_polygon(const xml_tag_t *tag, gauge_shape_t *shape, uint16_t *warnings)
{
    const char *p;
    size_t      len;
    uint16_t    clamp_warnings = 0;
    int         count          = 0;
    int64_t     twice_area     = 0;

    if (shape->part_count >= GAUGE_SHAPE_MAX_PARTS || !attr_find(tag, "points", &p, &len)) {
        goto reject;
    }

    const char        *end  = p + len;
    gauge_shape_part_t part = {
        .kind  = GAUGE_SHAPE_PART_POLYGON,
        .first = shape->point_count,
    };

    /* Points are written straight into the pool and only committed once the whole list is good. */
    for (;;) {
        float x, y;

        while (p < end && is_list_sep(*p)) {
            p++;
        }
        if (p >= end) {
            break;
        }
        if (!scan_number(&p, end, &x)) {
            goto reject;
        }
        while (p < end && is_list_sep(*p)) {
            p++;
        }
        if (!scan_number(&p, end, &y)) {
            goto reject; /* malformed, or an odd number of coordinates */
        }
        if (shape->point_count + count >= GAUGE_SHAPE_MAX_POINTS) {
            goto reject; /* over the shape's point budget */
        }

        shape->points[part.first + count] = (gauge_point_t){
            .x = to_shape_units(x, &clamp_warnings),
            .y = to_shape_units(y, &clamp_warnings),
        };
        count++;
    }

    if (count < 3) {
        goto reject;
    }

    /* Shoelace area. A polygon with (almost) none would draw nothing, which is surely a mistake. */
    for (int i = 0; i < count; i++) {
        const gauge_point_t *a = &shape->points[part.first + i];
        const gauge_point_t *b = &shape->points[part.first + (i + 1) % count];
        twice_area += (int64_t)a->x * b->y - (int64_t)b->x * a->y;
    }
    if (llabs(twice_area) < (int64_t)GAUGE_SHAPE_COORD_SCALE * GAUGE_SHAPE_COORD_SCALE / 2) {
        goto reject; /* under a quarter of a square pixel */
    }

    part.count = (uint8_t)count;
    attr_opt_color(tag, &part.has_color, &part.color, warnings);

    shape->point_count += (uint8_t)count;
    shape->parts[shape->part_count++] = part;
    *warnings += clamp_warnings;
    return;

reject:
    (*warnings)++;
}

static void parse_circle(const xml_tag_t *tag, gauge_shape_t *shape, uint16_t *warnings)
{
    float r;
    float cx = 0.0f;
    float cy = 0.0f;

    if (shape->part_count >= GAUGE_SHAPE_MAX_PARTS || !attr_float(tag, "r", &r) || !(r > 0.0f)) {
        (*warnings)++;
        return;
    }
    attr_float(tag, "cx", &cx);
    attr_float(tag, "cy", &cy);

    gauge_shape_part_t part = {
        .kind   = GAUGE_SHAPE_PART_CIRCLE,
        .center = {to_shape_units(cx, warnings), to_shape_units(cy, warnings)},
        .radius = to_shape_units(r, warnings),
    };
    if (part.radius <= 0) {
        (*warnings)++; /* rounds to nothing at 1/16 px */
        return;
    }
    attr_opt_color(tag, &part.has_color, &part.color, warnings);

    shape->parts[shape->part_count++] = part;
}

static void parse_title(const xml_tag_t *tag, gauge_config_t *cfg)
{
    if (cfg->title_count >= GAUGE_CONFIG_MAX_TITLES) {
        cfg->warning_count++;
        return;
    }

    gauge_title_t title = {
        .x     = INT16_MIN, /* centre horizontally */
        .y     = 0,
        .color = {0xff, 0xff, 0xff},
    };
    snprintf(title.font, sizeof(title.font), "montserrat_20");

    if (!attr_str(tag, "text", title.text, sizeof(title.text))) {
        cfg->warning_count++;
        return;
    }

    attr_coord(tag, "x", &title.x, &cfg->warning_count);
    attr_coord(tag, "y", &title.y, &cfg->warning_count);
    attr_str(tag, "font", title.font, sizeof(title.font));
    attr_color(tag, "color", &title.color, &cfg->warning_count);

    cfg->titles[cfg->title_count++] = title;
}

static void parse_readout(const xml_tag_t *tag, gauge_config_t *cfg)
{
    cfg->readout.present = true;

    attr_coord(tag, "x", &cfg->readout.x, &cfg->warning_count);
    attr_coord(tag, "y", &cfg->readout.y, &cfg->warning_count);
    attr_str(tag, "font", cfg->readout.font, sizeof(cfg->readout.font));
    attr_str(tag, "format", cfg->readout.format, sizeof(cfg->readout.format));
    attr_str(tag, "prefix", cfg->readout.prefix, sizeof(cfg->readout.prefix));
    attr_str(tag, "suffix", cfg->readout.suffix, sizeof(cfg->readout.suffix));
    attr_color(tag, "color", &cfg->readout.color, &cfg->warning_count);
}

static void parse_peak(const xml_tag_t *tag, gauge_config_t *cfg)
{
    cfg->peak.present = true;

    attr_color(tag, "color", &cfg->peak.color, &cfg->warning_count);
    attr_px(tag, "length", &cfg->peak.length_px, &cfg->warning_count);
    attr_px(tag, "width", &cfg->peak.width_px, &cfg->warning_count);
    attr_bool(tag, "show-value", &cfg->peak.show_value);
    attr_coord(tag, "value-y", &cfg->peak.value_y, &cfg->warning_count);
    attr_str(tag, "font", cfg->peak.font, sizeof(cfg->peak.font));
    attr_str(tag, "format", cfg->peak.format, sizeof(cfg->peak.format));
    attr_str(tag, "prefix", cfg->peak.prefix, sizeof(cfg->peak.prefix));
}

static void parse_alert(const xml_tag_t *tag, gauge_config_t *cfg)
{
    if (cfg->alert_count >= GAUGE_CONFIG_MAX_ALERTS) {
        cfg->warning_count++;
        return;
    }

    gauge_alert_t alert = {
        .color    = {0xd5, 0x00, 0x00},
        .flash_hz = 2.0f,
        .chime    = false,
    };

    alert.has_above = attr_float(tag, "above", &alert.above);
    alert.has_below = attr_float(tag, "below", &alert.below);

    /* An alert with no threshold can never fire -- ignore it and say so. */
    if (!alert.has_above && !alert.has_below) {
        cfg->warning_count++;
        return;
    }

    float v;
    if (attr_float(tag, "flash-hz", &v)) {
        alert.flash_hz = clampf(v, 0.0f, 20.0f, &cfg->warning_count);
    }

    attr_color(tag, "color", &alert.color, &cfg->warning_count);
    attr_bool(tag, "chime", &alert.chime);

    cfg->alerts[cfg->alert_count++] = alert;
}

/* ------------------------------------------------------------------ entry point ----- */

gauge_config_err_t gauge_config_parse(const char *xml, size_t len, gauge_config_t *cfg)
{
    if (xml == NULL || cfg == NULL) {
        return GAUGE_CONFIG_ERR_INVALID_ARG;
    }
    if (len == 0) {
        len = strlen(xml);
    }

    gauge_config_set_defaults(cfg);

    const char *p   = xml;
    const char *end = xml + len;
    xml_tag_t   tag;

    /* --- root --- */
    bool found_root = false;
    while (next_tag(&p, end, &tag)) {
        if (tag.is_close) {
            continue;
        }
        if (tag_is(&tag, "gauge")) {
            found_root = true;
        }
        break;
    }
    if (!found_root) {
        return GAUGE_CONFIG_ERR_NO_ROOT;
    }

    float version;
    if (!attr_float(&tag, "version", &version)) {
        return GAUGE_CONFIG_ERR_BAD_VERSION;
    }
    /* Refuse a schema we do not understand rather than silently misrendering it. */
    if (version < 1.0f || version > (float)GAUGE_CONFIG_SCHEMA_VERSION) {
        return GAUGE_CONFIG_ERR_BAD_VERSION;
    }
    cfg->version = (uint16_t)version;

    if (!attr_str(&tag, "id", cfg->id, sizeof(cfg->id))) {
        return GAUGE_CONFIG_ERR_MISSING_REQUIRED;
    }

    /*
     * <source> must be parsed before <band>, since band values clamp against the source
     * range. The schema lists it before <face>, and authoring tools emit it in that order.
     */
    bool have_source = false;

    /*
     * Shape elements mean something only inside their parent, so track which of <ticks> or
     * <needle> is open, and which shape container within it. Outside that context they are
     * ignored, like any other unknown element.
     */
    enum { CTX_NONE, CTX_TICKS, CTX_NEEDLE } ctx = CTX_NONE;
    gauge_shape_t *shape = NULL;

    while (next_tag(&p, end, &tag)) {
        if (tag.is_close) {
            if (tag_is(&tag, "gauge")) {
                break;
            }
            if (tag_is(&tag, "ticks") || tag_is(&tag, "needle")) {
                ctx   = CTX_NONE;
                shape = NULL;
            } else if (tag_is(&tag, "major-shape") || tag_is(&tag, "minor-shape") ||
                       tag_is(&tag, "shape") || tag_is(&tag, "hub")) {
                shape = NULL;
            }
            continue;
        }

        if (tag_is(&tag, "panel")) {
            parse_panel(&tag, cfg);
        } else if (tag_is(&tag, "source")) {
            gauge_config_err_t err = parse_source(&tag, cfg);
            if (err != GAUGE_CONFIG_OK) {
                return err;
            }
            have_source = true;
        } else if (tag_is(&tag, "face")) {
            parse_face(&tag, cfg);
        } else if (tag_is(&tag, "band")) {
            parse_band(&tag, cfg);
        } else if (tag_is(&tag, "ticks")) {
            parse_ticks(&tag, cfg);
            ctx   = tag.self_closing ? CTX_NONE : CTX_TICKS;
            shape = NULL;
        } else if (tag_is(&tag, "labels")) {
            parse_labels(&tag, cfg);
        } else if (tag_is(&tag, "needle")) {
            parse_needle(&tag, cfg);
            ctx   = tag.self_closing ? CTX_NONE : CTX_NEEDLE;
            shape = NULL;
        } else if (ctx == CTX_TICKS && tag_is(&tag, "major-shape")) {
            shape = open_shape(&tag, &cfg->face.ticks.major_shape, &cfg->warning_count);
        } else if (ctx == CTX_TICKS && tag_is(&tag, "minor-shape")) {
            shape = open_shape(&tag, &cfg->face.ticks.minor_shape, &cfg->warning_count);
        } else if (ctx == CTX_NEEDLE && tag_is(&tag, "shape")) {
            shape = open_shape(&tag, &cfg->needle.shape, &cfg->warning_count);
        } else if (ctx == CTX_NEEDLE && tag_is(&tag, "hub")) {
            shape = open_shape(&tag, &cfg->needle.hub_shape, &cfg->warning_count);
        } else if (shape != NULL && tag_is(&tag, "polygon")) {
            parse_polygon(&tag, shape, &cfg->warning_count);
        } else if (shape != NULL && tag_is(&tag, "circle")) {
            parse_circle(&tag, shape, &cfg->warning_count);
        } else if (tag_is(&tag, "title")) {
            parse_title(&tag, cfg);
        } else if (tag_is(&tag, "readout")) {
            parse_readout(&tag, cfg);
        } else if (tag_is(&tag, "peak")) {
            parse_peak(&tag, cfg);
        } else if (tag_is(&tag, "alert")) {
            parse_alert(&tag, cfg);
        }
        /* Unknown elements are ignored -- forward compatibility, see the schema doc. */
    }

    if (!have_source) {
        return GAUGE_CONFIG_ERR_MISSING_REQUIRED;
    }

    return GAUGE_CONFIG_OK;
}

const char *gauge_config_err_str(gauge_config_err_t err)
{
    switch (err) {
    case GAUGE_CONFIG_OK:                    return "ok";
    case GAUGE_CONFIG_ERR_MALFORMED_XML:     return "malformed XML";
    case GAUGE_CONFIG_ERR_NO_ROOT:           return "no <gauge> root element";
    case GAUGE_CONFIG_ERR_BAD_VERSION:       return "missing or unsupported schema version";
    case GAUGE_CONFIG_ERR_MISSING_REQUIRED:  return "missing required attribute";
    case GAUGE_CONFIG_ERR_BAD_RANGE:         return "invalid value range (max must exceed min)";
    case GAUGE_CONFIG_ERR_INVALID_ARG:       return "invalid argument";
    default:                                 return "unknown error";
    }
}
