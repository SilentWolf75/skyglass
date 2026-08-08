#include "sd_store.h"
#include "config.h"
#include <Arduino.h>

#if BOARD_HAS_SD
#include <SD_MMC.h>

static bool     s_mounted = false;
static uint8_t  s_width   = 0;
static uint64_t s_size    = 0;
static char     s_status[48] = "not probed";

// The card rail is switched by a P-channel FET (AO3401) whose gate is pulled up to 3V3,
// so the card is OFF at reset and pulling the gate LOW turns it ON. That is the standard
// arrangement and what the XC schematic shows, but the FET is a board choice rather than
// a chip constraint -- so if the LOW-side attempt finds nothing, try the other polarity
// before concluding there is no card. A wrong guess costs a failed mount, nothing more.
static bool try_mount(bool oneBit) {
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
    // mountpoint, mode1bit, format_if_mount_failed -- never format: an unmountable card
    // is a diagnosis, not an invitation to erase whatever the user had on it.
    return SD_MMC.begin("/sdcard", oneBit, false);
}

bool sd_begin(void) {
    if (s_mounted) return true;

#if PIN_SD_PWR >= 0
    pinMode(PIN_SD_PWR, OUTPUT);
#endif

    for (int polarity = 0; polarity < 2 && !s_mounted; ++polarity) {
#if PIN_SD_PWR >= 0
        digitalWrite(PIN_SD_PWR, polarity == 0 ? LOW : HIGH);
        delay(60);                       // let the rail settle before clocking the card
#else
        if (polarity) break;             // no power control: one attempt is all there is
#endif
        // 4-bit first; a slot wired 1-bit still enumerates in 1-bit mode.
        for (int oneBit = 0; oneBit < 2 && !s_mounted; ++oneBit) {
            if (try_mount(oneBit != 0)) {
                s_mounted = true;
                s_width = oneBit ? 1 : 4;
                s_size = SD_MMC.cardSize();
            } else {
                SD_MMC.end();
            }
        }
    }

    if (s_mounted) {
        snprintf(s_status, sizeof(s_status), "%u-bit %llu MB",
                 (unsigned)s_width, (unsigned long long)(s_size / (1024ULL * 1024ULL)));
        Serial.printf("[sd] mounted, %s\n", s_status);
    } else {
        snprintf(s_status, sizeof(s_status), "no card");
        Serial.println("[sd] no card mounted (empty slot, or the rail/bus is not as expected)");
    }
    return s_mounted;
}

bool        sd_mounted(void)    { return s_mounted; }
uint8_t     sd_bus_width(void)  { return s_width; }
uint64_t    sd_size_bytes(void) { return s_size; }
const char *sd_status(void)     { return s_status; }

#else   // no slot on this board

bool        sd_begin(void)      { return false; }
bool        sd_mounted(void)    { return false; }
uint8_t     sd_bus_width(void)  { return 0; }
uint64_t    sd_size_bytes(void) { return 0; }
const char *sd_status(void)     { return "no slot"; }

#endif
