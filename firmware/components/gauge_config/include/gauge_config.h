/**
 * @file gauge_config.h
 * @brief Parsed representation of a gauge XML configuration.
 *
 * The normative schema is docs/gauge-config-schema.md. Keep the two in step: that document
 * is what the future authoring web app is written against.
 *
 * This component deliberately depends on neither ESP-IDF nor LVGL -- it is pure data plus a
 * parser, so it builds and its tests run on a host machine with no board attached. That is
 * why it uses its own result type rather than esp_err_t.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GAUGE_CONFIG_SCHEMA_VERSION   1

#define GAUGE_CONFIG_MAX_ID_LEN       32
#define GAUGE_CONFIG_MAX_UNIT_LEN     16
#define GAUGE_CONFIG_MAX_TEXT_LEN     32
#define GAUGE_CONFIG_MAX_FORMAT_LEN   16
#define GAUGE_CONFIG_MAX_FONT_LEN     24

#define GAUGE_CONFIG_MAX_BANDS        8
#define GAUGE_CONFIG_MAX_TITLES       4
#define GAUGE_CONFIG_MAX_ALERTS       4

/** Result of a parse. Warnings still yield a usable config; errors do not. */
typedef enum {
    GAUGE_CONFIG_OK = 0,
    GAUGE_CONFIG_ERR_MALFORMED_XML,      /**< Not well-formed enough to parse. */
    GAUGE_CONFIG_ERR_NO_ROOT,            /**< No <gauge> root element. */
    GAUGE_CONFIG_ERR_BAD_VERSION,        /**< Unknown or missing major schema version. */
    GAUGE_CONFIG_ERR_MISSING_REQUIRED,   /**< A required attribute was absent. */
    GAUGE_CONFIG_ERR_BAD_RANGE,          /**< max <= min, or similar nonsense. */
    GAUGE_CONFIG_ERR_INVALID_ARG,
} gauge_config_err_t;

/** RGB888. Rendered at RGB565, so fine gradations are lost. */
typedef struct {
    uint8_t r, g, b;
} gauge_color_t;

typedef enum {
    GAUGE_PANEL_ROUND,
    GAUGE_PANEL_SQUARE,
} gauge_panel_shape_t;

typedef enum {
    GAUGE_NEEDLE_TAPER,
    GAUGE_NEEDLE_LINE,
    GAUGE_NEEDLE_ARROW,
} gauge_needle_style_t;

/*
 * Custom shapes -- see "Shapes" in docs/gauge-config-schema.md and
 * docs/adr/0006-custom-shapes-as-polygons.md.
 *
 * Coordinates are stored in 1/16 px, so anti-aliased edges keep their sub-pixel position without
 * floats in a struct that gets copied around. The polygon parts of a shape share one point pool.
 */
#define GAUGE_SHAPE_MAX_PARTS         6
#define GAUGE_SHAPE_MAX_POINTS        64
#define GAUGE_SHAPE_COORD_SCALE       16     /**< Stored units per pixel. */
#define GAUGE_SHAPE_COORD_LIMIT_PX    2047   /**< Coordinates and radii clamp to +/- this. */

/** A shape vertex, in 1/GAUGE_SHAPE_COORD_SCALE px. */
typedef struct {
    int16_t x, y;
} gauge_point_t;

typedef enum {
    GAUGE_SHAPE_PART_POLYGON,
    GAUGE_SHAPE_PART_CIRCLE,
} gauge_shape_part_kind_t;

/** One filled region of a shape. */
typedef struct {
    uint8_t       kind;       /**< gauge_shape_part_kind_t */
    bool          has_color;  /**< False: inherit the shape's colour, then the element's. */
    gauge_color_t color;
    uint8_t       first;      /**< Polygon: index of its first point in gauge_shape_t.points. */
    uint8_t       count;      /**< Polygon: number of points. */
    gauge_point_t center;     /**< Circle: centre. */
    int16_t       radius;     /**< Circle: radius, in 1/GAUGE_SHAPE_COORD_SCALE px. */
} gauge_shape_part_t;

/** A user-drawn replacement for a built-in tick, needle or hub. */
typedef struct {
    uint8_t            part_count;   /**< 0 = not specified; the built-in drawing is used. */
    uint8_t            point_count;
    bool               has_color;    /**< False: parts inherit the element's colour. */
    gauge_color_t      color;
    gauge_shape_part_t parts[GAUGE_SHAPE_MAX_PARTS];
    gauge_point_t      points[GAUGE_SHAPE_MAX_POINTS];
} gauge_shape_t;

/** A coloured arc segment marking a value range. */
typedef struct {
    float         from;
    float         to;
    gauge_color_t color;
    uint16_t      width_px;
} gauge_band_t;

/** Static text baked into the pre-rendered face, so it costs nothing per frame. */
typedef struct {
    char          text[GAUGE_CONFIG_MAX_TEXT_LEN];
    int16_t       x;              /**< INT16_MIN means "centre horizontally". */
    int16_t       y;
    char          font[GAUGE_CONFIG_MAX_FONT_LEN];
    gauge_color_t color;
} gauge_title_t;

/** Threshold alert. At least one of has_above / has_below is true. */
typedef struct {
    bool          has_above;
    float         above;
    bool          has_below;
    float         below;
    gauge_color_t color;
    float         flash_hz;       /**< 0 = steady colour change, no flash. */
    bool          chime;          /**< Audible alert via the onboard codec. */
} gauge_alert_t;

