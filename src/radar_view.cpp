// Radar scope (M1) + aircraft (M2) + selection (M3) + selectable themes (M4).
// Pure LVGL, portable. Visual reference: assets/plane_radar_2.0_mockup.html
//   THEME_PHOSPHOR : green-on-black radar scope (rings, sweep, altitude glyphs)
//   THEME_ORB   : Orb scope: green gradient, square grid, the 7 nearest
//                    aircraft as yellow balls (emitting waves) + off-range arrows.
#include "radar_view.h"
#include "config.h"
#include "geo.h"
#include "coastline.h"
#include "airports.h"
#include "runways.h"
#include "map_bg.h"
#include "vessel.h"
#include "aircraft_types.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <algorithm>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif
#if !defined(ARDUINO)
#include "native_compat.h"       // millis / micros for the simulator
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- phosphor palette (mockup) ----
#define COL_GREEN  lv_color_hex(0x1DFF86)
#define COL_LEAD   lv_color_hex(0x3DFF9A)
#define COL_INK    lv_color_hex(0xEAFFF3)
#define COL_SOFT   lv_color_hex(0x9AFFC8)
// Emergency squawk. Magenta rather than red: the altitude ramp already runs orange-red
// (sub-3,000 ft) through amber, lime and green to cyan, and this used to be the exact
// same value as the sub-3,000 ft band -- so a 7700 looked identical to any low
// aircraft, which is the one case where it must not. Nothing else on the scope is
// magenta on any theme, which is the point: an emergency may break the palette.
#define COL_EMERG  lv_color_hex(0xFF2D95)
// coastline outline — steel blue, deliberately off the red/amber/lime/green/cyan
// altitude-trail palette so land never reads as an aircraft track.
#define COAST_COLOR lv_color_hex(0x4E86C6)
// airport markers — a neutral muted grey-blue so they sit quietly under the traffic.
#define AIRPORT_COLOR lv_color_hex(0x8A93A6)
// Dimmer than the airport marker on purpose: the strips are ground detail you
// glance at, not something competing with the dot and its ident.
#define RUNWAY_COLOR  lv_color_hex(0x6E7789)
// ...but the ident label is meant to be READ. Pale ice-blue, near-opaque: bright enough
// on true black to scan at a glance, and cool enough that it never reads as an aircraft
// callsign (white) or as scope chrome (green/amber).
#define AIRPORT_LABEL_COLOR lv_color_hex(0xD6E6FF)
#define AIRPORT_LABEL_OPA   245
// Military contacts. Violet is the only hue not already spoken for: the altitude ramp
// owns red/amber/lime/green/cyan and the emergency halo owns red. Drawn as four corner
// brackets rather than a ring so it can never be mistaken for the emergency circle.
#define MIL_COLOR lv_color_hex(0xC77DFF)
// ---- orb palette (Orb) ----
#define ORB_BLIP   lv_color_hex(0xFFE11A)
#define ORB_EMERG  lv_color_hex(0xFF4D2E)
#define ORB_ACCENT lv_color_hex(0xFF8A1E)
#define ORB_GRID   lv_color_hex(0x3F8B30)
#define ORB_BG_TOP lv_color_hex(0x18540F)
#define ORB_BG_BOT lv_color_hex(0x09250A)
#define ORB_FLOW   lv_color_hex(0xFFC24D)

// ---- sweep config ----
// These are the original values. A shorter trail and a lower frame rate measured faster
// (9 -> 16 fps) but looked WORSE on the actual panel: the long fading trail visually
// masks the angular stepping, so shortening it exposed the very judder it was meant to
// cure. Raw frame rate is not the thing being optimised here — perceived smoothness is.
// Do not "optimise" these without looking at the screen.
#define SWEEP_PERIOD_MS   6000   // one revolution, honoured whenever frames keep up
// Largest distance the leading edge may travel in one frame, in pixels at the rim.
//
// Expressed in pixels rather than degrees on purpose: what the eye judges is how far the
// beam jumps on screen, and the same angle is a bigger jump on a bigger radius. Three
// degrees is ~11 px at the 466 panel's rim but ~18 px at 720, which is why the identical
// setting looked fine on one board and stepped on the other.
//
// It is a clamp, not a fixed rate. The sweep advances by elapsed time, so a revolution
// takes SWEEP_PERIOD_MS whenever frames keep up, and a late frame slows the beam rather
// than teleporting it. Deriving the angle straight from the wall clock was what made the
// S3 stutter; advancing a fixed amount per frame instead tied rotation speed to frame
// rate, so the same firmware crawled on one panel and span on the other.
#ifndef SWEEP_MAX_STEP_PX
#  define SWEEP_MAX_STEP_PX 11.0f
#endif
#define SWEEP_MAX_STEP_DEG     (SWEEP_MAX_STEP_PX * 180.0f / (3.14159265f * (float)RADAR_R_OUTER_PX))
// Deliberately not the fastest tick the panel can manage. The angle comes from the
// wall clock, so a frame that misses its deadline advances the beam further rather than
// later -- uneven angular steps, which the eye reads as stutter. Draw (~6-9 ms) plus the
// QSPI flush of the invalidated wedge (~95 KB, ~9 ms) sits right on a 20 ms budget, so
// frames landed inconsistently at 37-50 fps. A 30 ms tick every frame comfortably makes
// is steadier than a 20 ms tick it sometimes misses: constant step beats peak rate.
#ifndef SWEEP_FRAME_MS
#  define SWEEP_FRAME_MS  30
#endif
#ifndef SWEEP_TRAIL_DEG
#  define SWEEP_TRAIL_DEG 55.0f
#endif
#ifndef SWEEP_TRAIL_STEPS
#  define SWEEP_TRAIL_STEPS 18   // see sweep_draw_cb: cost is per-polygon
#endif
#define SWEEP_TRAIL_OPA   140

// ---- aircraft / flow / orb config ----
#define TRAIL_MAX         7
#define TAP_RADIUS_PX     40    // generous finger-tap catch radius (picks the nearest glyph within it)
#define FLOW_MAX          700
#define FLOW_REDRAW_EVERY 80
#define FLOW_OPA          55
#define ORB_BLIPS      7
#define ORB_ARROWS     8
#define BALL_R            9
#define WAVE_EXPAND       28.0f

static int        s_theme    = THEME_PHOSPHOR;
static void      (*s_themeCb)(int) = nullptr;
// scope "chrome" palette (rings/sweep/crosshair/labels) — retinted per theme
static lv_color_t s_cRing = COL_GREEN, s_cLead = COL_LEAD, s_cInk = COL_INK, s_cSoft = COL_SOFT;
static lv_obj_t  *s_parent   = nullptr;
static lv_obj_t  *s_mapCanvas = nullptr;   // basemap image, below every other layer
static lv_obj_t  *s_gridLayer = nullptr;
static lv_obj_t  *s_sweep     = nullptr;
static lv_obj_t  *s_acLayer   = nullptr;
static lv_obj_t  *s_flowCanvas = nullptr;
static lv_color_t *s_flowBuf  = nullptr;
static lv_obj_t  *s_rose[4]   = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t  *s_centerDot = nullptr;
static lv_obj_t  *s_pulse     = nullptr;
static lv_obj_t  *s_rangeLbl  = nullptr;
static bool       s_rangeLblVisible = true;
static bool       s_sweepEnabled    = true;
static bool       s_airportsEnabled = true;
static uint32_t   s_accentHex       = 0x1DFF86;   // chrome accent of the active theme
static int        s_trafficMode     = radar::TRAFFIC_AIR;
static int        s_mapOpacity      = 85;
static bool       s_typeIcons       = true;   // per-type silhouettes vs the plain dart
// Diagnostic: draw every contact as military. Military traffic is rare enough that the
// marker would otherwise only be checked the first time one happens to fly past.
static bool       s_milPreview      = false;
static int        s_maxOnScreen     = 20;          // how many (nearest) aircraft to draw (web-configurable)
static bool       s_bigText         = false;       // accessibility: bigger glyph labels (set before init)
static int        s_trailMax        = TRAIL_MAX;   // per-aircraft trail length (0 = off)
static int        s_flowMax         = FLOW_MAX;    // persistent flow-layer segments, count cap (0 = off)
static int        s_flowGenMax      = 14;          // ...and an age cap in polls (~2 s each) so tracks fade out
static lv_timer_t *s_timer    = nullptr;
static float       s_sweepDeg = 0.0f;
// Sweep shape, adjustable at runtime. Comparing variants by eye is the only way to judge
// this -- measured frame time has twice pointed the wrong way -- and reflashing between
// each one makes that comparison useless.
static float       s_trailDeg   = SWEEP_TRAIL_DEG;
static int         s_trailSteps = SWEEP_TRAIL_STEPS;
static float       s_maxStepPx  = SWEEP_MAX_STEP_PX;
static float       s_prevSweepDeg = 0.0f;
static float       s_wavePhase = 0.0f;
static uint32_t    s_lastUpdateMs = 0;       // smooth-motion: cadence + animation clock
static uint32_t    s_animStartMs  = 0;
static uint32_t    s_pollMs       = POLL_INTERVAL_MS;
static int         s_frameCtr     = 0;
static lv_coord_t  s_cx = SCREEN_CX, s_cy = SCREEN_CY;
static lv_area_t   s_keepOut[radar::KEEPOUT_SLOTS] = { { 0, 0, -1, -1 }, { 0, 0, -1, -1 } };
// Chrome the scope must route labels around: the zoom pill (always up) and the detail
// card (only while a contact is selected). Both are opaque and drawn over the scope.
static inline bool keepout_live(const lv_area_t &k) { return k.x2 >= k.x1 && k.y2 >= k.y1; }
static std::string s_selHex;

struct FlowSeg { lv_point_t a, b; uint16_t gen; };   // gen = the poll it was laid down on
static std::deque<FlowSeg> s_flow;
static int s_flowRedrawCtr = 0;
static uint16_t s_flowGen = 0;        // ++ each update(); flow segments fade out after s_flowGenMax polls

struct AcDraw {
    lv_point_t pos;            // current (animated) screen position — what gets drawn
    lv_point_t from, to;       // smooth-motion glide endpoints (M4 interpolation)
    float      track;
    lv_color_t color;
    bool       emergency;
    bool       military;
    bool       inRange;
    char       hex[8];
    char       call[12];
    char       type[8];
    char       altTxt[12];
    float      altFt;
    bool       onGround;
    float      vsFpm, gsKt, distKm, bearingDeg;
    double     lat, lon;       // live position (tracked mode: progress along the route)
    uint8_t    cat;            // AcCategory — which silhouette to draw
    int        squawk;
    lv_coord_t lblW = 0;       // measured label width; text changes rarely, so measure rarely
    lv_coord_t lblDx = 0, lblDy = 0;   // chosen offset from the glyph (see layout_labels)
    bool       lblSet = false;         // has a placement been chosen yet?
    std::vector<lv_point_t> trail;
};
static std::vector<AcDraw> s_acs;
static std::map<std::string, std::vector<lv_point_t>> s_trails;
static std::string s_trackHex;     // pinned contact (tracked view); survives the on-screen cap

