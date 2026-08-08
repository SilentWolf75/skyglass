#pragma once
// microSD bring-up. Nothing is built on this yet -- the point of the first pass is to
// establish, on hardware, that the card mounts and at what bus width.
//
// Pin assignment is not a board choice: the ESP32-P4 multiplexes SDMMC_HOST_SLOT_0 onto
// GPIO39-48 through the IO MUX, so those signals cannot be routed anywhere else. Any P4
// board with a card on slot 0 uses exactly these pins (ESP32-P4 datasheet, "SD/MMC Host
// Controller > Pin Assignment"). What IS a board choice is the card-power FET and
// whether D1-D3 are populated, which is why the mount below probes rather than assumes.
#include <stdint.h>

// Try to bring the card up. Safe to call when no card is present, and safe to call on a
// board with no slot: it fails and reports so. Returns true if a card mounted.
bool sd_begin(void);

bool     sd_mounted(void);
uint8_t  sd_bus_width(void);      // 4 or 1 when mounted, 0 otherwise
uint64_t sd_size_bytes(void);
const char *sd_status(void);      // short human string for /diag
