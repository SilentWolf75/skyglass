#pragma once
// M3 UI: swipeable views (radar / list / stats) + tap-to-inspect detail card.
// Pure LVGL, portable (device + SDL simulator). Builds on top of radar_view.
#include <stddef.h>   // size_t (ui_format_clock)
void ui_create(void);            // build the whole UI on the active screen
void ui_on_data_updated(void);
void ui_track_selected(bool on);   // remote equivalent of the card TRACK button (/view?trk=)
// Re-tint the on-scope controls after a theme change (long-press or the config page).
void ui_theme_changed(void);
void ui_show_view(int idx);      // 0 radar, 1 list, 2 stats, 3 weather, 4 tracked, 5 clock, 6 about
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock);  // HUD: signal bars (count=RSSI, colour: red=down, amber=stale feed, white=ok) + clock
void ui_set_battery(int pct, bool charging, bool present);  // top HUD battery indicator
void ui_set_date(const char *date);  // top HUD date line (e.g. "08 Jun 2026")
// Which feed the last poll actually came from: 0 internet, 1 the local receiver, -1
// not known yet. Worth showing -- the two answer with different skies, and there is no
// other way to tell from the device which one you are looking at.
void ui_set_feed_source(int src);
// Tapping the weather zoom buttons calls this. The UI does not fetch anything itself --
// the tiles are pulled by the network task, which owns the schedule -- so this is how it
// gets told to go again now instead of waiting out the refresh interval.
typedef void (*UiWxZoomCb)(int zoom);
void ui_set_wx_zoom_cb(UiWxZoomCb cb);
void ui_set_netinfo(const char *line);  // stats view footer: how to reach the config page
void ui_set_gps(int state, int sats);   // GPS indicator: state 0=off/hidden 1=acquiring 2=fix; HUD icon + Stats line
void ui_splash_show(void);  // branded boot splash (auto-fades, covers init time)
void ui_set_range_cb(void (*cb)(float km));  // on-screen zoom button -> notify main
void ui_set_range_km(float km);              // update the zoom button label / sync the cycle
void ui_set_range_preview_cb(void (*cb)(float km)); // live pinch-zoom preview (visual only, no feed re-query)
void ui_pinch_touch(int nPoints, int x0, int y0, int x1, int y1); // raw multi-touch feed (device driver)
void ui_set_units(int preset);               // 0 = Aviation (ft,kt,km) · 1 = Metric (m,km/h,km) · 2 = Imperial (ft,mph,mi)
void ui_set_time_24h(bool on);               // false = 12-hour clock with AM/PM
bool ui_time_24h(void);
// Shared clock formatter so the HUD, the clock face and the imagery timestamps agree.
// `withSuffix` appends " AM"/" PM" in 12-hour mode (skip it where space is tight).
void ui_format_clock(char *buf, size_t n, int hour, int min, bool withSuffix);
void ui_set_large_text(bool on);             // accessibility: bigger fonts everywhere. Call BEFORE ui_create()
void ui_set_weather_forecast(bool forecast); // false = WX radar, true = 3-day forecast
void ui_set_weather_mode(int mode);          // 0 = WX radar, 1 = sat clouds, 2 = 3-day forecast
void ui_preview_weather_icon(int wmoCode);   // diagnostic: force the glyph set to one code
void ui_show_flash_screen(const char *status, int pct); // Full-screen firmware flashing progress screen

// ---- On-device settings menu --------------------------------------------------------
// The menu on the SETTINGS & STATS screen is generated from a list the firmware supplies
// rather than hand-placed buttons, so it stays in step with the web page instead of being
// a hand-maintained subset of it. main.cpp owns the settings and their NVS keys; ui.cpp
// only draws rows and reports taps back by index.
typedef bool (*UiToggleGet)(int idx);
typedef void (*UiToggleSet)(int idx, bool on);
typedef const char *(*UiToggleLabel)(int idx);
void ui_set_toggle_provider(int count, UiToggleLabel label, UiToggleGet get, UiToggleSet set);