static const float GX[4] = { 0.0f,  7.0f, 0.0f, -7.0f };
static const float GY[4] = { -11.0f, 5.0f, 8.0f, 5.0f };

// ---- type silhouettes -------------------------------------------------------
// Local coordinates with the nose pointing up (-Y), rotated by track when drawn.
//
// Each aircraft is assembled from a few CONVEX polygons — fuselage, wings, tailplane —
// rather than one outline. Two reasons: a real swept wing spanning both sides is deeply
// concave, and LVGL's polygon mask is only dependable for convex shapes; and separate
// parts let the wing carry the category (swept / straight / long / delta) while the
// fuselage stays a recognisable aeroplane body. An earlier attempt used a single
// 4-point dart per category, which just produced the same triangle at three sizes.
struct AcPoly { const float *x, *y; uint8_t n; };
struct AcShape {
    AcPoly  parts[4];
    uint8_t nParts;
    uint8_t rotorR;    // helicopter rotor disc radius (0 = not a helicopter)
};

// --- narrowbody airliner: pointed fuselage, swept wings, tailplane ---
static const float NB_BX[6] = { 0.0f,  1.8f, 1.8f,  0.0f, -1.8f, -1.8f };
static const float NB_BY[6] = { -13.0f, -9.0f, 8.0f, 11.0f, 8.0f, -9.0f };
static const float NB_WRX[4] = { 1.5f, 10.0f, 10.0f, 1.5f };
static const float NB_WRY[4] = { -4.0f, 4.0f,  6.2f, 0.5f };
static const float NB_WLX[4] = { -1.5f, -10.0f, -10.0f, -1.5f };
static const float NB_WLY[4] = { -4.0f,  4.0f,   6.2f,  0.5f };
static const float NB_TX[4] = { 4.2f, 4.2f, -4.2f, -4.2f };
static const float NB_TY[4] = { 7.5f, 9.6f,  9.6f,  7.5f };

// --- widebody: same anatomy, noticeably larger ---
static const float WB_BX[6] = { 0.0f,  2.3f, 2.3f,  0.0f, -2.3f, -2.3f };
static const float WB_BY[6] = { -17.0f, -12.0f, 10.0f, 14.0f, 10.0f, -12.0f };
static const float WB_WRX[4] = { 2.0f, 13.0f, 13.0f, 2.0f };
static const float WB_WRY[4] = { -5.0f, 5.0f,  7.8f, 1.0f };
static const float WB_WLX[4] = { -2.0f, -13.0f, -13.0f, -2.0f };
static const float WB_WLY[4] = { -5.0f,  5.0f,   7.8f,  1.0f };
static const float WB_TX[4] = { 5.5f, 5.5f, -5.5f, -5.5f };
static const float WB_TY[4] = { 10.0f, 12.6f, 12.6f, 10.0f };

// --- regional / business jet: compact ---
static const float SJ_BX[6] = { 0.0f,  1.3f, 1.3f, 0.0f, -1.3f, -1.3f };
static const float SJ_BY[6] = { -9.5f, -6.5f, 6.0f, 8.0f, 6.0f, -6.5f };
static const float SJ_WRX[4] = { 1.1f, 7.0f, 7.0f, 1.1f };
static const float SJ_WRY[4] = { -3.0f, 3.0f, 4.6f, 0.4f };
static const float SJ_WLX[4] = { -1.1f, -7.0f, -7.0f, -1.1f };
static const float SJ_WLY[4] = { -3.0f,  3.0f,  4.6f,  0.4f };
static const float SJ_TX[4] = { 3.0f, 3.0f, -3.0f, -3.0f };
static const float SJ_TY[4] = { 5.5f, 7.0f,  7.0f,  5.5f };

// --- turboprop: straight, high-mounted wing well forward ---
static const float TP_BX[6] = { 0.0f,  1.7f, 1.7f, 0.0f, -1.7f, -1.7f };
static const float TP_BY[6] = { -11.0f, -7.5f, 8.0f, 10.0f, 8.0f, -7.5f };
static const float TP_WX[4] = { 11.0f, 11.0f, -11.0f, -11.0f };
static const float TP_WY[4] = { -5.0f, -2.0f,  -2.0f,  -5.0f };
static const float TP_TX[4] = { 4.5f, 4.5f, -4.5f, -4.5f };
static const float TP_TY[4] = { 7.0f, 9.0f,  9.0f,  7.0f };

// --- light aircraft: small, straight wing ---
static const float LT_BX[6] = { 0.0f, 1.3f, 1.3f, 0.0f, -1.3f, -1.3f };
static const float LT_BY[6] = { -8.0f, -5.5f, 5.5f, 7.5f, 5.5f, -5.5f };
static const float LT_WX[4] = { 8.0f, 8.0f, -8.0f, -8.0f };
static const float LT_WY[4] = { -3.5f, -1.0f, -1.0f, -3.5f };
static const float LT_TX[4] = { 3.2f, 3.2f, -3.2f, -3.2f };
static const float LT_TY[4] = { 5.0f, 6.8f,  6.8f,  5.0f };

// --- glider: the wing is the whole identity ---
static const float GL_BX[6] = { 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, -1.0f };
static const float GL_BY[6] = { -7.0f, -4.5f, 7.0f, 9.0f, 7.0f, -4.5f };
static const float GL_WX[4] = { 14.0f, 14.0f, -14.0f, -14.0f };
static const float GL_WY[4] = { -2.5f, -0.8f,  -0.8f,  -2.5f };
static const float GL_TX[4] = { 3.0f, 3.0f, -3.0f, -3.0f };
static const float GL_TY[4] = { 6.5f, 7.8f,  7.8f,  6.5f };

// --- fighter: slim body with delta half-wings ---
static const float FT_BX[6] = { 0.0f, 1.9f, 1.9f, 0.0f, -1.9f, -1.9f };
static const float FT_BY[6] = { -13.0f, -7.0f, 5.0f, 7.5f, 5.0f, -7.0f };
static const float FT_WRX[3] = { 1.7f, 7.8f, 1.7f };
static const float FT_WRY[3] = { -2.5f, 7.0f, 7.0f };
static const float FT_WLX[3] = { -1.7f, -7.8f, -1.7f };
static const float FT_WLY[3] = { -2.5f,  7.0f,  7.0f };
static const float FT_TX[4] = { 3.4f, 3.4f, -3.4f, -3.4f };
static const float FT_TY[4] = { 6.2f, 8.4f,  8.4f,  6.2f };

// --- helicopter: pod + tail boom + tail rotor, under a rotor disc ---
static const float HE_BX[6] = { 0.0f, 3.0f, 3.0f, 0.0f, -3.0f, -3.0f };
static const float HE_BY[6] = { -5.5f, -2.5f, 3.0f, 5.0f, 3.0f, -2.5f };
static const float HE_MX[4] = { 1.1f, 1.1f, -1.1f, -1.1f };
static const float HE_MY[4] = { 4.0f, 11.0f, 11.0f, 4.0f };
static const float HE_TX[4] = { 2.6f, 2.6f, -2.6f, -2.6f };
static const float HE_TY[4] = { 10.0f, 11.6f, 11.6f, 10.0f };

static const AcShape AC_SHAPES[AC_CAT_COUNT] = {
    /* NARROW    */ { { {NB_BX,NB_BY,6}, {NB_WRX,NB_WRY,4}, {NB_WLX,NB_WLY,4}, {NB_TX,NB_TY,4} }, 4, 0 },
    /* WIDE      */ { { {WB_BX,WB_BY,6}, {WB_WRX,WB_WRY,4}, {WB_WLX,WB_WLY,4}, {WB_TX,WB_TY,4} }, 4, 0 },
    /* SMALLJET  */ { { {SJ_BX,SJ_BY,6}, {SJ_WRX,SJ_WRY,4}, {SJ_WLX,SJ_WLY,4}, {SJ_TX,SJ_TY,4} }, 4, 0 },
    /* TURBOPROP */ { { {TP_BX,TP_BY,6}, {TP_WX,TP_WY,4},   {TP_TX,TP_TY,4} },                     3, 0 },
    /* LIGHT     */ { { {LT_BX,LT_BY,6}, {LT_WX,LT_WY,4},   {LT_TX,LT_TY,4} },                     3, 0 },
    /* HELI      */ { { {HE_BX,HE_BY,6}, {HE_MX,HE_MY,4},   {HE_TX,HE_TY,4} },                     3, 11 },
    /* FIGHTER   */ { { {FT_BX,FT_BY,6}, {FT_WRX,FT_WRY,3}, {FT_WLX,FT_WLY,3}, {FT_TX,FT_TY,4} }, 4, 0 },
    /* GLIDER    */ { { {GL_BX,GL_BY,6}, {GL_WX,GL_WY,4},   {GL_TX,GL_TY,4} },                     3, 0 },
};

static inline bool orb() { return s_theme == THEME_ORB; }

static void show(lv_obj_t *o, bool v) {
    if (!o) return;
    if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_color_t alt_color(float altFt, bool onGround) {
    // Military is night-vision: one phosphor green for every contact, the way an image
    // intensifier actually renders. Altitude survives as brightness rather than hue, so
    // the scope still reads high-versus-low without a five-colour key -- and the
    // emergency ring, being the only non-green thing on screen, becomes unmissable.
    if (s_theme == THEME_MILITARY) {
        if (onGround)      return lv_color_hex(0x2E7D46);
        if (altFt < 10000) return lv_color_hex(0x9BF0B2);
        if (altFt < 25000) return lv_color_hex(0x6FD98C);
        return lv_color_hex(0x4CBE6B);
    }
    if (onGround)      return lv_color_hex(0x888888);
    if (altFt < 3000)  return lv_color_hex(0xFF5A3C);
    if (altFt < 10000) return lv_color_hex(0xFFB23C);
    if (altFt < 20000) return lv_color_hex(0xC8FF3C);
    if (altFt < 30000) return lv_color_hex(0x39FF14);
    return lv_color_hex(0x3CE0FF);
}

static inline lv_point_t rim_point(float bearingDeg, float r) {
    const float a = bearingDeg * (float)M_PI / 180.0f;
    lv_point_t p;
    p.x = (lv_coord_t)lroundf((float)s_cx + r * sinf(a));
    p.y = (lv_coord_t)lroundf((float)s_cy - r * cosf(a));
    return p;
}

// rotate the local point (px,py) by `deg` (clockwise, screen coords) and offset to (ox,oy)
static inline lv_point_t rot_pt(float px, float py, float deg, lv_coord_t ox, lv_coord_t oy) {
    const float a = deg * (float)M_PI / 180.0f;
    const float c = cosf(a), s = sinf(a);
    lv_point_t p;
    p.x = (lv_coord_t)(ox + (lv_coord_t)lroundf(px * c - py * s));
    p.y = (lv_coord_t)(oy + (lv_coord_t)lroundf(px * s + py * c));
    return p;
}

// =============================== flow map ====================================
static void flow_draw_seg(const FlowSeg &s) {
    if (!s_flowCanvas) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = orb() ? ORB_FLOW : s_cRing;
    d.width = 2;
    d.opa = FLOW_OPA;
    lv_point_t pts[2] = { s.a, s.b };
    lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);
}

