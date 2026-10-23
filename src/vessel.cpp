#include "vessel.h"
#include "geo.h"
#include <mutex>
#include <vector>
#include <algorithm>
#include <math.h>
#include <string.h>
#include <stdio.h>

static std::mutex s_m;
static Vessel s_v[VESSEL_MAX];
static int    s_n = 0;
static bool   s_dirty = false;

struct VesselMark {
    lv_point_t pos;
    float cogDeg, sogKt;
    float distKm, bearingDeg;
    uint32_t mmsi;
    char name[21];
};
static std::vector<VesselMark> s_marks;
static double s_pLat = 1e9, s_pLon = 1e9;
static float  s_pRange = -1.0f;

void vessel_upsert(const Vessel &v) {
    std::lock_guard<std::mutex> g(s_m);
    for (int i = 0; i < s_n; ++i) {
        if (s_v[i].mmsi == v.mmsi) {
            // A position report carries no ship name, so preserve the one an earlier
            // static/voyage report gave us instead of blanking the label.
            char keep[sizeof(s_v[i].name)];
            memcpy(keep, s_v[i].name, sizeof(keep));
            s_v[i] = v;
            if (!v.name[0] && keep[0]) memcpy(s_v[i].name, keep, sizeof(keep));
            s_dirty = true;
            return;
        }
    }
    if (s_n < VESSEL_MAX) {
        s_v[s_n++] = v;
        s_dirty = true;
        return;
    }
    // Full: replace the least recently updated contact.
    int oldest = 0;
    for (int i = 1; i < s_n; ++i)
        if ((int32_t)(s_v[i].updatedMs - s_v[oldest].updatedMs) < 0) oldest = i;
    s_v[oldest] = v;
    s_dirty = true;
}

void vessel_expire(uint32_t nowMs) {
    std::lock_guard<std::mutex> g(s_m);
    for (int i = 0; i < s_n;) {
        if (nowMs - s_v[i].updatedMs > VESSEL_STALE_MS) {
            s_v[i] = s_v[--s_n];
            s_dirty = true;
        } else ++i;
    }
}

void vessel_clear(void) {
    std::lock_guard<std::mutex> g(s_m);
    s_n = 0;
    s_dirty = true;
}

int vessel_count(void) {
    std::lock_guard<std::mutex> g(s_m);
    return s_n;
}

bool vessel_project(double homeLat, double homeLon, double rangeKm,
                    float cx, float cy, float rOuterPx) {
    Vessel local[VESSEL_MAX];
    int n;
    {
        std::lock_guard<std::mutex> g(s_m);
        if (!s_dirty && homeLat == s_pLat && homeLon == s_pLon && (float)rangeKm == s_pRange)
            return false;
        n = s_n;
        memcpy(local, s_v, (size_t)n * sizeof(Vessel));
        s_dirty = false;
    }
    s_pLat = homeLat; s_pLon = homeLon; s_pRange = (float)rangeKm;

    s_marks.clear();
    if (rangeKm <= 0) return true;
        for (int i = 0; i < n; ++i) {
        const float d = geo::haversineKmf((float)homeLat, (float)homeLon, (float)local[i].lat, (float)local[i].lon);
        if (d > (float)rangeKm) continue;
        const float brg = geo::bearingDegf((float)homeLat, (float)homeLon, (float)local[i].lat, (float)local[i].lon);
        const float rPx = (d / (float)rangeKm) * rOuterPx;
        const float a = brg * (float)M_PI / 180.0f;
        VesselMark m;
        m.pos.x = (lv_coord_t)lroundf(cx + rPx * sinf(a));
        m.pos.y = (lv_coord_t)lroundf(cy - rPx * cosf(a));
        m.cogDeg = local[i].cogDeg;
        m.sogKt = local[i].sogKt;
        m.distKm = d;
        m.bearingDeg = brg;
        m.mmsi = local[i].mmsi;
        memcpy(m.name, local[i].name, sizeof(m.name));
        s_marks.push_back(m);
    }
    // Nearest first, so the list view reads like the aircraft list.
    std::sort(s_marks.begin(), s_marks.end(),
              [](const VesselMark &a, const VesselMark &b) { return a.distKm < b.distKm; });
    return true;
}