typedef struct {
    /* --- <gauge> --- */
    uint16_t version;
    char     id[GAUGE_CONFIG_MAX_ID_LEN];

    /* --- <panel> --- */
    struct {
        bool                specified;
        gauge_panel_shape_t shape;
        uint16_t            width_px;
        uint16_t            height_px;
    } panel;

    /* --- <source> --- */
    struct {
        char  channel[GAUGE_CONFIG_MAX_ID_LEN];
        char  unit[GAUGE_CONFIG_MAX_UNIT_LEN];
        float min;
        float max;
        float damping;            /**< 0..1. 0 = instant, higher = smoother and laggier. */
    } source;

    /* --- <face> --- */
    struct {
        float         start_angle;  /**< Degrees clockwise from 12 o'clock. */
        float         sweep;        /**< Total angular sweep, clockwise. */
        gauge_color_t background;
        uint16_t      radius_px;    /**< 0 = fit the panel's safe area. */

        gauge_band_t  bands[GAUGE_CONFIG_MAX_BANDS];
        uint8_t       band_count;

        struct {
            bool          present;
            float         major_every;
            float         minor_every;   /**< 0 = no minor ticks. */
            uint16_t      major_len_px;
            uint16_t      minor_len_px;
            uint16_t      major_width_px;
            uint16_t      minor_width_px;
            gauge_color_t color;
            gauge_shape_t major_shape;   /**< When set, replaces major_len/width. */
            gauge_shape_t minor_shape;   /**< When set, replaces minor_len/width. */
        } ticks;

        struct {
            bool          present;
            float         every;         /**< 0 = follow ticks.major_every. */
            char          font[GAUGE_CONFIG_MAX_FONT_LEN];
            gauge_color_t color;
            uint16_t      radius_px;     /**< 0 = just inside the ticks. */
            char          format[GAUGE_CONFIG_MAX_FORMAT_LEN];
        } labels;
    } face;

    /* --- <needle> --- */
    struct {
        gauge_needle_style_t style;
        uint16_t             length_px;      /**< 0 = 0.72 x face radius. */
        uint16_t             width_px;
        gauge_color_t        color;
        uint16_t             pivot_radius_px; /**< 0 disables the centre hub. */
        uint16_t             tail_px;
        gauge_shape_t        shape;           /**< When set, replaces style/length/width/tail. */
        gauge_shape_t        hub_shape;       /**< When set, replaces the pivot-radius circle. */
    } needle;

    /* --- <title> --- */
    gauge_title_t titles[GAUGE_CONFIG_MAX_TITLES];
    uint8_t       title_count;

    /* --- <readout> --- */
    struct {
        bool          present;
        int16_t       x;          /**< INT16_MIN means "centre horizontally". */
        int16_t       y;
        char          font[GAUGE_CONFIG_MAX_FONT_LEN];
        char          format[GAUGE_CONFIG_MAX_FORMAT_LEN];
        char          prefix[GAUGE_CONFIG_MAX_TEXT_LEN];
        char          suffix[GAUGE_CONFIG_MAX_TEXT_LEN];
        gauge_color_t color;
    } readout;

    /* --- <peak> --- */
    struct {
        bool          present;
        gauge_color_t color;
        uint16_t      length_px;   /**< From the outer edge of the scale inward. */
        uint16_t      width_px;
        bool          show_value;
        int16_t       value_y;     /**< INT16_MIN means "just below the readout". */
        char          font[GAUGE_CONFIG_MAX_FONT_LEN];
        char          format[GAUGE_CONFIG_MAX_FORMAT_LEN];
        char          prefix[GAUGE_CONFIG_MAX_TEXT_LEN];
    } peak;

    /* --- <alert> --- */
    gauge_alert_t alerts[GAUGE_CONFIG_MAX_ALERTS];
    uint8_t       alert_count;

    /**
     * Count of values clamped or defaulted during parsing.
     *
     * Non-zero means the config was usable but not exactly as authored. Surfaced on the
     * settings screen so a config that "almost works" is diagnosable without a serial cable.
     */
    uint16_t warning_count;
} gauge_config_t;

/**
 * @brief Populate a config with schema defaults.
 *
 * Called automatically by gauge_config_parse(); exposed for tests and for building a config
 * programmatically.
 */
void gauge_config_set_defaults(gauge_config_t *cfg);

/**
 * @brief Parse gauge XML.
 *
 * Strict-but-forgiving, per docs/gauge-config-schema.md: unknown elements and attributes are
 * ignored for forward compatibility, out-of-range numbers are clamped and counted in
 * @c warning_count, but a missing required attribute or an impossible range is an error.
 *
 * @param xml      XML text. Need not be NUL-terminated if @p len is given.
 * @param len      Length of @p xml, or 0 to use strlen().
 * @param[out] cfg Populated on success. Contents are undefined on error.
 * @return GAUGE_CONFIG_OK, or the reason the file was rejected.
 */
gauge_config_err_t gauge_config_parse(const char *xml, size_t len, gauge_config_t *cfg);

/** @brief Human-readable form of an error code, for logs and HTTP responses. */
const char *gauge_config_err_str(gauge_config_err_t err);

/**
 * @brief Load the compiled-in fallback face into @p out.
 *
 * Used when LittleFS is unmountable or the stored config is unparseable, so the gauge always
 * has something to display.
 *
 * Fills a caller-provided config rather than returning a pointer to a static one. A
 * gauge_config_t is over 2KB; a static copy would sit in internal RAM, which WiFi needs, whereas
 * the caller can put this one in PSRAM.
 */
void gauge_config_builtin_default(gauge_config_t *out);

#ifdef __cplusplus
}
#endif
