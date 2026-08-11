#include "units.h"
#include <stdio.h>

static int s_units = UNITS_AVIATION;

void units_set(int mode) { s_units = (mode < 0 || mode > 2) ? UNITS_AVIATION : mode; }
int  units_get(void)     { return s_units; }

float units_dist(float km) {
    if (s_units == UNITS_AVIATION) return km * 0.539957f;
    if (s_units == UNITS_IMPERIAL) return km * 0.621371f;
    return km;
}
const char *units_dist_label(void) {
    return s_units == UNITS_AVIATION ? "nm" : (s_units == UNITS_IMPERIAL ? "mi" : "km");
}
const char *units_dist_label_caps(void) {
    return s_units == UNITS_AVIATION ? "NM" : (s_units == UNITS_IMPERIAL ? "MI" : "KM");
}

// Imperial keeps feet: altitude is quoted in feet almost everywhere outside metric
// aviation, and mixing miles with metres would read as a bug rather than a preference.
float       units_alt(float ft)     { return s_units == UNITS_METRIC ? ft * 0.3048f : ft; }
const char *units_alt_label(void)   { return s_units == UNITS_METRIC ? "m" : "ft"; }
bool        units_alt_is_feet(void) { return s_units != UNITS_METRIC; }

float units_spd(float kt) {
    if (s_units == UNITS_METRIC)   return kt * 1.852f;
    if (s_units == UNITS_IMPERIAL) return kt * 1.15078f;
    return kt;
}
const char *units_spd_label(void) {
    return s_units == UNITS_METRIC ? "km/h" : (s_units == UNITS_IMPERIAL ? "mph" : "kt");
}

void units_fmt_alt(char *b, size_t n, float ft, bool onGround) {
    if (onGround) snprintf(b, n, "GND");
    else          snprintf(b, n, "%.0f %s", (double)units_alt(ft), units_alt_label());
}

void units_fmt_spd(char *b, size_t n, float kt) {
    if (kt != kt) snprintf(b, n, "-");
    else          snprintf(b, n, "%.0f %s", (double)units_spd(kt), units_spd_label());
}

void units_fmt_vs(char *b, size_t n, float fpm) {
    if (fpm != fpm)                snprintf(b, n, "-");
    else if (s_units == UNITS_METRIC) snprintf(b, n, "%+.1f m/s", (double)(fpm * 0.00508f));
    else                           snprintf(b, n, "%+.0f fpm", (double)fpm);
}