static void flow_redraw_all(void) {
    if (!s_flowCanvas) return;
    lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    for (const FlowSeg &s : s_flow) flow_draw_seg(s);
}

// =============================== grid ========================================
static void grid_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const lv_point_t c = { s_cx, s_cy };

    if (orb()) {
        lv_draw_line_dsc_t gl;
        lv_draw_line_dsc_init(&gl);
        gl.color = ORB_GRID;
        gl.width = 1;
        gl.opa = 120;
        const int step = 38;
        for (int x = s_cx % step; x < SCREEN_W; x += step) {
            lv_point_t p1 = { (lv_coord_t)x, 0 }, p2 = { (lv_coord_t)x, SCREEN_H - 1 };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        for (int y = s_cy % step; y < SCREEN_H; y += step) {
            lv_point_t p1 = { 0, (lv_coord_t)y }, p2 = { SCREEN_W - 1, (lv_coord_t)y };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        // center "you are here" triangle (orange, pointing up)
        lv_point_t tri[3] = { rot_pt(0, -11, 0, s_cx, s_cy),
                              rot_pt(10, 8, 0, s_cx, s_cy),
                              rot_pt(-10, 8, 0, s_cx, s_cy) };
        lv_draw_rect_dsc_t td;
        lv_draw_rect_dsc_init(&td);
        td.bg_color = ORB_ACCENT;
        td.bg_opa = LV_OPA_COVER;
        td.border_color = lv_color_hex(0x8A4A00);
        td.border_width = 1;
        td.border_opa = 160;
        coastline_draw(d, COAST_COLOR, 170, 2);    // landmass outline under the triangle
        if (s_airportsEnabled) {
            runways_draw(d, RUNWAY_COLOR, 150);
            airports_draw(d, AIRPORT_COLOR, 175, AIRPORT_LABEL_COLOR, AIRPORT_LABEL_OPA);
        }
            lv_draw_polygon(d, &td, tri, 3);
        return;
    }

    // coastline first, so the rings/crosshair sit cleanly on top of it.
    // Steel blue + 2 px so it reads as a map outline, distinct from the green altitude trails.
    coastline_draw(d, COAST_COLOR, 165, 2);
    if (s_airportsEnabled) {
            runways_draw(d, RUNWAY_COLOR, 150);
            airports_draw(d, AIRPORT_COLOR, 175, AIRPORT_LABEL_COLOR, AIRPORT_LABEL_OPA);
        }

    // phosphor: concentric rings + crosshair
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = s_cRing;
    ad.width = 2;
    const lv_coord_t rr[4] = { 50, 104, 160, RADAR_R_OUTER_PX };
    const lv_opa_t   ro[4] = { 66, 66, 66, 87 };
    for (int i = 0; i < 4; ++i) { ad.opa = ro[i]; lv_draw_arc(d, &ad, &c, rr[i], 0, 360); }

    lv_draw_line_dsc_t ll;
    lv_draw_line_dsc_init(&ll);
    ll.color = s_cRing;
    ll.width = 2;
    ll.opa = 41;
    lv_point_t h1 = { (lv_coord_t)(s_cx - 211), s_cy }, h2 = { (lv_coord_t)(s_cx + 211), s_cy };
    lv_point_t v1 = { s_cx, (lv_coord_t)(s_cy - 211) }, v2 = { s_cx, (lv_coord_t)(s_cy + 211) };
    lv_draw_line(d, &ll, &h1, &h2);
    lv_draw_line(d, &ll, &v1, &v2);
}

// =============================== sweep =======================================
// Sweep render cost, sampled so smoothness can be diagnosed from a number rather than
// an impression. draw_cb only fires when LVGL actually repaints, so counting it gives
// the real achieved rate -- not the rate the timer asks for.
static uint32_t s_drawUsAcc = 0, s_drawCnt = 0, s_perfMarkMs = 0;
static float    s_fps = 0.0f;
static float    s_stepAcc = 0.0f, s_stepMaxCur = 0.0f, s_stepAvg = 0.0f, s_stepMax = 0.0f;
static uint32_t s_dtAcc = 0, s_dtCnt = 0, s_dtAvg = 0;   // real tick interval, measured
static uint32_t s_stepCnt = 0;
static uint32_t s_drawUsAvg = 0;

static void sweep_draw_cb(lv_event_t *e) {
    if (orb()) return;
    const uint32_t t0 = micros();
    lv_draw_ctx_t *dctx = lv_event_get_draw_ctx(e);
    const lv_point_t center = { s_cx, s_cy };
    const float R = (float)RADAR_R_OUTER_PX;

    // Filled pie-slice triangles tiling the trail. The bands do not overlap, so the
    // painted area is the same whatever the count -- the cost is per-lv_draw_polygon
    // setup. 45 of them took 8-17 ms against a 20 ms budget, and because the angle is
    // clock-driven a missed deadline makes the beam jump rather than slow, which is what
    // reads as choppy. Fewer, wider bands buy headroom so every frame lands on time.
    const int steps = s_trailSteps;
    const float stepDeg = s_trailDeg / (float)steps;
    lv_draw_rect_dsc_t polyDsc;
    lv_draw_rect_dsc_init(&polyDsc);
    polyDsc.bg_color = s_cRing;

    for (int i = steps; i >= 1; --i) {
        const float frac = 1.0f - (float)i / (float)steps;
        const float a1 = s_sweepDeg - (float)i * stepDeg;
        const float a2 = s_sweepDeg - (float)(i - 1) * stepDeg;
        
        polyDsc.bg_opa = (lv_opa_t)(powf(frac, 1.8f) * 155.0f);
        if (polyDsc.bg_opa < 2) continue;

        lv_point_t pts[3];
        pts[0] = center;
        pts[1] = rim_point(a1, R);
        pts[2] = rim_point(a2, R);
        lv_draw_polygon(dctx, &polyDsc, pts, 3);
    }

    // Crisp leading beam line
    lv_draw_line_dsc_t le;
    lv_draw_line_dsc_init(&le);
    le.color = s_cLead;
    le.width = 2;
    le.opa = 245;
    le.round_start = 1;
    le.round_end = 1;
    lv_point_t lead = rim_point(s_sweepDeg, R);
    lv_draw_line(dctx, &le, &center, &lead);

    s_drawUsAcc += micros() - t0;
    if (++s_drawCnt >= 30) {
        const uint32_t now = millis();
        if (s_perfMarkMs) {
            const uint32_t el = now - s_perfMarkMs;
            if (el) s_fps = (float)s_drawCnt * 1000.0f / (float)el;
            s_drawUsAvg = s_drawUsAcc / s_drawCnt;
        }
        s_perfMarkMs = now;
        s_drawCnt = 0;
        s_drawUsAcc = 0;
    }
}

static void wedge_bbox(float deg, lv_area_t *out) {
    lv_coord_t minx = s_cx, maxx = s_cx, miny = s_cy, maxy = s_cy;
    const int steps = 10;
    for (int i = 0; i <= steps; ++i) {
        const float a = deg - s_trailDeg * (float)i / (float)steps;
        const lv_point_t p = rim_point(a, (float)RADAR_R_OUTER_PX);
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
    const lv_coord_t pad = 6;
    out->x1 = minx - pad; out->y1 = miny - pad;
    out->x2 = maxx + pad; out->y2 = maxy + pad;
}

// glyph + label bounding box (for partial invalidation during the glide).
// Must cover the label areas drawn in the aircraft layer (they grew for large-text mode).
// How far a floating label may be pushed from its glyph when routing around obstacles.
// Purely a taste limit now -- a label further than this from its aircraft stops reading
// as belonging to it -- since glyph_bbox() below tracks the real position rather than a
// worst case.
#define LBL_MAX_DX 135
#define LBL_MAX_DY 120

// Everything a contact paints: the glyph, plus its floating label wherever
// layout_labels() has put it. This box is BOTH the invalidate-on-move region and the
// per-band draw cull, so it has to cover the label *exactly*.
//
// It used to assume the label sat to the right (p.x-22 .. p.x+174) and was only widened
// for contacts near the chrome. Once labels could also route left to dodge each other,
// a left-placed callsign fell outside the box anywhere on the scope: LVGL repaints in
// horizontal bands, so it was culled mid-band and rendered in fragments -- "N181CA"
// arriving on screen as "A" -- and moving it left stale pixels behind.
static inline lv_area_t glyph_bbox(const AcDraw &ac, lv_point_t p) {
    lv_area_t a;
    if (orb()) { a.x1 = p.x - 30; a.y1 = p.y - 30; a.x2 = p.x + 30; a.y2 = p.y + 30; return a; }
    a.x1 = p.x - 22; a.y1 = p.y - 22; a.x2 = p.x + 22; a.y2 = p.y + 32;
    if (ac.lblW > 0) {
        const float gk = (float)SCREEN_W / (float)UI_DESIGN_W;
        if (ac.lblSet) {
            const lv_coord_t lx0 = (lv_coord_t)(p.x + ac.lblDx);
            const lv_coord_t ly0 = (lv_coord_t)(p.y - 14 * gk + ac.lblDy);
            const lv_coord_t lx1 = (lv_coord_t)(lx0 + ac.lblW);
            const lv_coord_t ly1 = (lv_coord_t)(p.y + 26 * gk + ac.lblDy);
            if (lx0 < a.x1) a.x1 = lx0;
            if (ly0 < a.y1) a.y1 = ly0;
            if (lx1 > a.x2) a.x2 = lx1;
            if (ly1 > a.y2) a.y2 = ly1;
        } else {
            // Not placed yet: cover the default right-hand spot it will be drawn in.
            a.x2 = (lv_coord_t)(p.x + 12 * gk + ac.lblW);
            a.y1 = (lv_coord_t)(p.y - 14 * gk);
            a.y2 = (lv_coord_t)(p.y + 26 * gk);
        }
    }
    a.x1 -= 2; a.y1 -= 2; a.x2 += 2; a.y2 += 2;   // slack for glyph antialiasing
    return a;
}

static inline void area_union(lv_area_t &d, const lv_area_t &s) {
    d.x1 = LV_MIN(d.x1, s.x1); d.y1 = LV_MIN(d.y1, s.y1);
    d.x2 = LV_MAX(d.x2, s.x2); d.y2 = LV_MAX(d.y2, s.y2);
}
static inline bool area_overlaps(const lv_area_t &a, const lv_area_t &b) {
    return !(a.x2 < b.x1 || a.x1 > b.x2 || a.y2 < b.y1 || a.y1 > b.y2);
}

// Advance each glyph from its previous position toward the new target (ease-out),
// invalidating only the small region each one occupies. Self-limiting: when a plane
// barely moves (far away / slow), nx==pos and it's skipped — near-zero cost.
static void layout_labels(void);   // defined below; run after every motion step

static void interp_step(void) {
#if MOTION_INTERP
    if (!s_acLayer || s_acs.empty()) return;
    const uint32_t now = lv_tick_get();
    float t = s_pollMs ? (float)(now - s_animStartMs) / (float)s_pollMs : 1.0f;
    if (t > 1.0f) t = 1.0f;
    const float e = t * (2.0f - t);                  // ease-out quad
    for (AcDraw &ac : s_acs) {
        const lv_coord_t nx = ac.from.x + (lv_coord_t)lroundf((float)(ac.to.x - ac.from.x) * e);
        const lv_coord_t ny = ac.from.y + (lv_coord_t)lroundf((float)(ac.to.y - ac.from.y) * e);
        if (nx == ac.pos.x && ny == ac.pos.y) continue;
        lv_point_t np; np.x = nx; np.y = ny;
        lv_area_t inv = glyph_bbox(ac, ac.pos);
        area_union(inv, glyph_bbox(ac, np));
        ac.pos = np;
        lv_obj_invalidate_area(s_acLayer, &inv);
    }
#endif
    layout_labels();
}

// Decide where every floating label sits. Deliberately NOT done in the draw callback:
// LVGL repaints the scope in horizontal bands and the draw loop culls contacts outside
// the current band, so a decision made there would see a different set of neighbours in
// each band and could place one label in two different spots within a single frame.
//
// Contacts are processed in s_acs order, which is nearest first, so when two labels
// collide the closer aircraft keeps the better spot.
static uint32_t s_lblUs = 0;        // last layout_labels() duration
static uint16_t s_lblMoves = 0;     // contacts repositioned in that pass
static uint16_t s_lblSeen = 0;      // contacts considered

static void layout_labels(void) {
    if (orb() || !s_acLayer) return;
    const uint32_t t0 = micros();
    uint16_t moves = 0, seen = 0;
    const float gk = (float)SCREEN_W / (float)UI_DESIGN_W;
    const lv_coord_t gap    = (lv_coord_t)(12 * gk);
    const lv_coord_t pad    = (lv_coord_t)(4 * gk);
    const lv_coord_t rGlass = (lv_coord_t)(SCREEN_W / 2 - 3);
    const int32_t UNPLACEABLE = 1 << 28;

    lv_area_t placed[28];
    int nPlaced = 0;

    for (AcDraw &ac : s_acs) {
        if (!ac.inRange || ac.lblW <= 0) continue;
        ++seen;
        const lv_coord_t wmax = ac.lblW;
        const lv_coord_t yTop = (lv_coord_t)(ac.pos.y - 14 * gk);
        const lv_coord_t yBot = (lv_coord_t)(ac.pos.y + 26 * gk);

        // Escape routes are measured against whichever obstacle the preferred placement
        // actually lands on -- a piece of chrome, or another aircraft's label.
        lv_area_t blocker = { 0, 0, -1, -1 };
        {
            const lv_coord_t rx0 = (lv_coord_t)(ac.pos.x + gap), rx1 = (lv_coord_t)(rx0 + wmax);
            for (int i = 0; i < radar::KEEPOUT_SLOTS && !keepout_live(blocker); ++i) {
                const lv_area_t &k = s_keepOut[i];
                if (keepout_live(k) && rx0 <= k.x2 && rx1 >= k.x1 && yTop <= k.y2 && yBot >= k.y1)
                    blocker = k;
            }
            for (int i = 0; i < nPlaced && !keepout_live(blocker); ++i) {
                const lv_area_t &k = placed[i];
                if (rx0 <= k.x2 && rx1 >= k.x1 && yTop <= k.y2 && yBot >= k.y1) blocker = k;
            }
        }
        const lv_coord_t bx1 = keepout_live(blocker) ? blocker.x1 : (lv_coord_t)0;
        const lv_coord_t bx2 = keepout_live(blocker) ? blocker.x2 : (lv_coord_t)0;

        struct Cand { lv_coord_t x0, dy; };
        const lv_coord_t midX = (lv_coord_t)(ac.pos.x - wmax / 2);
        const Cand cands[] = {
            { (lv_coord_t)(ac.pos.x + gap),        0 },
            { (lv_coord_t)(ac.pos.x - gap - wmax), 0 },
            { (lv_coord_t)(bx2 + pad),             0 },
            { (lv_coord_t)(bx1 - pad - wmax),      0 },
            { midX, (lv_coord_t)(blocker.y1 - pad - yBot) },
            { midX, (lv_coord_t)(blocker.y2 + pad - yTop) },
        };

        auto costOf = [&](lv_coord_t x0, lv_coord_t dy) -> int32_t {
            const lv_coord_t x1 = (lv_coord_t)(x0 + wmax);
            const lv_coord_t y0 = (lv_coord_t)(yTop + dy), y1 = (lv_coord_t)(yBot + dy);
            // glyph_bbox() is the draw cull, so a label outside it is dropped, not moved.
            if (x0 < ac.pos.x - LBL_MAX_DX || x1 > ac.pos.x + 180 ||
                dy < -LBL_MAX_DY || dy > LBL_MAX_DY) return UNPLACEABLE;
            const lv_coord_t xs2[2] = { x0, x1 }, ys2[2] = { y0, y1 };
            for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) {
                const int32_t dx = xs2[i] - s_cx, dyy = ys2[j] - s_cy;
                if (dx * dx + dyy * dyy > (int32_t)rGlass * rGlass) return UNPLACEABLE;
            }
            int32_t cover = 0;
            for (int i = 0; i < radar::KEEPOUT_SLOTS; ++i) {
                const lv_area_t &k = s_keepOut[i];
                if (!keepout_live(k)) continue;
                const int32_t ow = LV_MIN(x1, k.x2) - LV_MAX(x0, k.x1);
                const int32_t oh = LV_MIN(y1, k.y2) - LV_MAX(y0, k.y1);
                if (ow > 0 && oh > 0) cover += ow * oh;
            }
            for (int i = 0; i < nPlaced; ++i) {
                const int32_t ow = LV_MIN(x1, placed[i].x2) - LV_MAX(x0, placed[i].x1);
                const int32_t oh = LV_MIN(y1, placed[i].y2) - LV_MAX(y0, placed[i].y1);
                if (ow > 0 && oh > 0) cover += ow * oh;
            }
            return cover;
        };

        // Stay put unless staying put is actually bad. This runs on every motion step, so
        // re-deciding from scratch let a one-pixel drift flip a label to another candidate
        // and back again -- on screen the callsigns visibly danced around even where
        // nothing overlapped. A clean placement is never disturbed, and a dirty one only
        // moves for a clearly better option, so a marginal score cannot start an
        // oscillation.
        const int32_t curCost = ac.lblSet ? costOf((lv_coord_t)(ac.pos.x + ac.lblDx), ac.lblDy)
                                          : UNPLACEABLE + 1;
        lv_coord_t bestX = (lv_coord_t)(ac.pos.x + ac.lblDx), bestDy = ac.lblDy;
        int32_t best = curCost;
        if (curCost != 0) {
            const int32_t margin = (int32_t)(wmax * 4);   // ~4 text rows' worth of cover
            for (const Cand &c : cands) {
                const int32_t score = costOf(c.x0, c.dy);
                const int32_t bar = ac.lblSet ? (best - margin) : best;
                if (score < bar) { best = score; bestX = c.x0; bestDy = c.dy; }
                if (best == 0) break;              // clean; earlier candidates win ties
            }
            if (!ac.lblSet && best > UNPLACEABLE) { bestX = cands[0].x0; bestDy = 0; }
        }

        const lv_coord_t ndx = (lv_coord_t)(bestX - ac.pos.x);
        if (!ac.lblSet || ndx != ac.lblDx || bestDy != ac.lblDy) {
            // Repaint what it is leaving as well as where it lands, and do it here: a
            // label can move because a *neighbour* moved while this glyph sits perfectly
            // still, and interp_step only invalidates contacts it actually moved.
            lv_area_t inv = glyph_bbox(ac, ac.pos);
            ac.lblDx = ndx;
            ac.lblDy = bestDy;
            ac.lblSet = true;
            area_union(inv, glyph_bbox(ac, ac.pos));
            lv_obj_invalidate_area(s_acLayer, &inv);
            ++moves;
        }
        if (nPlaced < (int)(sizeof(placed) / sizeof(placed[0]))) {
            lv_area_t r = { bestX, (lv_coord_t)(yTop + bestDy),
                            (lv_coord_t)(bestX + wmax), (lv_coord_t)(yBot + bestDy) };
            placed[nPlaced++] = r;
        }
    }
    s_lblUs = micros() - t0;
    s_lblMoves = moves;
    s_lblSeen = seen;
}

static void sweep_timer_cb(lv_timer_t *t) {
    (void)t;
    if (++s_frameCtr % 3 == 0) interp_step();         // smooth glyph motion (~90 ms cadence)
    if (orb()) {
        // animate the blip waves (invalidate only the ball areas)
        s_wavePhase += 0.05f;
        if (s_wavePhase >= 1.0f) s_wavePhase -= 1.0f;
        if (!s_acLayer) return;
        int balls = 0;
        for (const AcDraw &ac : s_acs) {
            if (!ac.inRange) continue;
            if (balls >= ORB_BLIPS) break;
            balls++;
            lv_area_t a = { (lv_coord_t)(ac.pos.x - 44), (lv_coord_t)(ac.pos.y - 44),
                            (lv_coord_t)(ac.pos.x + 44), (lv_coord_t)(ac.pos.y + 44) };
            lv_obj_invalidate_area(s_acLayer, &a);
        }
        return;
    }
    if (!s_sweepEnabled) return;          // sweep disabled: glyph interpolation above still runs
    s_prevSweepDeg = s_sweepDeg;
    // Fixed increment per rendered frame -- NOT elapsed time.
    //
    // Time-based advance was tried here and reverted: it looked correct on paper, and the
    // clamp pinned the step to its maximum whenever a frame took longer than ~50 ms, but
    // frames on this panel range 57-118 ms. Every frame under that threshold advanced
    // less than a full step, so the step varied frame to frame. That variance is small
    // enough to hide in the averages and plainly visible on the panel.
    //
    // A fixed increment has zero step variance by construction. The cost is that
    // revolution time follows frame rate rather than the nominal period, which is the
    // trade this project has repeatedly chosen: even motion beats a correct period.
    //
    // The size still comes from the pixel setting, so one number means the same thing on
    // both panels: the same angle is ~11 px of rim travel at 466 and ~18 px at 720.
    {
        const uint32_t now = millis();
        static uint32_t s_lastSweepMs = 0;
        if (s_lastSweepMs) {
            const uint32_t dt = now - s_lastSweepMs;
            s_dtAcc += dt;
            if (++s_dtCnt >= 40) { s_dtAvg = s_dtAcc / s_dtCnt; s_dtAcc = 0; s_dtCnt = 0; }
        }
        s_lastSweepMs = now;

        const float stepDeg = s_maxStepPx * 180.0f
                              / (3.14159265f * (float)RADAR_R_OUTER_PX);
        s_sweepDeg += stepDeg;
        if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
    }

    // How far the beam actually moved. Kept as telemetry even though the increment is
    // fixed: it is what proves the step is not varying.
    {
        float d = s_sweepDeg - s_prevSweepDeg;
        if (d < 0) d += 360.0f;
        if (d > 0.0f && d < 90.0f) {
            s_stepAcc += d;
            if (d > s_stepMaxCur) s_stepMaxCur = d;
            if (++s_stepCnt >= 40) {
                s_stepAvg = s_stepAcc / (float)s_stepCnt;
                s_stepMax = s_stepMaxCur;
                s_stepAcc = 0.0f; s_stepCnt = 0; s_stepMaxCur = 0.0f;
            }
        }
    }
    if (!s_sweep) return;
    lv_area_t a, b, area;
    wedge_bbox(s_prevSweepDeg, &a);
    wedge_bbox(s_sweepDeg, &b);
    area.x1 = LV_MIN(a.x1, b.x1);
    area.y1 = LV_MIN(a.y1, b.y1);
    area.x2 = LV_MAX(a.x2, b.x2);
    area.y2 = LV_MAX(a.y2, b.y2);
    lv_obj_invalidate_area(s_sweep, &area);
}

// =============================== aircraft ====================================
static void draw_trail(lv_draw_ctx_t *d, const AcDraw &ac, lv_color_t col) {
    const int n = (int)ac.trail.size();
    if (n < 2) return;
    lv_draw_line_dsc_t t;
    lv_draw_line_dsc_init(&t);
    t.color = col;
    t.width = 2;
    for (int i = 1; i < n; ++i) {
        t.opa = (lv_opa_t)(10 + 45 * i / n);
        lv_point_t a = ac.trail[i - 1], b = ac.trail[i];
        lv_draw_line(d, &t, &a, &b);
    }
}

static void draw_ball(lv_draw_ctx_t *d, const AcDraw &ac) {
    // emitted waves: several expanding rings (sonar-ping look)
    lv_draw_arc_dsc_t w;
    lv_draw_arc_dsc_init(&w);
    w.color = ORB_ACCENT;
    w.width = 3;
    for (int wv = 0; wv < 3; ++wv) {
        float ph = s_wavePhase + (float)wv * 0.34f;
        if (ph >= 1.0f) ph -= 1.0f;
        w.opa = (lv_opa_t)((1.0f - ph) * 245.0f);
        if (w.opa > 6) lv_draw_arc(d, &w, &ac.pos, (uint16_t)(BALL_R + 3 + ph * WAVE_EXPAND), 0, 360);
    }

    // the ball
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    b.border_color = lv_color_hex(0x7A5A00);
    b.border_width = 1;
    b.border_opa = 150;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - BALL_R), (lv_coord_t)(ac.pos.y - BALL_R),
                    (lv_coord_t)(ac.pos.x + BALL_R), (lv_coord_t)(ac.pos.y + BALL_R) };
    lv_draw_rect(d, &b, &r);

    // glossy highlight
    lv_draw_rect_dsc_t hl;
    lv_draw_rect_dsc_init(&hl);
    hl.bg_color = lv_color_hex(0xFFFBCC);
    hl.bg_opa = 170;
    hl.radius = LV_RADIUS_CIRCLE;
    lv_area_t hr = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 6),
                     (lv_coord_t)(ac.pos.x - 1), (lv_coord_t)(ac.pos.y - 2) };
    lv_draw_rect(d, &hl, &hr);
}

