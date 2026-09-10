/*
 * The compiled-in fallback gauge face.
 *
 * Used when LittleFS cannot be mounted or the stored config is unparseable. It exists so the
 * gauge always has something to display -- a driver turning the ignition on should never be
 * met with a blank screen because a config file went bad.
 *
 * Deliberately expressed as XML and parsed at startup, rather than hand-written as a struct:
 * that way the fallback exercises the same parser as every other config, so a parser
 * regression cannot hide behind a path that bypasses it.
 */

#include "gauge_config.h"

#include <stddef.h>

static const char s_default_xml[] =
    "<gauge version=\"1\" id=\"default\">"
    "  <panel shape=\"round\" width=\"466\" height=\"466\"/>"
    "  <source channel=\"boost\" unit=\"psi\" min=\"0\" max=\"30\" damping=\"0.15\"/>"
    "  <face start-angle=\"225\" sweep=\"270\" background=\"#000000\">"
    "    <band from=\"0\" to=\"18\" color=\"#00c853\"/>"
    "    <band from=\"18\" to=\"25\" color=\"#ffab00\"/>"
    "    <band from=\"25\" to=\"30\" color=\"#d50000\"/>"
    "    <ticks major-every=\"5\" minor-every=\"1\" major-len=\"24\" minor-len=\"12\""
    "           color=\"#ffffff\"/>"
    "    <labels every=\"5\" font=\"montserrat_24\" color=\"#ffffff\" radius=\"150\"/>"
    "  </face>"
    "  <needle style=\"taper\" length=\"170\" width=\"14\" color=\"#ff1744\""
    "          pivot-radius=\"18\"/>"
    "  <title text=\"BOOST\" y=\"150\" font=\"montserrat_20\" color=\"#9e9e9e\"/>"
    "  <readout y=\"320\" font=\"montserrat_48\" format=\"%.1f\" suffix=\" psi\""
    "           color=\"#ffffff\"/>"
    "  <alert above=\"25\" flash-hz=\"2\" color=\"#d50000\"/>"
    "</gauge>";

const gauge_config_t *gauge_config_builtin_default(void)
{
    static gauge_config_t s_cfg;
    static bool           s_ready = false;

    if (!s_ready) {
        if (gauge_config_parse(s_default_xml, sizeof(s_default_xml) - 1, &s_cfg)
            != GAUGE_CONFIG_OK) {
            /*
             * Unreachable unless the built-in XML above is wrong -- which the host tests
             * check for. Fall back to bare defaults so this can never return garbage.
             */
            gauge_config_set_defaults(&s_cfg);
        }
        s_ready = true;
    }

    return &s_cfg;
}
