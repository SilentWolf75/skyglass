#pragma once
// Runway outlines for the radar scope. Same shape as airports.*: cull the embedded
// OurAirports list (runways_data.h) to the scope, great-circle project both thresholds,
// cache the screen segments, and draw them in the static chrome layer. Projection runs
// only on a home/range change, never per frame.
//
// Both ends are projected independently rather than deriving one from a heading and a
// length, so the drawn strip lies along the real runway on the real bearing.
#include <lvgl.h>

void runways_project(double homeLat, double homeLon, double rangeKm,
                     float cx, float cy, float rOuterPx);

// Drawn under the airport markers and well under the traffic: this is terrain, not
// something to read. A runway shorter than a few pixels is dropped during projection.
void runways_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa);
