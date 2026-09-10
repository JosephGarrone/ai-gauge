/*
 * Host tests for the gauge XML parser.
 *
 * Covers the behaviour docs/gauge-config-schema.md promises -- particularly the
 * strict-but-forgiving contract, since that is what stops a bad config file leaving a
 * vehicle with a blank gauge.
 */

#include "gauge_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("  FAIL %s:%d: ", __func__, __LINE__);                    \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

static bool near(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

/* ------------------------------------------------------------------------------------ */

static const char *MINIMAL =
    "<gauge version=\"1\" id=\"test\">"
    "  <source channel=\"boost\" min=\"0\" max=\"30\"/>"
    "</gauge>";

static void test_minimal_document(void)
{
    gauge_config_t cfg;
    CHECK(gauge_config_parse(MINIMAL, 0, &cfg) == GAUGE_CONFIG_OK, "should parse");
    CHECK(strcmp(cfg.id, "test") == 0, "id was '%s'", cfg.id);
    CHECK(strcmp(cfg.source.channel, "boost") == 0, "channel was '%s'", cfg.source.channel);
    CHECK(near(cfg.source.min, 0.0f) && near(cfg.source.max, 30.0f), "range wrong");
    CHECK(cfg.warning_count == 0, "unexpected warnings: %u", cfg.warning_count);

    /* Documented defaults must apply when attributes are absent. */
    CHECK(near(cfg.face.start_angle, 225.0f), "default start-angle");
    CHECK(near(cfg.face.sweep, 270.0f), "default sweep");
    CHECK(near(cfg.source.damping, 0.15f), "default damping");
    CHECK(cfg.needle.style == GAUGE_NEEDLE_TAPER, "default needle style");
    CHECK(cfg.needle.pivot_radius_px == 16, "default pivot radius");
}

static void test_full_document(void)
{
    static const char *XML =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!-- a comment, with a > inside it -->\n"
        "<gauge version=\"1\" id=\"boost\">\n"
        "  <panel shape=\"round\" width=\"466\" height=\"466\"/>\n"
        "  <source channel=\"boost\" unit=\"psi\" min=\"0\" max=\"30\" damping=\"0.2\"/>\n"
        "  <face start-angle=\"225\" sweep=\"270\" background=\"#101010\">\n"
        "    <band from=\"0\" to=\"18\" color=\"#00c853\"/>\n"
        "    <band from=\"18\" to=\"25\" color=\"#ffab00\" width=\"22\"/>\n"
        "    <band from=\"25\" to=\"30\" color=\"#d50000\"/>\n"
        "    <ticks major-every=\"5\" minor-every=\"1\" major-len=\"24\" color=\"#fff\"/>\n"
        "    <labels every=\"5\" font=\"montserrat_24\" radius=\"150\"/>\n"
        "  </face>\n"
        "  <needle style=\"arrow\" length=\"170\" width=\"14\" color=\"#ff1744\"/>\n"
        "  <title text=\"BOOST\" y=\"150\"/>\n"
        "  <readout y=\"320\" format=\"%.1f\" suffix=\" psi\"/>\n"
        "  <alert above=\"25\" flash-hz=\"2\" chime=\"true\"/>\n"
        "</gauge>\n";

    gauge_config_t cfg;
    CHECK(gauge_config_parse(XML, 0, &cfg) == GAUGE_CONFIG_OK, "should parse");
    CHECK(cfg.warning_count == 0, "unexpected warnings: %u", cfg.warning_count);

    CHECK(cfg.panel.specified && cfg.panel.width_px == 466, "panel");
    CHECK(strcmp(cfg.source.unit, "psi") == 0, "unit was '%s'", cfg.source.unit);
    CHECK(near(cfg.source.damping, 0.2f), "damping");

    CHECK(cfg.face.background.r == 0x10 && cfg.face.background.g == 0x10, "bg colour");
    CHECK(cfg.face.band_count == 3, "band count was %u", cfg.face.band_count);
    CHECK(cfg.face.bands[1].width_px == 22, "band width override");
    CHECK(cfg.face.bands[0].color.g == 0xc8, "band colour");

    CHECK(cfg.face.ticks.present && near(cfg.face.ticks.minor_every, 1.0f), "ticks");
    /* #fff must expand to #ffffff, not #f0f0f0. */
    CHECK(cfg.face.ticks.color.r == 0xff, "short hex colour expansion");
    CHECK(cfg.face.ticks.minor_len_px == 10, "minor-len default retained");

    CHECK(cfg.face.labels.present && cfg.face.labels.radius_px == 150, "labels");
    CHECK(cfg.needle.style == GAUGE_NEEDLE_ARROW, "needle style");

    CHECK(cfg.title_count == 1 && strcmp(cfg.titles[0].text, "BOOST") == 0, "title");
    CHECK(cfg.titles[0].x == INT16_MIN, "title x should default to centred");

    CHECK(cfg.readout.present && strcmp(cfg.readout.suffix, " psi") == 0, "readout");
    CHECK(cfg.alert_count == 1 && cfg.alerts[0].has_above && cfg.alerts[0].chime, "alert");
    CHECK(!cfg.alerts[0].has_below, "alert should have no lower threshold");
}

/* --- rejections: these must fall back to the built-in face, not render nonsense --- */

static void test_rejections(void)
{
    gauge_config_t cfg;

    CHECK(gauge_config_parse("not xml at all", 0, &cfg) == GAUGE_CONFIG_ERR_NO_ROOT,
          "garbage input");

    CHECK(gauge_config_parse("<other version=\"1\"/>", 0, &cfg) == GAUGE_CONFIG_ERR_NO_ROOT,
          "wrong root element");

    CHECK(gauge_config_parse("<gauge id=\"x\"><source channel=\"c\" min=\"0\" max=\"1\"/>"
                             "</gauge>", 0, &cfg) == GAUGE_CONFIG_ERR_BAD_VERSION,
          "missing version");

    CHECK(gauge_config_parse("<gauge version=\"99\" id=\"x\"/>", 0, &cfg)
              == GAUGE_CONFIG_ERR_BAD_VERSION,
          "future schema version must be refused, not guessed at");

    CHECK(gauge_config_parse("<gauge version=\"1\"/>", 0, &cfg)
              == GAUGE_CONFIG_ERR_MISSING_REQUIRED,
          "missing id");

    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\"/>", 0, &cfg)
              == GAUGE_CONFIG_ERR_MISSING_REQUIRED,
          "missing source element");

    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"0\"/></gauge>", 0, &cfg)
              == GAUGE_CONFIG_ERR_MISSING_REQUIRED,
          "missing max");

    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"30\" max=\"0\"/></gauge>", 0, &cfg)
              == GAUGE_CONFIG_ERR_BAD_RANGE,
          "inverted range");

    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"5\" max=\"5\"/></gauge>", 0, &cfg)
              == GAUGE_CONFIG_ERR_BAD_RANGE,
          "zero-width range");

    CHECK(gauge_config_parse(NULL, 0, &cfg) == GAUGE_CONFIG_ERR_INVALID_ARG, "null xml");
    CHECK(gauge_config_parse(MINIMAL, 0, NULL) == GAUGE_CONFIG_ERR_INVALID_ARG, "null cfg");
}

