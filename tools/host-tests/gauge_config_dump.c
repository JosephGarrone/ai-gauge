/*
 * Print what gauge_config_parse() makes of XML files, as one JSON object per line.
 *
 * Not a test on its own. The web editor carries a JavaScript port of the parser so it can say
 * exactly what the device will do with a face, and tools/config-app/test/firmware-diff.test.mjs runs
 * both parsers over the same corpus and compares their output field by field. The JSON shape here
 * is the editor's model (tools/config-app/js/parse.js); keep the two in step.
 *
 * Usage: gauge_config_dump FILE...
 */

#include "gauge_config.h"

#include <stdio.h>
#include <stdlib.h>

static void put_str(const char *s)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            printf("\\%c", *p);
        } else if (*p < 0x20) {
            printf("\\u%04x", *p);
        } else {
            putchar(*p);
        }
    }
    putchar('"');
}

/* %.9g round-trips a float exactly, which is what the comparison relies on. */
static void put_float(float v)
{
    printf("%.9g", (double)v);
}

static void put_color(gauge_color_t c)
{
    printf("\"#%02x%02x%02x\"", c.r, c.g, c.b);
}

static void put_units(int16_t u)
{
    printf("%.9g", (double)u / GAUGE_SHAPE_COORD_SCALE);
}

static void put_shape(const gauge_shape_t *s)
{
    if (s->part_count == 0) {
        printf("null");
        return;
    }
    printf("{\"color\":");
    if (s->has_color) {
        put_color(s->color);
    } else {
        printf("null");
    }
    printf(",\"parts\":[");
    for (uint8_t i = 0; i < s->part_count; i++) {
        const gauge_shape_part_t *p = &s->parts[i];
        printf("%s{", i ? "," : "");
        if (p->kind == GAUGE_SHAPE_PART_CIRCLE) {
            printf("\"kind\":\"circle\",\"cx\":");
            put_units(p->center.x);
            printf(",\"cy\":");
            put_units(p->center.y);
            printf(",\"r\":");
            put_units(p->radius);
        } else {
            printf("\"kind\":\"polygon\",\"points\":[");
            for (uint8_t k = 0; k < p->count; k++) {
                const gauge_point_t *pt = &s->points[p->first + k];
                printf("%s[", k ? "," : "");
                put_units(pt->x);
                putchar(',');
                put_units(pt->y);
                putchar(']');
            }
            putchar(']');
        }
        printf(",\"color\":");
        if (p->has_color) {
            put_color(p->color);
        } else {
            printf("null");
        }
        putchar('}');
    }
    printf("]}");
}

static void put_coord(int16_t v)
{
    if (v == INT16_MIN) {
        printf("null");
    } else {
        printf("%d", v);
    }
}