static void draw_offrange(lv_draw_ctx_t *d, const AcDraw &ac) {
    // small ball at the rim
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 5),
                    (lv_coord_t)(ac.pos.x + 5), (lv_coord_t)(ac.pos.y + 5) };
    lv_draw_rect(d, &b, &r);

    // small orange triangle just outside it, pointing toward the aircraft's bearing
    const lv_coord_t ox = (lv_coord_t)lroundf(ac.pos.x + 12.0f * sinf(ac.bearingDeg * (float)M_PI / 180.0f));
    const lv_coord_t oy = (lv_coord_t)lroundf(ac.pos.y - 12.0f * cosf(ac.bearingDeg * (float)M_PI / 180.0f));
    lv_point_t tri[3] = { rot_pt(0, -7, ac.bearingDeg, ox, oy),
                          rot_pt(5, 4, ac.bearingDeg, ox, oy),
                          rot_pt(-5, 4, ac.bearingDeg, ox, oy) };
    lv_draw_rect_dsc_t td;
    lv_draw_rect_dsc_init(&td);
    td.bg_color = ORB_ACCENT;
    td.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(d, &td, tri, 3);
}

static void ac_draw_cb(lv_event_t *e) {
    // Glyphs, labels and markers are all drawn from coordinates chosen against the 466 px
    // panel. Rotation was applied but never scale, so on a larger screen the aircraft and
    // their labels stayed the same physical size and read as tiny against a wider scope.
    const float gk = (float)SCREEN_W / (float)UI_DESIGN_W;
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const bool drg = orb();
    int balls = 0, arrows = 0;

    // Marine mode replaces the air picture rather than overlaying it: ships and
    // aircraft share no scale, altitude or speed frame, so a combined plot reads as
    // noise. Vessels live on this layer because AIS updates arrive continuously and
    // this layer is already invalidated every poll.
    if (s_trafficMode == radar::TRAFFIC_MARINE) {
        vessel_draw(d, 220);
        return;
    }

    for (const AcDraw &ac : s_acs) {
        // Decide eligibility and claim an Orb blip/arrow slot FIRST. This callback runs
        // once per invalidated region, so the budget must be spent in the same order
        // every time — clipping before counting would let a different set of aircraft
        // qualify depending on which region is being repainted, and they would flicker.
        bool eligible;
        if (drg) {
            if (ac.inRange) { eligible = (balls  < ORB_BLIPS);  if (eligible) balls++; }
            else            { eligible = (arrows < ORB_ARROWS); if (eligible) arrows++; }
        } else {
            eligible = ac.inRange;                // phosphor shows in-range traffic only
        }
        if (!eligible) continue;

        // Now skip the drawing work for anything outside the region being repainted.
        // The sweep invalidates a large box every frame; without this, every aircraft
        // was redrawn for every region even when nowhere near it.
        if (d->clip_area) {
            const lv_area_t bb = glyph_bbox(ac, ac.pos);
            if (!area_overlaps(bb, *d->clip_area)) continue;
        }

        if (drg) {
            if (ac.inRange) {
                draw_trail(d, ac, ORB_FLOW);
                draw_ball(d, ac);
            } else {
                draw_offrange(d, ac);
            }
        } else {
            // Target Phosphor Glow: Calculate opacity based on angular distance to sweep beam
            lv_opa_t targetOpa = 100; // Baseline dim phosphor opacity (~39%)
            if (s_sweepEnabled) {
                const float dx = (float)(ac.pos.x - s_cx);
                const float dy = (float)(ac.pos.y - s_cy);
                float acDeg = atan2f(dy, dx) * 180.0f / (float)M_PI + 90.0f;
                if (acDeg < 0.0f) acDeg += 360.0f;
                float delta = fmodf(s_sweepDeg - acDeg + 360.0f, 360.0f);
                if (delta < 55.0f) {
                    float glowFrac = 1.0f - (delta / 55.0f);
                    // Smooth quadratic decay from 255 (100% full bright glow) down to 100 (39% dim)
                    targetOpa = (lv_opa_t)(100.0f + 155.0f * (glowFrac * glowFrac));

                    // Draw soft glowing phosphor radial halo surrounding blip (matching sample 2 & sample 3)
                    lv_draw_arc_dsc_t haloDsc;
                    lv_draw_arc_dsc_init(&haloDsc);
                    haloDsc.color = ac.color;
                    haloDsc.width = (uint16_t)(3 + (uint16_t)(glowFrac * 5.0f));
                    haloDsc.opa = (lv_opa_t)(glowFrac * glowFrac * 210.0f);
                    if (haloDsc.opa > 10) {
                        lv_draw_arc(d, &haloDsc, &ac.pos, 10, 0, 360);
                    }
                }
            } else {
                targetOpa = LV_OPA_COVER;
            }

            draw_trail(d, ac, ac.color);

            const float th = ((ac.track != ac.track) ? 0.0f : ac.track) * (float)M_PI / 180.0f;
            const float c = cosf(th), s = sinf(th);

            lv_draw_rect_dsc_t g;
            lv_draw_rect_dsc_init(&g);
            g.bg_color = ac.color;
            g.bg_opa = targetOpa;

            if (s_typeIcons) {
                const AcShape &sh = AC_SHAPES[ac.cat < AC_CAT_COUNT ? ac.cat : AC_CAT_NARROW];
                lv_point_t pts[8];
                for (int p = 0; p < sh.nParts; ++p) {
                    const AcPoly &poly = sh.parts[p];
                    for (int i = 0; i < poly.n; ++i) {
                        const float x = (poly.x[i] * c - poly.y[i] * s) * gk;
                        const float y = (poly.x[i] * s + poly.y[i] * c) * gk;
                        pts[i].x = (lv_coord_t)(ac.pos.x + (lv_coord_t)lroundf(x));
                        pts[i].y = (lv_coord_t)(ac.pos.y + (lv_coord_t)lroundf(y));
                    }
                    lv_draw_polygon(d, &g, pts, poly.n);
                }
                if (sh.rotorR) {                       // helicopter: rotor disc over the pod
                    lv_draw_arc_dsc_t r;
                    lv_draw_arc_dsc_init(&r);
                    r.color = ac.color;
                    r.width = (lv_coord_t)(2 * gk);
                    r.opa = 150;
                    lv_draw_arc(d, &r, &ac.pos, (lv_coord_t)(sh.rotorR * gk), 0, 360);
                }
            } else {
                lv_point_t pts[4];
                for (int i = 0; i < 4; ++i) {
                    const float x = (GX[i] * c - GY[i] * s) * gk;
                    const float y = (GX[i] * s + GY[i] * c) * gk;
                    pts[i].x = (lv_coord_t)(ac.pos.x + (lv_coord_t)lroundf(x));
                    pts[i].y = (lv_coord_t)(ac.pos.y + (lv_coord_t)lroundf(y));
                }
                lv_draw_polygon(d, &g, pts, 4);
            }
            if (ac.emergency) {
                lv_draw_arc_dsc_t h;
                lv_draw_arc_dsc_init(&h);
                h.color = COL_EMERG; h.width = (lv_coord_t)(2 * gk); h.opa = 200;
                lv_draw_arc(d, &h, &ac.pos, (lv_coord_t)(16 * gk), 0, 360);
            }
            if (ac.military || s_milPreview) {
                // Targeting-style corner brackets: four short L pairs at the corners of
                // a box around the glyph. Deliberately angular so it reads differently
                // from the emergency circle even at a glance.
                lv_draw_line_dsc_t m;
                lv_draw_line_dsc_init(&m);
                m.color = MIL_COLOR; m.width = (lv_coord_t)(2 * gk); m.opa = 225;
                const lv_coord_t R = (lv_coord_t)(17 * gk), L = (lv_coord_t)(6 * gk);
                const lv_coord_t xs[2] = { (lv_coord_t)(ac.pos.x - R), (lv_coord_t)(ac.pos.x + R) };
                const lv_coord_t ys[2] = { (lv_coord_t)(ac.pos.y - R), (lv_coord_t)(ac.pos.y + R) };
                for (int iy = 0; iy < 2; ++iy) {
                    for (int ix = 0; ix < 2; ++ix) {
                        const lv_coord_t sx = ix ? -L : L, sy = iy ? -L : L;
                        lv_point_t c = { xs[ix], ys[iy] };
                        lv_point_t hx = { (lv_coord_t)(xs[ix] + sx), ys[iy] };
                        lv_point_t vy = { xs[ix], (lv_coord_t)(ys[iy] + sy) };
                        lv_draw_line(d, &m, &c, &hx);
                        lv_draw_line(d, &m, &c, &vy);
                    }
                }
            }
        }

        // selection ring(s)
        if (!s_selHex.empty() && s_selHex == ac.hex) {
            lv_draw_arc_dsc_t sr;
            lv_draw_arc_dsc_init(&sr);
            sr.width = 2;
            sr.opa = 240;
            if (drg) {
                sr.color = ORB_ACCENT;
                lv_draw_arc(d, &sr, &ac.pos, 15, 0, 360);
                lv_draw_arc(d, &sr, &ac.pos, 23, 0, 360);
            } else {
                sr.color = ac.emergency ? COL_EMERG : s_cInk;
                lv_draw_arc(d, &sr, &ac.pos, (lv_coord_t)(19 * gk), 0, 360);
            }
        }

        // floating labels (phosphor only; orb keeps clean balls + the tap card)
        if (!drg) {
            // Bigger panel gets a bigger label font as well as a scaled offset -- keeping
            // 14 px text beside a glyph half again as large reads as an afterthought.
            const bool bigPanel = (SCREEN_W >= 600);
            const lv_font_t *fc = (s_bigText || bigPanel) ? &lv_font_montserrat_18 : &lv_font_montserrat_14;
            const lv_font_t *fa = (s_bigText || bigPanel) ? &lv_font_montserrat_16 : &lv_font_montserrat_12;

            // Placement was decided in layout_labels() -- see there for why it cannot
            // be done here. This just reads it back.
            const lv_coord_t wmax = ac.lblW;
            const lv_coord_t yTop = (lv_coord_t)(ac.pos.y - 14 * gk);
            const lv_coord_t yBot = (lv_coord_t)(ac.pos.y + 26 * gk);
            const lv_coord_t px = (lv_coord_t)(ac.pos.x + ac.lblDx), pdy = ac.lblDy;

            lv_draw_label_dsc_t lc;
            lv_draw_label_dsc_init(&lc);
            lc.font = fc;
            lc.color = s_cInk;
            lv_area_t a1 = { px, (lv_coord_t)(yTop + pdy),
                             (lv_coord_t)(px + wmax), (lv_coord_t)(ac.pos.y + 4 * gk + pdy) };
            if (ac.call[0]) lv_draw_label(d, &lc, &a1, ac.call, NULL);
            lv_draw_label_dsc_t la;
            lv_draw_label_dsc_init(&la);
            la.font = fa;
            la.color = ac.color;
            lv_area_t a2 = { px, (lv_coord_t)(ac.pos.y + 4 * gk + pdy),
                             (lv_coord_t)(px + wmax), (lv_coord_t)(yBot + pdy) };
            if (ac.altTxt[0]) lv_draw_label(d, &la, &a2, ac.altTxt, NULL);
        }
    }
}

