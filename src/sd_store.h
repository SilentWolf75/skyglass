#pragma once
// microSD: card bring-up plus the flight-log store that lives on it.
//
// Pin assignment is not a board choice: the ESP32-P4 multiplexes SDMMC_HOST_SLOT_0 onto
// GPIO39-48 through the IO MUX, so those signals cannot be routed anywhere else (ESP32-P4
// datasheet, "SD/MMC Host Controller > Pin Assignment"), and Waveshare's own BSP for this
// board family agrees: D0-D3 on 39/40/41/42, CMD 44, CLK 43, slot 0, width 4.
//
// Mounted through esp_vfs_fat_sdmmc_mount rather than Arduino's SD_MMC because that keeps
// the sdmmc_card_t handle, which is what makes an on-device format possible at all.
#include <stdint.h>
#include <stdbool.h>

// ---- card ------------------------------------------------------------------------
bool        sd_begin(void);       // safe with no card and on boards with no slot
bool        sd_mounted(void);
uint8_t     sd_bus_width(void);   // 4 or 1 when mounted, else 0
uint64_t    sd_size_bytes(void);
const char *sd_status(void);      // short human string for /diag

// Reformat the whole card as FAT and remount. Destroys everything on it, which is why
// nothing calls this without an explicit confirmed action from the user.
bool sd_format(void);

// ---- flight log --------------------------------------------------------------------
// One fixed-size record per airframe, keyed by ICAO hex. Aggregates rather than a raw
// sighting list: the question the detail card asks is "how often has this one been over
// before", and answering that from an append-only log would mean scanning it every time.
struct SdSeen {
    uint16_t count;        // distinct visits (see SD_VISIT_GAP_S)
    uint32_t firstSeen;    // epoch seconds, 0 if the clock was unset when first logged
    uint32_t lastSeen;
    uint16_t closestDam;   // closest approach ever, in units of 10 m; 0 = unknown
};

// A contact seen again within this window is the same visit, not a new one. Aircraft
// drop off the feed briefly all the time; without it a single overflight counts as five.
#define SD_VISIT_GAP_S 1800

// Called from the feed task, never the render loop: this writes to the card.
void sd_log_seen(const char *hex, const char *callsign, float distKm, uint32_t nowEpoch);

// Read-only lookup for the detail card. False if there is no card or no record.
bool sd_seen_lookup(const char *hex, SdSeen *out);

uint32_t sd_seen_records(void);   // how many airframes are on file
bool     sd_seen_erase(void);     // wipe the log, keep everything else on the card
