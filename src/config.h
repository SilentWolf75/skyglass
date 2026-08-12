#pragma once
// SkyGlass — build & user configuration.

#define FW_VERSION "1.18.3"   // shown on the web config page + Stats screen; bump on release
// ---------- Board selection ----------
// Everything hardware-specific (screen geometry, pin map, which peripherals exist)
// lives in src/boards/<board>.h. Pick one with a -D flag in platformio.ini; the
// 1.75" AMOLED board is the default so existing builds are unaffected.
#if defined(BOARD_WAVESHARE_P4_LCD_4C)
#  include "boards/waveshare_p4_lcd_4c.h"
#elif defined(BOARD_WAVESHARE_S3_AMOLED_175) || 1
#  include "boards/waveshare_s3_amoled_175.h"
#endif

// ---------- UI scaling ----------
// The UI was laid out by hand against the 466x466 panel, so absolute pixel offsets in
// ui.cpp are in that design space. UI_S() maps them onto whatever panel is built for.
// Both operands are compile-time constants, so on the 466 board this folds to the
// identity at compile time and the original build is bit-for-bit unaffected.
#define UI_DESIGN_W 466
#define UI_S(v)     (((int)(v) * SCREEN_W) / UI_DESIGN_W)

// ---------- Home location (default: Dénia, Spain) ----------
// Overridable at runtime via the captive portal (stored in NVS).
#define HOME_LAT_DEFAULT   38.8409
#define HOME_LON_DEFAULT    0.1059

// ---------- Radar ----------
#define RANGE_KM_DEFAULT    30.0f          // display range (outer ring). Query is wider, see below.
// Feed query radius = display range × MULT, clamped to [MIN, MAX]. Querying a bit wider than
// the display shows off-range traffic as edge arrows.
#define ADSB_QUERY_MULT     1.6f
#define ADSB_QUERY_MIN_KM   20.0f
#define ADSB_QUERY_MAX_KM   150.0f
static const float RANGE_STEPS_KM[] = {1.60934f, 4.82803f, 10.0f, 20.0f, 30.0f, 50.0f, 100.0f};
#define POLL_INTERVAL_MS    2000           // be gentle with the free API (>=1000)
#define POLL_INTERVAL_BATTERY_MS 5000      // slower polling when running on battery
#define MOTION_INTERP       1              // 1 = glyphs glide between polls; 0 = snap to new pos
#define AC_STALE_MS         15000          // keep the last contacts through brief empty feed responses

// ---------- Weather forecast (Open-Meteo, no API key) ----------
#define WEATHER_REFRESH_MS  1800000UL      // 30 minutes; forecast data changes slowly
#define WX_RADAR_REFRESH_MS 300000UL       // RainViewer frames update about every 5 minutes
#define CLOUD_IMAGE_REFRESH_MS 600000UL    // EUMETSAT MTG cloud imagery; cache for 10 minutes

#define LV_COLOR_DEPTH_BITS 16
#define BRIGHTNESS_DEFAULT  200            // 0..255, panel brightness via cmd 0x51
#define TZ_STR              "CET-1CEST,M3.5.0,M10.5.0/3"  // POSIX TZ (Spain) for local time/date
#define BRIGHTNESS_IDLE     25             // dimmed after no touch for IDLE_DIM_MS
#define IDLE_DIM_MS         20000          // dim the screen after this long without a touch

// ---------- ADS-B API (free, non-commercial) ----------
#define ADSB_PRIMARY_HOST   "api.airplanes.live"   // GET /v2/point/{lat}/{lon}/{radius_nm}
#define ADSB_FALLBACK_HOST  "api.adsb.lol"          // same readsb format
// A dump1090 / readsb / tar1090 receiver on your own network, set from the config page
// (blank = use the public feeds). This is the path every one of those serves the decoded
// picture on; the ADSB Exchange and PiAware images both include tar1090.
#define ADSB_LOCAL_PATH     "/tar1090/data/aircraft.json"
#define ADSB_USER_AGENT     "SkyGlass/1.0 (ESP32-S3 hobby; +https://github.com/SilentWolf75/skyglass)"
#define ADSB_HTTPS_INSECURE 1               // 1 = setInsecure() (hobby). 0 = use pinned root CA.
// Rows the contact list will build. Bounded because each row is LVGL pool memory and
// the pool is fixed; the list is nearest-first so the cap keeps the contacts that matter.
// Measured on hardware at ~437 bytes per row, so the cap is worth ~10.5 KB of pool. At
// the couple of dozen contacts a local receiver typically shows it changes nothing; what
// it protects against is ADSB_MAX_AIRCRAFT in busy airspace, where 120 rows would want
// ~52 KB and the S3 only has ~23 KB of pool free once every screen has been visited.
#define UI_LIST_MAX_ROWS    24

#define ADSB_MAX_AIRCRAFT   120             // hard cap parsed per poll (protect RAM in busy areas)
// When the primary feed answers cleanly but with zero aircraft, re-probe the fallback
// only every Nth poll. An empty sky is normal and can last hours; see AdsbClient::poll.
#define ADSB_EMPTY_RECHECK_POLLS 30         // 30 x POLL_INTERVAL_MS = ~1 min while empty

// ---------- Self-update (GitHub Pages, published by the web-flasher workflow) ----------
// The workflow stamps manifest.json with FW_VERSION and publishes firmware.bin beside
// it, so the device can compare versions and pull the image itself. Point these at your
// own fork's Pages site if you republish.
#define UPDATE_MANIFEST_URL  "https://silentwolf75.github.io/skyglass/manifest.json"
#define UPDATE_FIRMWARE_URL  "https://silentwolf75.github.io/skyglass/firmware.bin"
#define UPDATE_CHECK_INTERVAL_MS 21600000UL   // 6 h; releases are not that frequent

// ---------- Debug ----------
#define DEBUG_MEM           0               // 1 = print a [mem] heap/fps line every 5s on serial
