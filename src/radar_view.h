#pragma once
// Scope rendering API (M1 scope, M2 aircraft, M3 selection). See docs/ARCHITECTURE.md.
// Visual reference: assets/plane_radar_2.0_mockup.html
#include <vector>
#include "aircraft.h"

struct RadarSettings {
    double homeLat, homeLon;
    float  rangeKm;
    double rotationDeg = 0.0;   // 0 = north-up
    bool   mute = false;
};

// Selectable visual skins.
enum RadarTheme {
    THEME_PHOSPHOR = 0,   // green-on-black radar scope (the mockup look)
    THEME_ORB   = 1,   // Orb scope: green gradient, grid, yellow blips
    THEME_AMBER    = 2,   // amber CRT scope (warm monochrome chrome)
    THEME_MILITARY = 3,   // night-vision / military green scope
    THEME_RED      = 4,   // red CRT: keeps dark adaptation at night
    THEME_CYAN     = 5,   // cyan / aviation standard scope theme
    THEME_COUNT    = 6
};

// Flattened, display-ready info for one aircraft (detail card / list view).
struct AcInfo {
    char  hex[8];
    char  call[12];
    char  type[8];
    float altFt;
    bool  onGround;
    float vsFpm;        // NaN if unknown
    float gsKt;         // NaN if unknown
    float distKm;
    float bearingDeg;
    int   squawk;       // -1 if unknown
    bool  emergency;
    bool  military;   // feed dbFlags bit 0
};

namespace radar {

// Build the radar scope (rings, crosshair, rose, sweep, center) under `parent`.
void init(void* lv_parent);                 // pass lv_obj_t*

// Rebuild the aircraft layer from the latest snapshot. Call at poll cadence.
void update(const std::vector<Aircraft>& aircraft, const RadarSettings& s);

// Nearest aircraft to (x,y) within a tap radius -> snapshot index, or -1.
int  hitTest(int x, int y);

// Selection (tracked by hex so it survives data updates). idx < 0 clears.
void select(int idx);
bool selected(AcInfo& out);                 // false if nothing selected/visible

// Snapshot access for the list / stats views.
int  count();
int  countInRange();                        // aircraft within the display range (for the HUD)
bool info(int idx, AcInfo& out);
bool infoByHex(const char* hex, AcInfo& out);   // tracked mode: follow one contact across polls
bool positionByHex(const char* hex, double* lat, double* lon);  // live lat/lon of a contact

// Sweep self-animates via an internal timer; kept for API compatibility.
void tickSweep();

// Selectable visual skin (THEME_PHOSPHOR / THEME_ORB).
void setTheme(int theme);
int  theme();
void cycleTheme();
void setThemeChangedCb(void (*cb)(int theme));   // called when the theme changes (for persistence)
void setRangeLabelVisible(bool v);               // hide the built-in range label (UI shows its own)
// Rectangle on the scope that floating aircraft labels must avoid -- the zoom pill
// sits on top of the scope and was hiding the callsign of anything due south. Passed
// in rather than recomputed here so the pill's geometry stays defined in one place.
enum { KEEPOUT_ZOOM = 0, KEEPOUT_CARD = 1, KEEPOUT_SLOTS = 2 };
void labelPerf(uint32_t *us, uint16_t *moves, uint16_t *seen);   // label layout cost
void setLabelKeepOut(int slot, int x1, int y1, int x2, int y2);   // x2 < x1 clears
void setStillMode(bool on);                     // park every looping animation (screenshot capture)
void setSweepEnabled(bool on);                   // show/hide the rotating sweep line
bool sweepEnabled();
void setAirportsEnabled(bool on);                // show/hide airport markers on the scope
bool airportsEnabled();

// The active theme's chrome accent, so on-scope controls can match the rings
// instead of staying green on an amber or red scope.
uint32_t themeAccent();
// What the scope plots. Aircraft and vessels are different pictures at different
// scales, so the scope shows one or the other rather than overlaying both.
enum TrafficMode { TRAFFIC_AIR = 0, TRAFFIC_MARINE = 1 };
void setTrafficMode(int mode);
int  trafficMode();
bool selectByHex(const char *hex);               // stable selection (index re-sorts each poll)
void setMilitaryPreview(bool on);                // diagnostic: mark every contact military
void setTypeIcons(bool on);                      // per-type silhouettes vs one generic glyph
bool typeIcons();
void setTrailLength(int level);                  // 0=off 1=short 2=medium 3=long (aircraft trails + flow)
void setMaxOnScreen(int n);                       // how many (nearest) aircraft to draw on the scope
void setTracked(const char* hex);                 // pin a contact so the on-screen cap never drops it
const char* tracked();                            // "" when nothing is tracked
void setMapOpacity(int percent);                 // 0..100% map background visibility
void sweepPerf(float *fps, uint32_t *drawUs, float *stepAvg, float *stepMax);  // sweep smoothness telemetry
uint32_t sweepFrameMs(void);      // measured interval between sweep ticks
// Sweep shape, tunable live so variants can be judged by eye without a reflash.
void setSweepTuning(float trailDeg, int trailSteps, float maxStepPx);
void sweepTuning(float *trailDeg, int *trailSteps, float *maxStepPx);
void setLargeText(bool on);                       // accessibility: bigger glyph labels. Call BEFORE init()

// ---- Radar screen display options ---------------------------------------------------
// Everything the scope draws per contact is a row in one table (RADAR_OPTS in
// radar_view.cpp). The settings page, the NVS keys and the on-screen HUD are all
// generated from it, so adding a new switchable piece of information is one row here
// plus one `if` in the draw code -- nothing to wire up in the web form by hand.
enum RadarOpt {
    ROPT_CALLSIGN = 0,   // flight ID above the glyph
    ROPT_ALTITUDE,       // altitude / flight level
    ROPT_VSARROW,        // climb / descent arrow
    ROPT_SPEED,          // ground speed
    ROPT_DISTBRG,        // distance + bearing from home
    ROPT_VECTOR,         // heading prediction line
    ROPT_VECTOR_TICKS,   // minute ticks along that line
    ROPT_ALT_GLOW,       // altitude-band halo behind the glyph
    ROPT_COUNT
};

struct RadarOptInfo {
    const char *key;     // NVS key and web form id (keep <= 15 chars for NVS)
    const char *label;   // shown on the settings page
    const char *shortLabel;  // shown on the device's own menu, where a pill is ~120px
    bool        dflt;
};

const RadarOptInfo &optInfo(int idx);
bool optEnabled(int idx);
void setOptEnabled(int idx, bool on);
// Notified whenever an option changes, including from the on-screen HUD buttons, so the
// caller can persist it. Without this a toggle made on the device is lost at the next
// reboot, which is how these behaved before they were table-driven.
void onOptChanged(void (*cb)(int idx, bool on));

// Named wrappers kept for the on-screen HUD buttons in ui.cpp.
void setVectorLinesEnabled(bool on);
bool vectorLinesEnabled();
void setVectorTicksEnabled(bool on);
bool vectorTicksEnabled();
void setAltGlowEnabled(bool on);
bool altGlowEnabled();
void setDistBrgLabelEnabled(bool on);
bool distBrgLabelEnabled();
void setSpeedAltFormatEnabled(bool on);
bool speedAltFormatEnabled();

} // namespace radar
