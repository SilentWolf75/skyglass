#pragma once
// Airport markers for the radar scope. Projects the embedded OurAirports list
// (airports_data.h) like the coastline: cull to the scope, great-circle project,
// cache screen markers, draw in the static chrome layer. Large airports get a small
// ring + IATA label; medium airports are a faint dot. Projection is done only on a
// home/range change, never per frame.
#include <lvgl.h>

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx);

// Markers and labels are styled separately on purpose: the ring/dot should sit quietly
// under the traffic, but the ident has to be readable or there is no point drawing it.
void airports_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa,
                   lv_color_t labelColor, lv_opa_t labelOpa);

// The rectangles airports_draw() will put idents in, so other layers can route around
// them. Aircraft labels and airport idents were overprinting each other once the marker
// list grew to include every field with a runway -- there are simply far more idents on
// screen now. Mirrors the draw logic exactly, including which airports get labelled.
int airports_label_boxes(lv_area_t *out, int max);

// Find the nearest recognizable airport (one with an IATA code). Used by the
// weather view for aviation context; entirely offline from the embedded data.
bool airports_nearest_iata(double lat, double lon, float maxKm,
                           char iata[6], float *distKm, float *bearingDeg);
