#pragma once
#include <stddef.h>

// One unit preset, honoured by every screen.
//
// This exists because the conversions used to live as file-static helpers inside ui.cpp,
// where only the detail card and the list view could reach them. The radar scope had its
// own hard-coded "kt" and "NM", so with Imperial selected the label over a contact read
// 105kt while the card for that same contact read 121 mph. Anything that prints a value
// with a unit goes through here so there is one table to change, not several to keep in
// step.
enum {
    UNITS_AVIATION = 0,   // ft, kt, nm      (the default; matches how ATC talks)
    UNITS_METRIC   = 1,   // m,  km/h, km
    UNITS_IMPERIAL = 2,   // ft, mph,  mi
};

void units_set(int mode);
int  units_get(void);

// Scalar conversions plus the matching suffix, for callers that lay out the number and
// the unit separately (the radar labels pack them tight: "121mph", no space).
float       units_dist(float km);        // km -> nm / km / mi
const char *units_dist_label(void);      // "nm" / "km" / "mi"
const char *units_dist_label_caps(void); // "NM" / "KM" / "MI"
float       units_alt(float ft);         // ft -> ft / m
const char *units_alt_label(void);       // "ft" / "m"
bool        units_alt_is_feet(void);     // flight levels only make sense in feet
float       units_spd(float kt);         // kt -> kt / km/h / mph
const char *units_spd_label(void);       // "kt" / "km/h" / "mph"

// Preformatted "value unit" strings, for the detail card and the list rows.
void units_fmt_alt(char *b, size_t n, float ft, bool onGround);
void units_fmt_spd(char *b, size_t n, float kt);
void units_fmt_vs(char *b, size_t n, float fpm);