static void dump(const gauge_config_t *c)
{
    static const char *styles[] = {"taper", "line", "arrow"};

    printf("{\"version\":%u,\"id\":", c->version);
    put_str(c->id);

    printf(",\"panel\":");
    if (c->panel.specified) {
        printf("{\"shape\":\"%s\",\"width\":%u,\"height\":%u}",
               c->panel.shape == GAUGE_PANEL_SQUARE ? "square" : "round",
               c->panel.width_px, c->panel.height_px);
    } else {
        printf("null");
    }

    printf(",\"source\":{\"channel\":");
    put_str(c->source.channel);
    printf(",\"unit\":");
    put_str(c->source.unit);
    printf(",\"min\":");
    put_float(c->source.min);
    printf(",\"max\":");
    put_float(c->source.max);
    printf(",\"damping\":");
    put_float(c->source.damping);

    printf("},\"face\":{\"startAngle\":");
    put_float(c->face.start_angle);
    printf(",\"sweep\":");
    put_float(c->face.sweep);
    printf(",\"background\":");
    put_color(c->face.background);
    printf(",\"radius\":%u}", c->face.radius_px);

    printf(",\"bands\":[");
    for (uint8_t i = 0; i < c->face.band_count; i++) {
        const gauge_band_t *b = &c->face.bands[i];
        printf("%s{\"from\":", i ? "," : "");
        put_float(b->from);
        printf(",\"to\":");
        put_float(b->to);
        printf(",\"color\":");
        put_color(b->color);
        printf(",\"width\":%u}", b->width_px);
    }

    printf("],\"ticks\":");
    if (c->face.ticks.present) {
        printf("{\"majorEvery\":");
        put_float(c->face.ticks.major_every);
        printf(",\"minorEvery\":");
        put_float(c->face.ticks.minor_every);
        printf(",\"majorLen\":%u,\"minorLen\":%u,\"majorWidth\":%u,\"minorWidth\":%u,\"color\":",
               c->face.ticks.major_len_px, c->face.ticks.minor_len_px,
               c->face.ticks.major_width_px, c->face.ticks.minor_width_px);
        put_color(c->face.ticks.color);
        printf(",\"majorShape\":");
        put_shape(&c->face.ticks.major_shape);
        printf(",\"minorShape\":");
        put_shape(&c->face.ticks.minor_shape);
        putchar('}');
    } else {
        printf("null");
    }

    printf(",\"labels\":");
    if (c->face.labels.present) {
        printf("{\"every\":");
        put_float(c->face.labels.every);
        printf(",\"font\":");
        put_str(c->face.labels.font);
        printf(",\"color\":");
        put_color(c->face.labels.color);
        printf(",\"radius\":%u,\"format\":", c->face.labels.radius_px);
        put_str(c->face.labels.format);
        putchar('}');
    } else {
        printf("null");
    }

    printf(",\"needle\":{\"style\":\"%s\",\"length\":%u,\"width\":%u,\"tail\":%u,\"color\":",
           styles[c->needle.style], c->needle.length_px, c->needle.width_px, c->needle.tail_px);
    put_color(c->needle.color);
    printf(",\"pivotRadius\":%u,\"shape\":", c->needle.pivot_radius_px);
    put_shape(&c->needle.shape);
    printf(",\"hub\":");
    put_shape(&c->needle.hub_shape);
    putchar('}');

    printf(",\"titles\":[");
    for (uint8_t i = 0; i < c->title_count; i++) {
        const gauge_title_t *t = &c->titles[i];
        printf("%s{\"text\":", i ? "," : "");
        put_str(t->text);
        printf(",\"x\":");
        put_coord(t->x);
        printf(",\"y\":%d,\"font\":", t->y);
        put_str(t->font);
        printf(",\"color\":");
        put_color(t->color);
        putchar('}');
    }

    printf("],\"readout\":");
    if (c->readout.present) {
        printf("{\"x\":");
        put_coord(c->readout.x);
        printf(",\"y\":%d,\"font\":", c->readout.y);
        put_str(c->readout.font);
        printf(",\"format\":");
        put_str(c->readout.format);
        printf(",\"prefix\":");
        put_str(c->readout.prefix);
        printf(",\"suffix\":");
        put_str(c->readout.suffix);
        printf(",\"color\":");
        put_color(c->readout.color);
        putchar('}');
    } else {
        printf("null");
    }

    printf(",\"peak\":");
    if (c->peak.present) {
        printf("{\"color\":");
        put_color(c->peak.color);
        printf(",\"length\":%u,\"width\":%u,\"showValue\":%s,\"valueY\":",
               c->peak.length_px, c->peak.width_px, c->peak.show_value ? "true" : "false");
        put_coord(c->peak.value_y);
        printf(",\"font\":");
        put_str(c->peak.font);
        printf(",\"format\":");
        put_str(c->peak.format);
        printf(",\"prefix\":");
        put_str(c->peak.prefix);
        putchar('}');
    } else {
        printf("null");
    }

    printf(",\"alerts\":[");
    for (uint8_t i = 0; i < c->alert_count; i++) {
        const gauge_alert_t *a = &c->alerts[i];
        printf("%s{\"above\":", i ? "," : "");
        if (a->has_above) {
            put_float(a->above);
        } else {
            printf("null");
        }
        printf(",\"below\":");
        if (a->has_below) {
            put_float(a->below);
        } else {
            printf("null");
        }
        printf(",\"color\":");
        put_color(a->color);
        printf(",\"flashHz\":");
        put_float(a->flash_hz);
        printf(",\"chime\":%s}", a->chime ? "true" : "false");
    }
    printf("]}");
}

int main(int argc, char **argv)
{
    /* A gauge_config_t is a few KB; keep it off the stack like the firmware does. */
    gauge_config_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (f == NULL) {
            fprintf(stderr, "cannot open %s\n", argv[i]);
            return 2;
        }
        static char buf[65536];
        size_t      len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = '\0';

        /* len 0 would mean "use strlen", which is also right for an empty file. */
        gauge_config_err_t err = gauge_config_parse(buf, len, cfg);
        if (err != GAUGE_CONFIG_OK) {
            printf("{\"ok\":false,\"error\":");
            put_str(gauge_config_err_str(err));
            printf("}\n");
            continue;
        }
        printf("{\"ok\":true,\"warnings\":%u,\"model\":", cfg->warning_count);
        dump(cfg);
        printf("}\n");
    }

    free(cfg);
    return 0;
}
