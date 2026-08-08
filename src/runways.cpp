#include "runways.h"
#include "runways_data.h"
#include "geo.h"
#include <vector>
#include <math.h>

struct Rwy { lv_point_t a, b; };
static std::vector<Rwy> s_rwys;

// Below this a runway is a smudge rather than a shape, and drawing it just adds noise
// around the airport dot that is already there. At a 6 nm range a 5,000 ft strip is
// comfortably past it; at 100 nm almost nothing survives, which is correct.
#define RWY_MIN_PX 5

// A busy metro area can hold a lot of strips. The cap is a memory bound, not a display
// choice: the list is walked in file order, so this only bites somewhere implausibly
// dense, and 96 segments is already far more than reads on a 466 px scope.
#define RWY_MAX 96

void runways_project(double homeLat, double homeLon, double rangeKm,
                     float cx, float cy, float rOuterPx) {
    s_rwys.clear();
    if (rangeKm <= 0) return;

    const float rangeDeg  = (float)rangeKm / 111.0f;
    const float latMargin = rangeDeg * 1.05f;
    const float cosLat    = cosf((float)(homeLat * M_PI / 180.0));
    const float lonMargin = latMargin / (cosLat < 0.15f ? 0.15f : cosLat);

    for (int i = 0; i < RUNWAY_NUM && (int)s_rwys.size() < RWY_MAX; ++i) {
        const float aLat = (float)RUNWAY_LE_LAT[i] / (float)RUNWAY_SCALE;
        const float aLon = (float)RUNWAY_LE_LON[i] / (float)RUNWAY_SCALE;
        // Cheap bbox reject on one threshold. A runway is at most a few km long, so if
        // one end is far outside the scope the other cannot be inside it.
        if (fabsf(aLat - (float)homeLat) > latMargin) continue;
        const float dlon = aLon - (float)homeLon;
        if (fabsf(dlon) > lonMargin && fabsf(fabsf(dlon) - 360.0f) > lonMargin) continue;

        const float bLat = (float)RUNWAY_HE_LAT[i] / (float)RUNWAY_SCALE;
        const float bLon = (float)RUNWAY_HE_LON[i] / (float)RUNWAY_SCALE;

        const float dA = geo::haversineKmf((float)homeLat, (float)homeLon, aLat, aLon);
        const float dB = geo::haversineKmf((float)homeLat, (float)homeLon, bLat, bLon);
        // Keep it if either end is on the scope, so a runway straddling the outer ring
        // is still drawn rather than vanishing whole.
        if (dA > (float)rangeKm && dB > (float)rangeKm) continue;

        const float brgA = geo::bearingDegf((float)homeLat, (float)homeLon, aLat, aLon);
        const float brgB = geo::bearingDegf((float)homeLat, (float)homeLon, bLat, bLon);
        const float rA = (dA / (float)rangeKm) * rOuterPx;
        const float rB = (dB / (float)rangeKm) * rOuterPx;
        const float radA = brgA * (float)M_PI / 180.0f;
        const float radB = brgB * (float)M_PI / 180.0f;

        Rwy r;
        r.a.x = (lv_coord_t)lroundf(cx + rA * sinf(radA));
        r.a.y = (lv_coord_t)lroundf(cy - rA * cosf(radA));
        r.b.x = (lv_coord_t)lroundf(cx + rB * sinf(radB));
        r.b.y = (lv_coord_t)lroundf(cy - rB * cosf(radB));

        const int dx = r.b.x - r.a.x, dy = r.b.y - r.a.y;
        if (dx * dx + dy * dy < RWY_MIN_PX * RWY_MIN_PX) continue;   // too short to read
        s_rwys.push_back(r);
    }
}

void runways_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa) {
    if (s_rwys.empty()) return;

    lv_draw_line_dsc_t ln;
    lv_draw_line_dsc_init(&ln);
    ln.color = color;
    ln.opa = opa;
    // Real runways are 20-60 m wide, which is well under a pixel at any range this scope
    // uses. A fixed 3 px reads as a strip without pretending to be to scale.
    ln.width = 3;
    ln.round_start = 1;
    ln.round_end = 1;

    for (const Rwy &r : s_rwys) lv_draw_line(ctx, &ln, &r.a, &r.b);
}
