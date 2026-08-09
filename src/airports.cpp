#include "airports.h"
#include "airports_data.h"
#include "geo.h"
#include <vector>
#include <math.h>
#include <string.h>

struct Apt { lv_point_t pos; char iata[6]; uint8_t large; };
static std::vector<Apt> s_apts;

// Label everything when the scope isn't crowded. Zoomed in, the nearby GA field is
// exactly what you want named; zoomed out, labelling 40 airports is unreadable, so
// only the large ones keep their tag.
#define APT_LABEL_ALL_MAX 14

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx) {
    s_apts.clear();
    if (rangeKm <= 0) return;

    const float rangeDeg  = (float)rangeKm / 111.0f;
    const float latMargin = rangeDeg * 1.05f;
    const float cosLat    = cosf((float)(homeLat * M_PI / 180.0));
    const float lonMargin = latMargin / (cosLat < 0.15f ? 0.15f : cosLat);

    for (int i = 0; i < AIRPORT_NUM; ++i) {
        const float lat = (float)AIRPORT_LAT[i] / (float)AIRPORT_SCALE;
        const float lon = (float)AIRPORT_LON[i] / (float)AIRPORT_SCALE;
        const float dlon = lon - (float)homeLon;
        if (fabsf(lat - (float)homeLat) > latMargin) continue;                      // cheap bbox reject
        if (fabsf(dlon) > lonMargin && fabsf(fabsf(dlon) - 360.0f) > lonMargin) continue;
        const float dist = geo::haversineKmf((float)homeLat, (float)homeLon, lat, lon);
        if (dist > (float)rangeKm) continue;                                       // only inside the scope
        const float brg = geo::bearingDegf((float)homeLat, (float)homeLon, lat, lon);
        const float rPx = (dist / (float)rangeKm) * rOuterPx;
        const float a   = brg * (float)M_PI / 180.0f;
        Apt ap;
        ap.pos.x = (lv_coord_t)lroundf(cx + rPx * sinf(a));
        ap.pos.y = (lv_coord_t)lroundf(cy - rPx * cosf(a));
        memcpy(ap.iata, AIRPORT_IATA[i], sizeof(ap.iata));
        ap.iata[sizeof(ap.iata) - 1] = 0;
        ap.large = AIRPORT_LARGE[i];
        s_apts.push_back(ap);
    }
}

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa,
                   lv_color_t labelColor, lv_opa_t labelOpa) {
    if (s_apts.empty()) return;

    lv_draw_arc_dsc_t ring;
    lv_draw_arc_dsc_init(&ring);
    ring.color = color; ring.width = 2; ring.opa = opa;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = color; dot.bg_opa = opa; dot.radius = LV_RADIUS_CIRCLE;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = labelColor; lbl.opa = labelOpa; lbl.font = &lv_font_montserrat_14;

    const bool labelAll = (s_apts.size() <= APT_LABEL_ALL_MAX);
    for (const Apt &ap : s_apts) {
        if (ap.large) {
            lv_draw_arc(ctx, &ring, &ap.pos, 3, 0, 360);                    // small hollow ring
        } else {
            lv_area_t d = { (lv_coord_t)(ap.pos.x - 2), (lv_coord_t)(ap.pos.y - 2),
                            (lv_coord_t)(ap.pos.x + 2), (lv_coord_t)(ap.pos.y + 2) };
            lv_draw_rect(ctx, &dot, &d);                                    // small marker
        }
        if (ap.iata[0] && (ap.large || labelAll)) {
            lv_area_t la = { (lv_coord_t)(ap.pos.x + 5), (lv_coord_t)(ap.pos.y - 7),
                             (lv_coord_t)(ap.pos.x + 52), (lv_coord_t)(ap.pos.y + 7) };
            lv_draw_label(ctx, &lbl, &la, ap.iata, NULL);
        }
    }
}

int airports_label_boxes(lv_area_t *out, int max) {
    if (!out || max <= 0) return 0;
    // Same condition as the draw, or the boxes would describe labels that are not there.
    const bool labelAll = (s_apts.size() <= APT_LABEL_ALL_MAX);
    int n = 0;
    for (const Apt &ap : s_apts) {
        if (n >= max) break;
        if (!ap.iata[0] || !(ap.large || labelAll)) continue;
        out[n].x1 = (lv_coord_t)(ap.pos.x + 5);
        out[n].y1 = (lv_coord_t)(ap.pos.y - 7);
        out[n].x2 = (lv_coord_t)(ap.pos.x + 52);
        out[n].y2 = (lv_coord_t)(ap.pos.y + 7);
        ++n;
    }
    return n;
}

bool airports_nearest_iata(double lat, double lon, float maxKm,
                           char iata[6], float *distKm, float *bearingDeg) {
    if (iata) iata[0] = 0;
    // Two passes: a large airport is the useful landmark for the weather view's
    // "nearest field" line. Now that small GA strips are in the dataset, a single
    // nearest-any search would answer with whatever grass runway is down the road.
    double best = maxKm;
    int bestIdx = -1;
    for (int pass = 0; pass < 2 && bestIdx < 0; ++pass) {
        best = maxKm;
        for (int i = 0; i < AIRPORT_NUM; ++i) {
            if (pass == 0 && !AIRPORT_LARGE[i]) continue;
            if (!AIRPORT_IATA[i][0]) continue;
            const float alat = (float)AIRPORT_LAT[i] / (float)AIRPORT_SCALE;
            const float alon = (float)AIRPORT_LON[i] / (float)AIRPORT_SCALE;
            const float d = geo::haversineKmf((float)lat, (float)lon, alat, alon);
            if (d < best) { best = d; bestIdx = i; }
        }
    }
    if (bestIdx < 0) return false;
    if (iata) { memcpy(iata, AIRPORT_IATA[bestIdx], 6); iata[5] = 0; }
    if (distKm) *distKm = (float)best;
    if (bearingDeg) {
        const float alat = (float)AIRPORT_LAT[bestIdx] / (float)AIRPORT_SCALE;
        const float alon = (float)AIRPORT_LON[bestIdx] / (float)AIRPORT_SCALE;
        *bearingDeg = geo::bearingDegf((float)lat, (float)lon, alat, alon);
    }
    return true;
}