static void fill_vessel_info(const VesselMark &m, VesselInfo *out) {
    memcpy(out->name, m.name, sizeof(out->name));
    out->mmsi = m.mmsi;
    out->sogKt = m.sogKt;
    out->cogDeg = m.cogDeg;
    out->distKm = m.distKm;
    out->bearingDeg = m.bearingDeg;
}

int vessel_visible_count(void) { return (int)s_marks.size(); }

bool vessel_visible_info(int idx, VesselInfo *out) {
    if (idx < 0 || idx >= (int)s_marks.size()) return false;
    if (out) fill_vessel_info(s_marks[idx], out);
    return true;
}

void vessel_draw(lv_draw_ctx_t *ctx, lv_opa_t opa) {
    if (s_marks.empty()) return;

    // Steel-cyan hull shape, deliberately unlike the aircraft glyph and the altitude
    // palette: a vessel should never be mistaken for low-altitude traffic.
    const lv_color_t col = lv_color_hex(0x35D6FF);
    lv_draw_rect_dsc_t hull;
    lv_draw_rect_dsc_init(&hull);
    hull.bg_color = col;
    hull.bg_opa = opa;

    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.color = col;
    lbl.opa = (lv_opa_t)(opa * 3 / 4);
    lbl.font = &lv_font_montserrat_12;

    for (const VesselMark &m : s_marks) {
        // Bow points along the course when known, otherwise north.
        const float hdg = (m.cogDeg == m.cogDeg) ? m.cogDeg : 0.0f;
        const float a = hdg * (float)M_PI / 180.0f;
        const float c = cosf(a), s = sinf(a);
        static const float VX[4] = { 0.0f,  5.0f,  0.0f, -5.0f };
        static const float VY[4] = { -8.0f, 5.0f,  2.0f,  5.0f };
        lv_point_t pts[4];
        for (int i = 0; i < 4; ++i) {
            pts[i].x = (lv_coord_t)(m.pos.x + (lv_coord_t)lroundf(VX[i] * c - VY[i] * s));
            pts[i].y = (lv_coord_t)(m.pos.y + (lv_coord_t)lroundf(VX[i] * s + VY[i] * c));
        }
        // The hull is a concave chevron (the notch at pts[2] sits above both rear corners),
        // and LVGL 8's lv_draw_polygon only supports CONVEX polygons -- on a concave one its
        // edge walk never finishes, which froze the UI task until the watchdog rebooted the
        // board as soon as the first ships arrived. Draw it as two triangles instead (always
        // convex): tip-right-notch and tip-notch-left. Same outline, no hang.
        const lv_point_t right[3] = { pts[0], pts[1], pts[2] };
        const lv_point_t left[3]  = { pts[0], pts[2], pts[3] };
        lv_draw_polygon(ctx, &hull, right, 3);
        lv_draw_polygon(ctx, &hull, left, 3);

        if (m.name[0]) {
            lv_area_t la = { (lv_coord_t)(m.pos.x + 9), (lv_coord_t)(m.pos.y - 6),
                             (lv_coord_t)(m.pos.x + 120), (lv_coord_t)(m.pos.y + 10) };
            lv_draw_label(ctx, &lbl, &la, m.name, NULL);
        }
    }
}

bool vessel_hit_test(int x, int y, int maxPx, VesselInfo *out) {
    int best = -1;
    long bestD = (long)maxPx * maxPx;
    for (size_t i = 0; i < s_marks.size(); ++i) {
        const long dx = (long)s_marks[i].pos.x - x;
        const long dy = (long)s_marks[i].pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; }
    }
    if (best < 0) return false;
    if (out) fill_vessel_info(s_marks[best], out);
    return true;
}