// =============================== helpers =====================================
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                            lv_color_t color, lv_align_t align, lv_coord_t dx, lv_coord_t dy) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, align, dx, dy);
    return l;
}

static lv_obj_t *make_layer(lv_obj_t *parent, lv_event_cb_t draw_cb) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCREEN_W, SCREEN_H);
    lv_obj_center(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (draw_cb) lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

static void pulse_start(void);

static void pulse_anim_cb(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    const lv_coord_t dia = 10 + (lv_coord_t)((v * 44) / 100);
    lv_obj_set_size(o, dia, dia);
    lv_obj_center(o);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 220 / 100), 0);
}

static void pulse_start(void) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pulse);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

namespace radar {

void setTheme(int t) {
    s_theme = ((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
    const bool drg = orb();

    switch (s_theme) {                          // pick the scope chrome palette
        case THEME_AMBER:
            s_accentHex = 0xFFB23C;
            s_cRing = lv_color_hex(0xFFB23C); s_cLead = lv_color_hex(0xFFD27A);
            s_cInk  = lv_color_hex(0xFFE9C2); s_cSoft = lv_color_hex(0xFFC98A); break;
        case THEME_RED:
            s_accentHex = 0xE0323C;
            // Deep red chrome. Red preserves night vision, which is the point, but it
            // also collides with the low-altitude end of the aircraft palette -- so the
            // chrome sits darker than the amber theme's to keep traffic reading on top.
            s_cRing = lv_color_hex(0xE0323C); s_cLead = lv_color_hex(0xFF6B6B);
            s_cInk  = lv_color_hex(0xFFE0E0); s_cSoft = lv_color_hex(0xFF9B9B); break;
        case THEME_MILITARY:
            s_accentHex = 0x49C46B;
            s_cRing = lv_color_hex(0x49C46B); s_cLead = lv_color_hex(0x76E08C);
            s_cInk  = lv_color_hex(0xE0FFE6); s_cSoft = lv_color_hex(0x9FD7A8); break;
        default:                                // phosphor (orb uses its own colors)
            s_accentHex = 0x1DFF86;
            s_cRing = COL_GREEN; s_cLead = COL_LEAD; s_cInk = COL_INK; s_cSoft = COL_SOFT; break;
    }

    if (s_parent) {
        if (drg) {
            lv_obj_set_style_bg_color(s_parent, ORB_BG_TOP, 0);
            lv_obj_set_style_bg_grad_color(s_parent, ORB_BG_BOT, 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_VER, 0);
        } else {
            lv_obj_set_style_bg_color(s_parent, lv_color_black(), 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_NONE, 0);
        }
        lv_obj_set_style_bg_opa(s_parent, LV_OPA_COVER, 0);
    }
    for (int i = 0; i < 4; ++i) show(s_rose[i], !drg);   // hide compass in Orb
    show(s_rangeLbl, !drg && s_rangeLblVisible);
    show(s_centerDot, !drg);                             // orb draws an orange triangle instead
    show(s_pulse, !drg);

    // retint the persistent chrome objects for the active palette
    for (int i = 0; i < 4; ++i) if (s_rose[i]) lv_obj_set_style_text_color(s_rose[i], s_cSoft, 0);
    if (s_centerDot) lv_obj_set_style_bg_color(s_centerDot, s_cInk, 0);
    if (s_pulse)     lv_obj_set_style_border_color(s_pulse, s_cInk, 0);
    if (s_rangeLbl)  lv_obj_set_style_text_color(s_rangeLbl, s_cRing, 0);

    flow_redraw_all();
    if (s_parent) lv_obj_invalidate(s_parent);
    if (s_themeCb) s_themeCb(s_theme);
}

int  theme() { return s_theme; }
void cycleTheme() { setTheme(s_theme + 1); }
void setThemeChangedCb(void (*cb)(int)) { s_themeCb = cb; }
void setRangeLabelVisible(bool v) { s_rangeLblVisible = v; if (s_rangeLbl) show(s_rangeLbl, v && !orb()); }

// Park every free-running animation on the scope. The screenshot regression net renders
// each screen and diffs it pixel-for-pixel, and both the sweep wedge and the home-marker
// pulse are on infinite timers -- without this the same build yields a different image on
// every run (it showed up as ~18 pixels drifting in a box around the centre marker).
// Only the simulator calls this; the sweep toggle in settings stays a separate setting.
void labelPerf(uint32_t *us, uint16_t *moves, uint16_t *seen) {
    if (us) *us = s_lblUs;
    if (moves) *moves = s_lblMoves;
    if (seen) *seen = s_lblSeen;
}

void setLabelKeepOut(int slot, int x1, int y1, int x2, int y2) {
    if (slot < 0 || slot >= radar::KEEPOUT_SLOTS) return;
    lv_area_t &k = s_keepOut[slot];
    const bool changed = k.x1 != (lv_coord_t)x1 || k.y1 != (lv_coord_t)y1 ||
                         k.x2 != (lv_coord_t)x2 || k.y2 != (lv_coord_t)y2;
    k.x1 = (lv_coord_t)x1; k.y1 = (lv_coord_t)y1;
    k.x2 = (lv_coord_t)x2; k.y2 = (lv_coord_t)y2;
    // The card appears and disappears under the user's finger, and labels near it move
    // when it does. Repaint the scope so they are not left drawn at the old spot.
    if (changed && s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setStillMode(bool on) {
    setSweepEnabled(!on);
    if (!s_pulse) return;
    lv_anim_del(s_pulse, pulse_anim_cb);
    if (on) pulse_anim_cb(s_pulse, 0);   // park at the start of the cycle
    else    pulse_start();
}

void setSweepEnabled(bool on) {
    s_sweepEnabled = on;
    if (s_sweep) {
        show(s_sweep, on);
        if (!on) lv_obj_invalidate(s_sweep);   // clear any wedge currently painted
    }
}
bool sweepEnabled() { return s_sweepEnabled; }

void setAirportsEnabled(bool on) {
    s_airportsEnabled = on;
    if (s_gridLayer) lv_obj_invalidate(s_gridLayer);   // repaint the chrome with/without markers
}
bool airportsEnabled() { return s_airportsEnabled; }

uint32_t themeAccent() { return s_accentHex; }

void setTrafficMode(int mode) {
    s_trafficMode = (mode == TRAFFIC_MARINE) ? TRAFFIC_MARINE : TRAFFIC_AIR;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}
int trafficMode() { return s_trafficMode; }

void setMilitaryPreview(bool on) {
    s_milPreview = on;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setTypeIcons(bool on) {
    s_typeIcons = on;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}
bool typeIcons() { return s_typeIcons; }

// 0 = off, 1 = short, 2 = medium (default), 3 = long. Controls both the per-aircraft
// trail and the persistent flow layer (the long-lived "where everything has been" tracks).
void setTrailLength(int level) {
    switch (level) {
        case 0: s_trailMax = 0;  s_flowMax = 0;    s_flowGenMax = 0;  break;
        case 1: s_trailMax = 3;  s_flowMax = 150;  s_flowGenMax = 8;  break;   // ~16 s
        case 3: s_trailMax = 12; s_flowMax = 1500; s_flowGenMax = 30; break;   // ~60 s
        default: s_trailMax = 7; s_flowMax = 700;  s_flowGenMax = 14; break;   // ~28 s
    }
    if (s_flowMax == 0) { s_flow.clear(); s_trails.clear(); }
    else while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
    // With trails off the canvas is fully transparent but LVGL still reads and blends
    // every pixel of it inside each redrawn region. Hiding it removes that cost entirely.
    show(s_flowCanvas, s_flowMax > 0);
    flow_redraw_all();                              // repaint the flow canvas at the new length
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setMaxOnScreen(int n) {
    s_maxOnScreen = (n < 1) ? 1 : (n > ADSB_MAX_AIRCRAFT ? ADSB_MAX_AIRCRAFT : n);  // never more than the feed pulls
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setLargeText(bool on) {
    s_bigText = on;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void init(void *lv_parent) {
    lv_obj_t *parent = (lv_obj_t *)lv_parent;
    s_parent = parent;
    s_cx = SCREEN_CX;
    s_cy = SCREEN_CY;
    s_acs.clear();
    s_trails.clear();
    s_flow.clear();
    s_selHex.clear();
    s_flowRedrawCtr = 0;

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    if (!s_flowBuf) {
        const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
        s_flowBuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_flowBuf = (lv_color_t *)malloc(sz);
#endif
    }
    s_flowCanvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_flowCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (s_flowBuf) {
        lv_canvas_set_buffer(s_flowCanvas, s_flowBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
        lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    }
    lv_obj_center(s_flowCanvas);

    // Basemap sits at the very bottom: created before the other layers so the flow
    // canvas, chrome, sweep and glyphs all paint over it.
    s_mapCanvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_mapCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_mapCanvas);
    lv_obj_add_flag(s_mapCanvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(s_mapCanvas);

    s_gridLayer = make_layer(parent, grid_draw_cb);
    s_sweep     = make_layer(parent, sweep_draw_cb);
    s_acLayer   = make_layer(parent, ac_draw_cb);

    s_rose[0] = make_label(parent, "N", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_TOP_MID,    0, 12);
    s_rose[1] = make_label(parent, "S", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_rose[2] = make_label(parent, "E", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_RIGHT_MID, -12, 0);
    s_rose[3] = make_label(parent, "W", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_LEFT_MID,   12, 0);

    char rng[16];
    snprintf(rng, sizeof(rng), "%.0f km", (double)RANGE_KM_DEFAULT);
    s_rangeLbl = make_label(parent, rng, &lv_font_montserrat_14, COL_GREEN, LV_ALIGN_CENTER, 92, -8);
    lv_obj_set_style_text_opa(s_rangeLbl, 128, 0);

    s_pulse = lv_obj_create(parent);
    lv_obj_remove_style_all(s_pulse);
    lv_obj_set_size(s_pulse, 12, 12);
    lv_obj_center(s_pulse);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pulse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pulse, COL_INK, 0);
    lv_obj_set_style_border_width(s_pulse, 2, 0);
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    pulse_start();

    s_centerDot = lv_obj_create(parent);
    lv_obj_remove_style_all(s_centerDot);
    lv_obj_set_size(s_centerDot, 7, 7);
    lv_obj_center(s_centerDot);
    lv_obj_set_style_radius(s_centerDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_centerDot, COL_INK, 0);
    lv_obj_set_style_bg_opa(s_centerDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_centerDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_sweepDeg = 0.0f;
    s_prevSweepDeg = 0.0f;
    if (!s_timer) s_timer = lv_timer_create(sweep_timer_cb, SWEEP_FRAME_MS, nullptr);

    setTheme(s_theme);
}

void update(const std::vector<Aircraft> &aircraft, const RadarSettings &s) {
    std::vector<AcDraw> out;
    out.reserve(aircraft.size());
    std::set<std::string> present;
    const float R = (float)RADAR_R_OUTER_PX;
    ++s_flowGen;                                  // one tick per poll; flow segments age in these units

    // Reproject the coastline only when the scope geometry actually changes (home
    // moved or range zoomed) — never per frame. Then repaint the static chrome layer.
    static double s_coLat = 1e9, s_coLon = 1e9; static float s_coRange = -1.0f;
    if (s.homeLat != s_coLat || s.homeLon != s_coLon || s.rangeKm != s_coRange) {
        const bool firstFix = (s_coRange < 0.0f);
        s_coLat = s.homeLat; s_coLon = s.homeLon; s_coRange = s.rangeKm;
        coastline_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        airports_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
    runways_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        if (s_gridLayer) lv_obj_invalidate(s_gridLayer);
        if (!firstFix) {
            // Scope scale/center changed: old trails were plotted at the previous
            // projection and would be wrong now — drop them and clear the flow layer.
            s_trails.clear();
            s_flow.clear();
            flow_redraw_all();
        }
    }

    // Basemap: show it only while the committed image matches the live scope. After a
    // zoom the old image is geometrically wrong, so it hides until the rebuild lands.
    if (s_mapCanvas) {
        const uint16_t *px = nullptr;
        double mlat = 0, mlon = 0;
        float mrange = 0;
        uint32_t ver = 0;
        static uint32_t shownVer = 0;
        const bool have = map_bg_front(&px, &mlat, &mlon, &mrange, &ver) && px;
        const bool matches = have && fabs(mlat - s.homeLat) < 0.02 &&
                             fabs(mlon - s.homeLon) < 0.02 &&
                             fabsf(mrange - s.rangeKm) < 0.5f;
        if (matches) {
            lv_opa_t lvOpa = (lv_opa_t)((s_mapOpacity * 255) / 100);
            lv_obj_set_style_img_opa(s_mapCanvas, lvOpa, LV_PART_MAIN);
            if (ver != shownVer) {
                lv_canvas_set_buffer(s_mapCanvas, (void *)px, MAP_BG_SIZE, MAP_BG_SIZE,
                                     LV_IMG_CF_TRUE_COLOR);
                lv_obj_set_size(s_mapCanvas, MAP_BG_SIZE, MAP_BG_SIZE);
                lv_obj_center(s_mapCanvas);
                lv_obj_move_background(s_mapCanvas);
                shownVer = ver;
            }
            lv_obj_clear_flag(s_mapCanvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_mapCanvas);
        } else {
            lv_obj_add_flag(s_mapCanvas, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Only project vessels when they're the picture being shown. In aircraft mode they
    // are never drawn, so re-projecting every AIS contact each poll is pure waste.
    if (s_trafficMode == radar::TRAFFIC_MARINE)
        vessel_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);

    std::map<std::string, lv_point_t> prevPos;        // smooth-motion: glide starts here
    for (const AcDraw &a : s_acs) prevPos[a.hex] = a.pos;

    for (const Aircraft &ac : aircraft) {
        const double distKm = geo::haversineKm(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const double brg = geo::bearingDeg(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const geo::Point p = geo::projectToScreen(distKm, brg, s.rangeKm, s_cx, s_cy, R, s.rotationDeg);

        AcDraw d;
        lv_point_t target;
        target.x = (lv_coord_t)lroundf(p.x);
        target.y = (lv_coord_t)lroundf(p.y);
        d.to = target;
        {
            auto pit = prevPos.find(std::string(ac.hex.c_str()));
            if (pit != prevPos.end()) {
                const long dx = (long)target.x - pit->second.x;
                const long dy = (long)target.y - pit->second.y;
                d.from = (dx * dx + dy * dy > 120L * 120L) ? target : pit->second;  // snap if it jumped
            } else d.from = target;                                                  // new contact: appear in place
        }
#if MOTION_INTERP
        d.pos = d.from;                  // begin the glide at the previous position
#else
        d.pos = target;
        d.from = target;
#endif
        d.inRange = p.inRange;
        d.track = ac.track;
        d.color = alt_color(ac.altBaro, ac.onGround);
        d.emergency = acIsEmergency(ac.squawk);
        d.military  = ac.military;
        snprintf(d.hex,  sizeof(d.hex),  "%s", ac.hex.c_str());
        snprintf(d.call, sizeof(d.call), "%s", ac.flight.c_str());
        snprintf(d.type, sizeof(d.type), "%s", ac.type.c_str());
        d.altFt = ac.altBaro;
        d.onGround = ac.onGround;
        d.vsFpm = ac.baroRate;
        d.gsKt = ac.gs;
        d.distKm = (float)distKm;
        d.bearingDeg = (float)brg;
        d.lat = ac.lat;
        d.lon = ac.lon;
        d.cat = (uint8_t)aircraft_category(ac.type.c_str(), ac.altBaro, ac.gs);
        d.squawk = ac.squawk;
        if (ac.onGround) snprintf(d.altTxt, sizeof(d.altTxt), "GND");
        else             snprintf(d.altTxt, sizeof(d.altTxt), "%.0f ft", (double)ac.altBaro);

        // Measure the label once here rather than per frame in layout_labels(): the text
        // only changes when the feed does, while placement is redone on every motion step.
        {
            const bool bigPanel = (SCREEN_W >= 600);
            const lv_font_t *fc = (s_bigText || bigPanel) ? &lv_font_montserrat_18 : &lv_font_montserrat_14;
            const lv_font_t *fa = (s_bigText || bigPanel) ? &lv_font_montserrat_16 : &lv_font_montserrat_12;
            lv_point_t szc = { 0, 0 }, sza = { 0, 0 };
            if (d.call[0])   lv_txt_get_size(&szc, d.call,   fc, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (d.altTxt[0]) lv_txt_get_size(&sza, d.altTxt, fa, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            d.lblW = LV_MAX(szc.x, sza.x);
        }

        const std::string key = ac.hex.c_str();
        present.insert(key);
        if (d.inRange) {
            std::vector<lv_point_t> &hist = s_trails[key];
            const bool moved = hist.empty() ||
                               abs((int)hist.back().x - (int)target.x) > 0 ||
                               abs((int)hist.back().y - (int)target.y) > 0;
            if (moved) {
                if (s_flowMax > 0 && !hist.empty()) {
                    FlowSeg seg = { hist.back(), target, s_flowGen };
                    s_flow.push_back(seg);
                    while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
                    flow_draw_seg(seg);
                }
                if (s_trailMax > 0) {
                    hist.push_back(target);
                    while ((int)hist.size() > s_trailMax) hist.erase(hist.begin());
                } else {
                    hist.clear();
                }
            }
            d.trail = hist;
        }
        out.push_back(std::move(d));
    }

    for (auto it = s_trails.begin(); it != s_trails.end();) {
        if (present.find(it->first) == present.end()) it = s_trails.erase(it);
        else ++it;
    }
    if (!s_selHex.empty() && present.find(s_selHex) == present.end()) s_selHex.clear();

    // Fade the flow layer by AGE, not just count: drop segments older than s_flowGenMax
    // polls so old tracks self-clear even in busy airspace (a 5 nm view doesn't stay caked
    // in green). If any were dropped, repaint the flow canvas so they actually disappear.
    if (s_flowGenMax > 0 && !s_flow.empty()) {
        bool pruned = false;
        while (!s_flow.empty() && (uint16_t)(s_flowGen - s_flow.front().gen) > (uint16_t)s_flowGenMax) {
            s_flow.pop_front();
            pruned = true;
        }
        if (pruned) flow_redraw_all();
    }

    // nearest first (the blips + the list); cap to keep work bounded (web-configurable)
    std::sort(out.begin(), out.end(),
              [](const AcDraw &a, const AcDraw &b) { return a.distKm < b.distKm; });
    if ((int)out.size() > s_maxOnScreen) {
        // A tracked flight is usually the one heading away from us, so it would be the
        // first thing the distance cap drops. Pull it into the kept range instead.
        if (!s_trackHex.empty()) {
            for (size_t i = (size_t)s_maxOnScreen; i < out.size(); ++i) {
                if (s_trackHex == out[i].hex) { std::swap(out[(size_t)s_maxOnScreen - 1], out[i]); break; }
            }
        }
        out.resize(s_maxOnScreen);
    }

    if (++s_flowRedrawCtr >= FLOW_REDRAW_EVERY) {
        s_flowRedrawCtr = 0;
        flow_redraw_all();
    }

    if (s_rangeLbl) {                                 // keep the range label in sync with settings
        char r[16];
        // NOTE: hardcoded km. This label is hidden (setRangeLabelVisible(false) — the
        // zoom button shows the range instead), so it never reaches the screen. Anything
        // re-enabling it must convert to the user's unit first, as ui.cpp does.
        snprintf(r, sizeof(r), "%.0f km", (double)s.rangeKm);
        lv_label_set_text(s_rangeLbl, r);
    }

    const uint32_t now = lv_tick_get();              // measure actual cadence for the glide clock
    s_pollMs = (s_lastUpdateMs && now > s_lastUpdateMs) ? (now - s_lastUpdateMs) : (uint32_t)POLL_INTERVAL_MS;
    if (s_pollMs < 400)  s_pollMs = 400;
    if (s_pollMs > 8000) s_pollMs = 8000;
    s_lastUpdateMs = now;
    s_animStartMs  = now;

    // Carry each contact's label placement across the rebuild, keyed by ICAO hex.
    // AcDraw is constructed fresh every poll and the whole vector is replaced here, so
    // without this every label re-derived its position about once a second and visibly
    // jumped -- the hysteresis in layout_labels() only ever applied within a single poll
    // interval, which is to say it never applied at all.
    {
        std::map<std::string, lv_point_t> keepOfs;
        for (const AcDraw &prev : s_acs)
            if (prev.lblSet) keepOfs[std::string(prev.hex)] = lv_point_t{ prev.lblDx, prev.lblDy };
        for (AcDraw &nx : out) {
            const auto it = keepOfs.find(std::string(nx.hex));
            if (it == keepOfs.end()) continue;
            nx.lblDx = it->second.x;
            nx.lblDy = it->second.y;
            nx.lblSet = true;
        }
    }
    s_acs = std::move(out);
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

int hitTest(int x, int y) {
    if (s_trafficMode == TRAFFIC_MARINE) return -1;   // aircraft aren't on screen to hit
    int best = -1;
    long bestD = (long)TAP_RADIUS_PX * TAP_RADIUS_PX;
    const bool drg = orb();
    int balls = 0, arrows = 0;
    for (size_t i = 0; i < s_acs.size(); ++i) {
        if (drg) {
            if (s_acs[i].inRange) { if (balls >= ORB_BLIPS) continue; balls++; }
            else { if (arrows >= ORB_ARROWS) continue; arrows++; }
        } else if (!s_acs[i].inRange) continue;
        const long dx = (long)s_acs[i].pos.x - x;
        const long dy = (long)s_acs[i].pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; }
    }
    return best;
}

static void fill_info(const AcDraw &a, AcInfo &out) {
    snprintf(out.hex, sizeof(out.hex), "%s", a.hex);
    snprintf(out.call, sizeof(out.call), "%s", a.call);
    snprintf(out.type, sizeof(out.type), "%s", a.type);
    out.altFt = a.altFt; out.onGround = a.onGround;
    out.vsFpm = a.vsFpm; out.gsKt = a.gsKt;
    out.distKm = a.distKm; out.bearingDeg = a.bearingDeg;
    out.squawk = a.squawk; out.emergency = a.emergency; out.military = a.military;
}

// Select by ICAO hex. The index-based form is unstable for anything scripted: the list
// re-sorts by distance every poll, so index N is a different aircraft seconds later.
bool selectByHex(const char *hex) {
    if (!hex || !hex[0]) return false;
    for (const AcDraw &a : s_acs) {
        if (strcasecmp(a.hex, hex) == 0) {
            s_selHex = a.hex;
            if (s_acLayer) lv_obj_invalidate(s_acLayer);
            return true;
        }
    }
    return false;
}

void select(int idx) {
    if (idx < 0 || idx >= (int)s_acs.size()) s_selHex.clear();
    else s_selHex = s_acs[idx].hex;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

bool selected(AcInfo &out) {
    if (s_selHex.empty()) return false;
    for (const AcDraw &a : s_acs)
        if (s_selHex == a.hex) { fill_info(a, out); return true; }
    return false;
}

int count() { return (int)s_acs.size(); }

int countInRange() {
    int n = 0;
    for (const AcDraw &a : s_acs) if (a.inRange) ++n;
    return n;
}

bool info(int idx, AcInfo &out) {
    if (idx < 0 || idx >= (int)s_acs.size()) return false;
    fill_info(s_acs[idx], out);
    return true;
}

bool infoByHex(const char *hex, AcInfo &out) {
    if (!hex || !hex[0]) return false;
    for (const AcDraw &a : s_acs)
        if (strcmp(a.hex, hex) == 0) { fill_info(a, out); return true; }
    return false;
}

bool positionByHex(const char *hex, double *lat, double *lon) {
    if (!hex || !hex[0]) return false;
    for (const AcDraw &a : s_acs)
        if (strcmp(a.hex, hex) == 0) {
            if (lat) *lat = a.lat;
            if (lon) *lon = a.lon;
            return true;
        }
    return false;
}

void setTracked(const char *hex) { s_trackHex = (hex && hex[0]) ? hex : ""; }
const char *tracked() { return s_trackHex.c_str(); }

void sweepPerf(float *fps, uint32_t *drawUs, float *stepAvg, float *stepMax) {
    if (fps)     *fps = s_fps;
    if (drawUs)  *drawUs = s_drawUsAvg;
    if (stepAvg) *stepAvg = s_stepAvg;
    if (stepMax) *stepMax = s_stepMax;
}

uint32_t sweepFrameMs() { return s_dtAvg; }

void setSweepTuning(float trailDeg, int trailSteps, float maxStepPx) {
    if (trailDeg   > 5.0f && trailDeg   <= 180.0f) s_trailDeg   = trailDeg;
    if (trailSteps >= 4   && trailSteps <= 48)     s_trailSteps = trailSteps;
    if (maxStepPx  > 1.0f && maxStepPx  <= 60.0f)  s_maxStepPx  = maxStepPx;
}

void sweepTuning(float *trailDeg, int *trailSteps, float *maxStepPx) {
    if (trailDeg)   *trailDeg   = s_trailDeg;
    if (trailSteps) *trailSteps = s_trailSteps;
    if (maxStepPx)  *maxStepPx  = s_maxStepPx;
}

void setMapOpacity(int percent) {
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_mapOpacity = percent;
    if (s_mapCanvas) {
        lv_opa_t lvOpa = (lv_opa_t)((percent * 255) / 100);
        lv_obj_set_style_img_opa(s_mapCanvas, lvOpa, LV_PART_MAIN);
        lv_obj_invalidate(s_mapCanvas);
    }
}

void tickSweep() { /* sweep self-animates via lv_timer */ }

} // namespace radar