/* --- forward compatibility: a newer authoring tool must not brick older firmware --- */

static void test_unknown_elements_ignored(void)
{
    static const char *XML =
        "<gauge version=\"1\" id=\"x\" future-attr=\"whatever\">"
        "  <source channel=\"c\" min=\"0\" max=\"10\"/>"
        "  <hologram depth=\"3\" shimmer=\"true\"/>"
        "  <needle width=\"9\" glow=\"#ff0000\"/>"
        "</gauge>";

    gauge_config_t cfg;
    CHECK(gauge_config_parse(XML, 0, &cfg) == GAUGE_CONFIG_OK,
          "unknown elements and attributes must be ignored, not rejected");
    CHECK(cfg.needle.width_px == 9, "known attributes alongside unknown ones still apply");
    CHECK(cfg.warning_count == 0, "unknown content should not warn: %u", cfg.warning_count);
}

/* --- clamping: usable config, but counted so the settings screen can report it --- */

static void test_clamping_warns(void)
{
    gauge_config_t cfg;

    /* damping is documented as 0..1 */
    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"0\" max=\"10\" damping=\"5\"/>"
                             "</gauge>", 0, &cfg) == GAUGE_CONFIG_OK, "should still parse");
    CHECK(near(cfg.source.damping, 1.0f), "damping should clamp to 1.0");
    CHECK(cfg.warning_count == 1, "clamp should warn once, got %u", cfg.warning_count);

    /* Bands outside the source range clamp to it. */
    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"0\" max=\"10\"/>"
                             "<band from=\"-5\" to=\"50\" color=\"#fff\"/>"
                             "</gauge>", 0, &cfg) == GAUGE_CONFIG_OK, "should still parse");
    CHECK(cfg.face.band_count == 1, "band should survive clamping");
    CHECK(near(cfg.face.bands[0].from, 0.0f) && near(cfg.face.bands[0].to, 10.0f),
          "band should clamp to the source range");
    CHECK(cfg.warning_count == 2, "two clamps expected, got %u", cfg.warning_count);

    /* A malformed colour substitutes the default and warns. */
    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"0\" max=\"10\"/>"
                             "<needle color=\"not-a-colour\"/>"
                             "</gauge>", 0, &cfg) == GAUGE_CONFIG_OK, "should still parse");
    CHECK(cfg.needle.color.r == 0xff && cfg.needle.color.g == 0x17,
          "malformed colour should leave the default in place");
    CHECK(cfg.warning_count == 1, "bad colour should warn, got %u", cfg.warning_count);
}

