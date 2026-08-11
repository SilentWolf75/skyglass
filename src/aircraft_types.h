#pragma once
// ICAO type designator -> drawing category.
//
// The feed's `t` field carries the ICAO type ("B738", "C172", "EC35"). At roughly 20 px
// on the scope, fine distinctions are wasted, so types collapse into a handful of
// silhouettes that stay readable: swept-wing jets (three sizes), straight-wing props,
// helicopters, fighters and gliders.
#include <stdint.h>

enum AcCategory : uint8_t {
    AC_CAT_NARROW = 0,   // A320, B738 — the default when a type is unknown
    AC_CAT_WIDE,         // B77W, A388, C17 — large swept jets
    AC_CAT_SMALLJET,     // CRJ9, E175, GLF6 — regional + business jets
    AC_CAT_TURBOPROP,    // DH8D, AT72, C130 — straight-wing twin props
    AC_CAT_LIGHT,        // C172, PA28, SR22 — light singles/twins
    AC_CAT_HELI,         // EC35, R44, H60
    AC_CAT_FIGHTER,      // F16, EUFI, SU30
    AC_CAT_GLIDER,       // GLID, DG40
    AC_CAT_COUNT
};

// Longest-prefix match against the embedded table; fallback based on altitude/speed when unlisted.
// `emitter` is the ADS-B emitter category ("A7", "B1", ...) when the feed carries it.
// It is consulted after the type-code table and before the altitude/speed guess: less
// specific than a real type code, but broadcast by the aircraft rather than looked up,
// so it is right where the database is simply missing an entry.
AcCategory aircraft_category(const char *icaoType, float altFt = 0.0f, float gsKt = 0.0f,
                             const char *emitter = nullptr);
