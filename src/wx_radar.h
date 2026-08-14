#pragma once

#include <stdint.h>

#include "config.h"

// Displayed size of the radar image: a centre crop of the WX_RADAR_SOURCE_SIZE tile we
// fetch. Larger shows more of the same imagery at the same scale -- it is not a zoom.
// Boards set it to their panel width so the imagery runs to the bezel like the scope
// does; a smaller value simply insets it.
#ifndef WX_RADAR_SIZE
#  define WX_RADAR_SIZE 360
#endif
// RainViewer renders whatever square you ask for, so the tile is fetched slightly larger
// than the panel and centre-cropped -- the crop is what keeps the circle clean at the
// edges. It has to be >= WX_RADAR_SIZE or there is nothing to crop from.
#ifndef WX_RADAR_SOURCE_SIZE
#  define WX_RADAR_SOURCE_SIZE 512
#endif
#if WX_RADAR_SOURCE_SIZE < WX_RADAR_SIZE
#  error "WX_RADAR_SOURCE_SIZE must be at least WX_RADAR_SIZE"
#endif

// How many past frames to keep for the loop. RainViewer publishes 13 at ten-minute steps
// (two hours); a board keeps as many as its PSRAM can spare. Each frame costs
// WX_RADAR_SIZE^2 * 2 bytes -- 450 KB on a 480 px panel, 200 KB on a 320 px one.
// Boards override this; the default suits a small panel.
#ifndef WX_RADAR_FRAMES
#  define WX_RADAR_FRAMES 6
#endif

// Slippy-map zoom of the fetched tile. Higher is closer: each step halves the ground
// covered. The frames already held were rendered at the old scale and cannot be mixed
// with the new ones, so setting a different zoom drops them.
#define WX_ZOOM_MIN 5
#define WX_ZOOM_MAX 9
#define WX_ZOOM_DEF 7
int  wx_radar_zoom(void);
void wx_radar_set_zoom(int z);        // clears the loop if the zoom actually changes
// Ground distance across the *displayed* image, in km, at the current zoom. A zoom-7
// tile spans about 107 km per 512 px, and the scale halves with every zoom step.
float wx_radar_range_km(void);

void wx_radar_begin(void);
uint16_t *wx_radar_back_buffer(void);                    // network/sim: decode here

// Publish the back buffer as the frame for `frameTime`. Frames are kept in time order and
// the oldest is evicted once the ring is full, so the loop always spans the most recent
// window regardless of the order they arrive in.
void wx_radar_commit(uint32_t frameTime, double lat, double lon);

// True if this frame time is already held -- the fetcher uses it to work out which of
// RainViewer's frames it still needs, so it never downloads the same one twice.
bool wx_radar_has_frame(uint32_t frameTime);
int  wx_radar_frame_count(void);
int  wx_radar_capacity(void);                            // frames actually allocated

// Frame `idx` in time order, 0 = oldest. Playback walks this.
bool wx_radar_frame(int idx, const uint16_t **pixels, uint32_t *frameTime);

// The newest frame, for callers that just want "now".
bool wx_radar_front(const uint16_t **pixels, uint32_t *frameTime,
                    double *lat, double *lon, uint32_t *version);