static void test_degenerate_elements_dropped(void)
{
    gauge_config_t cfg;

    /* An alert with no threshold can never fire. */
    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"0\" max=\"10\"/>"
                             "<alert color=\"#fff\"/>"
                             "</gauge>", 0, &cfg) == GAUGE_CONFIG_OK, "should still parse");
    CHECK(cfg.alert_count == 0, "thresholdless alert should be dropped");
    CHECK(cfg.warning_count == 1, "dropping it should warn");

    /* A zero-width band would render as nothing. */
    CHECK(gauge_config_parse("<gauge version=\"1\" id=\"x\">"
                             "<source channel=\"c\" min=\"0\" max=\"10\"/>"
                             "<band from=\"5\" to=\"5\" color=\"#fff\"/>"
                             "</gauge>", 0, &cfg) == GAUGE_CONFIG_OK, "should still parse");
    CHECK(cfg.face.band_count == 0, "zero-width band should be dropped");
}

static void test_overflow_is_bounded(void)
{
    /* More bands than GAUGE_CONFIG_MAX_BANDS must not overrun the fixed array. */
    char xml[4096];
    int  n = snprintf(xml, sizeof(xml),
                      "<gauge version=\"1\" id=\"x\">"
                      "<source channel=\"c\" min=\"0\" max=\"100\"/>");
    for (int i = 0; i < GAUGE_CONFIG_MAX_BANDS + 5; i++) {
        n += snprintf(xml + n, sizeof(xml) - (size_t)n,
                      "<band from=\"%d\" to=\"%d\" color=\"#fff\"/>", i, i + 1);
    }
    snprintf(xml + n, sizeof(xml) - (size_t)n, "</gauge>");

    gauge_config_t cfg;
    CHECK(gauge_config_parse(xml, 0, &cfg) == GAUGE_CONFIG_OK, "should still parse");
    CHECK(cfg.face.band_count == GAUGE_CONFIG_MAX_BANDS,
          "band count should cap at %d, got %u", GAUGE_CONFIG_MAX_BANDS, cfg.face.band_count);
    CHECK(cfg.warning_count == 5, "each dropped band should warn, got %u", cfg.warning_count);
}

static void test_long_strings_truncate(void)
{
    /* An over-long id must truncate safely rather than overflow. */
    static const char *XML =
        "<gauge version=\"1\" id=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\">"
        "<source channel=\"c\" min=\"0\" max=\"10\"/></gauge>";

    gauge_config_t cfg;
    CHECK(gauge_config_parse(XML, 0, &cfg) == GAUGE_CONFIG_OK, "should parse");
    CHECK(strlen(cfg.id) == GAUGE_CONFIG_MAX_ID_LEN - 1, "id should truncate to fit");
}

static void test_builtin_default_is_valid(void)
{
    /*
     * The built-in face is the last line of defence when a stored config is bad. If it does
     * not parse, a filesystem problem becomes a blank screen -- so this is worth asserting.
     */
    const gauge_config_t *cfg = gauge_config_builtin_default();

    CHECK(cfg != NULL, "must never be NULL");
    CHECK(strcmp(cfg->id, "default") == 0, "id was '%s'", cfg->id);
    CHECK(cfg->face.band_count == 3, "expected 3 bands, got %u", cfg->face.band_count);
    CHECK(cfg->readout.present, "should have a readout");
    CHECK(cfg->title_count == 1, "should have a title");
    CHECK(cfg->warning_count == 0,
          "the built-in face should parse cleanly, got %u warnings", cfg->warning_count);
    CHECK(cfg->source.max > cfg->source.min, "range must be sane");
}

/* ------------------------------------------------------------------------------------ */

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"minimal_document",         test_minimal_document},
        {"full_document",            test_full_document},
        {"rejections",               test_rejections},
        {"unknown_elements_ignored", test_unknown_elements_ignored},
        {"clamping_warns",           test_clamping_warns},
        {"degenerate_elements",      test_degenerate_elements_dropped},
        {"overflow_is_bounded",      test_overflow_is_bounded},
        {"long_strings_truncate",    test_long_strings_truncate},
        {"builtin_default_is_valid", test_builtin_default_is_valid},
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = g_failures;
        printf("%s\n", tests[i].name);
        tests[i].fn();
        if (g_failures == before) {
            printf("  ok\n");
        }
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
