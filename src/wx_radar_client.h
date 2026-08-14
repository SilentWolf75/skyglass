#pragma once

#include <stdint.h>

bool wx_radar_fetch(double lat, double lon);

// Frames still to download before the loop is complete (0 = done).
int wx_radar_backlog(void);
// Last decode: painted pixels seen in the source tile, and painted pixels that survived
// the centre crop and the circular mask. On the P4 the Serial log never reaches the USB
// port, so without these there is no way to tell "no weather" from "decode produced
// nothing" -- which look identical on the glass.
void wx_radar_last_decode(uint32_t *sourcePx, uint32_t *drawnPx, int *w, int *h);
