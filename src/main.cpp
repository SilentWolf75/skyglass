// SkyGlass — entry point / glue. SKELETON: TODOs mark what to implement.
// Order of work is in CLAUDE.md (milestones). Bring up the Waveshare demo first.
#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include "config.h"
#include "aircraft.h"
#include "geo.h"
#include "adsb_client.h"
#include "snapshot_gate.h"
#include "route.h"
#include "route_client.h"
#include "photo.h"
#include "photo_client.h"
#include "airline.h"
#include "map_bg.h"
#include "map_client.h"
#include "vessel.h"
#include "ais_client.h"
#include "wav_upload.h"
#include "updater.h"
#include <LittleFS.h>
#include "sd_store.h"
#include "weather.h"
#include "weather_client.h"
#include "wx_radar.h"
#include "wx_radar_client.h"
#include "cloud_image.h"
#include "cloud_image_client.h"
#include "radar_view.h"
#include "ui.h"
#include "display.h"                  // M0: CO5300 + LVGL bring-up
#include "imu_qmi8658.h"             // face-down sleep
#include "gps.h"                     // LC76G GNSS (-G variant only)
#include "battery.h"                 // AXP2101 battery gauge
#include "rtc_pcf85063.h"            // PCF85063 RTC (offline clock + date)
#include "audio.h"                   // ES8311 alert pings
#include <set>                       // audio: track which contacts are in range
#include <string>
#if BOARD_HAS_WIFIMANAGER
#include <WiFiManager.h>             // captive portal
#endif
#include <Preferences.h>            // NVS (persist theme/settings)
#include <time.h>                   // NTP/RTC clock + date
#include <WebServer.h>              // configuration web page
#include <ESPmDNS.h>                // http://<board hostname>.local
#include <ArduinoOTA.h>             // OTA firmware update over WiFi (PlatformIO/espota)
#include <Update.h>                 // browser OTA: self-flash an uploaded .bin
#include <esp_heap_caps.h>          // largest-free-block metric (heap health)
#include <esp_wifi.h>               // WiFi driver control (reset must survive the reboot)
#include <nvs.h>                    // erase the driver's "nvs.net80211" namespace (WiFi reset)

// ---- shared state ----
static std::vector<Aircraft> g_aircraft;      // latest snapshot
static SemaphoreHandle_t     g_ac_mutex;      // guards g_aircraft
static volatile bool         g_acDirty = false; // set when a new snapshot is ready
static AdsbClient            g_adsb;
static RadarSettings         g_settings;
#if BOARD_HAS_WIFIMANAGER
static WiFiManager           g_wm;
#endif
static int                   g_brightnessDay = BRIGHTNESS_DEFAULT;   // user brightness (web/NVS)
static int                   g_volume = 60;                          // alert volume 0..100 (web/NVS)
static bool                  g_muted  = false;                       // mute alert pings
static int                   g_packNew   = AUDIO_PACK_CHIME;         // sound for new contacts (web/NVS)
static int                   g_packAlert = AUDIO_PACK_CHIME;         // sound for emergencies (web/NVS)
// Alert mode is a bitmask: bit 0 = new contacts, bit 1 = emergencies.
// 0 = off, 1 = new only, 2 = emergencies only, 3 = both.
#define ALERT_NEW   0x1
#define ALERT_EMERG 0x2
static int                   g_alertMode = ALERT_NEW | ALERT_EMERG;  // (web/NVS)
static float                 g_proximityKm = 0.0f;                   // proximity alert radius, km (0=off) (web/NVS)
static uint32_t              g_idleDimMs = IDLE_DIM_MS;              // dim after this idle time (0 = never)
static bool                  g_showSweep = true;                     // rotating sweep line on/off (web/NVS)
static int                   g_units = 0;                            // 0=Aviation 1=Metric 2=Imperial (web/NVS)
static bool                  g_showAirports = true;                  // airport markers on/off (web/NVS)
static bool                  g_hideGround   = false;                 // skip on-ground aircraft in the feed (web/NVS)
static int                   g_minAltFt     = 0;                     // only show aircraft above this altitude, ft (0 = off) (web/NVS)
static bool                  g_milOnly      = false;                 // only show military-flagged aircraft (web/NVS)
static int                   g_rotation = 0;                         // clockwise display rotation, 0..359° (web/NVS)
static bool                  g_useGps = false;                       // auto-set home from the LC76G GPS (-G variant) (web/NVS)
static int                   g_trailLen = 2;                         // aircraft trails 0=off 1=short 2=med 3=long (web/NVS)
// Where the frame time actually goes. The sweep tick only runs as often as loop()
// completes, so if something outside rendering is slow the sweep stutters no matter how
// it is drawn -- and no amount of trail tuning will touch it.
static volatile uint32_t g_loopLvglUs = 0, g_loopRestUs = 0, g_loopCount = 0;
static float g_loopLvglMs = 0, g_loopRestMs = 0;

static int                   g_maxAc = 20;                           // max aircraft drawn on the scope (web/NVS)
static bool                  g_bigText = false;                      // accessibility: large fonts (web/NVS, applied at boot)
static volatile bool         g_onBattery = false;                    // discharging (set on core 1, read on core 0)
static bool                  g_rtcSynced = false;                    // RTC written from NTP this session?
static std::vector<Aircraft> g_snap;                                 // last snapshot (instant re-render on zoom)
static volatile bool         g_requery = false;                      // range changed -> adsb_task re-begins
static float                 g_requeryKm = 0.0f;
static volatile bool         g_feedOk = true;                        // ADS-B feed healthy? (HUD warning)
static volatile uint32_t     g_lastFeedOkMs = 0;                     // millis() of the last good poll (HUD staleness)
static volatile uint32_t     g_rebootAtMs = 0;                       // !=0: reboot when millis() reaches it (clean start after WiFi config)
static String                g_tz = TZ_STR;                          // POSIX timezone (web-configurable, NVS); applied via configTzTime
static int                   g_qhMode  = 0;                          // quiet hours: 0=off 1=dim 2=screen off 3=clock screen (web/NVS)
static int                   g_qhStart = 22 * 60;                    // quiet-hours start, minutes since midnight (web/NVS)
static int                   g_qhEnd   = 7 * 60;                     // quiet-hours end, minutes since midnight (web/NVS)
static bool                  g_inQuiet = false;                      // currently inside the quiet-hours window
static volatile bool         g_mapDirty = false;
static bool                  g_mapBg = false;                        // map-tile background on/off (web/NVS)
static int                   g_mapStyle = 0;                         // 0 = dark, 1 = light (web/NVS)
static int                   g_mapOpa = 85;                          // basemap visibility / opacity % 0..100 (web/NVS)
static bool                  g_time24 = false;                       // false = 12-hour clock (web/NVS)
static bool                  g_typeIcons = true;                     // per-type aircraft silhouettes (web/NVS)
static int                   g_trafficMode = 0;                      // 0 = aircraft, 1 = marine (web/NVS)
static int                   g_adsbSrc = 0;                          // 0 auto, 1 local only, 2 public only
static String                g_adsbLocal;                            // dump1090/tar1090 host on the LAN ("" = public feeds)
static String                g_aisKey;                               // aisstream.io API key (web/NVS; empty = off)
static volatile bool         g_weatherDirty = false;
static volatile bool         g_wxRadarDirty = false;
static volatile bool         g_cloudImageDirty = false;

// Web-selectable time zones (label + POSIX TZ). The <option> value is the index; the save
// handler maps it back to the POSIX string stored in NVS and used by configTzTime at boot.
// (Index avoids putting POSIX strings with '<>' / ',' into HTML attributes.)
// offMin = standard (winter) UTC offset in minutes; dst = 1 if the zone observes DST.
// The web page uses these to auto-pick the visitor's zone from their browser clock.
static const struct { const char *label; const char *tz; int offMin; int dst; } TZOPTS[] = {
    {"UTC",                      "UTC0",                              0, 0},
    {"London / Lisbon",          "GMT0BST,M3.5.0/1,M10.5.0",          0, 1},
    {"Madrid / Paris / Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3",       60, 1},
    {"Athens / Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4",     120, 1},
    {"New York (US Eastern)",    "EST5EDT,M3.2.0,M11.1.0",          -300, 1},
    {"Chicago (US Central)",     "CST6CDT,M3.2.0,M11.1.0",          -360, 1},
    {"Denver (US Mountain)",     "MST7MDT,M3.2.0,M11.1.0",          -420, 1},
    {"Phoenix (Arizona)",        "MST7",                            -420, 0},
    {"Los Angeles (US Pacific)", "PST8PDT,M3.2.0,M11.1.0",          -480, 1},
    {"Anchorage (Alaska)",       "AKST9AKDT,M3.2.0,M11.1.0",        -540, 1},
    {"Honolulu (Hawaii)",        "HST10",                           -600, 0},
    {"Argentina / Brazil (E)",   "<-03>3",                          -180, 0},
    {"India (IST)",              "<+0530>-5:30",                     330, 0},
    {"China / Singapore",        "<+08>-8",                          480, 0},
    {"Japan / Korea",            "JST-9",                            540, 0},
    {"Sydney (AU Eastern)",      "AEST-10AEDT,M10.1.0,M4.1.0/3",     600, 1},
    {"Auckland (NZ)",            "NZST-12NZDT,M9.5.0,M4.1.0/3",      720, 1},
};
static const int TZOPTS_N = sizeof(TZOPTS) / sizeof(TZOPTS[0]);

static float queryRadiusKm();   // defined below; the feed task needs it for the fire bbox

// --- setup portal fallback (boards without WiFiManager) ----------------------------
// The S3 gets this free: WiFiManager's autoConnect() opens a captive portal whenever it
// cannot reach the stored network. The P4 has no WiFiManager, and it only raised its
// setup AP when *no* credentials were stored at all -- so a board carrying credentials
// for one network, powered up somewhere else, sat retrying a network that was not there
// with no way in and nothing on screen. That is exactly what happened taking it to
// someone else's house.
//
// So: if the station has not associated for a while, bring the setup AP up alongside it.
// AP_STA rather than AP, so the station keeps trying in the background and the portal
// disappears by itself if the real network comes back (someone powers the router on, or
// you carry it home) without needing a reboot.
#if !BOARD_HAS_WIFIMANAGER
#ifndef WIFI_PORTAL_AFTER_MS
#  define WIFI_PORTAL_AFTER_MS 45000UL
#endif
static bool     g_setupApUp   = false;
static bool     g_setupApFail = false;   // radio refused AP mode; say so rather than lie
static uint32_t g_staWaitMs   = 0;

static void wifi_portal_tick(void) {
    if (WiFi.status() == WL_CONNECTED) {
        if (g_setupApUp) {                       // real network is back; drop the portal
            Serial.println("[wifi] station associated -> taking the setup AP down");
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            g_setupApUp = false;
        }
        g_staWaitMs = millis();
        g_setupApFail = false;
        return;
    }
    if (g_setupApUp || g_setupApFail) return;
    if (g_staWaitMs == 0) { g_staWaitMs = millis(); return; }
    if (millis() - g_staWaitMs < WIFI_PORTAL_AFTER_MS) return;

    Serial.printf("[wifi] no association after %lus -> raising setup AP '%s'\n",
                  (unsigned long)(WIFI_PORTAL_AFTER_MS / 1000), BOARD_SETUP_AP);
    // Keep the station alive so it can still find the stored network on its own.
    g_setupApUp = WiFi.mode(WIFI_AP_STA) && WiFi.softAP(BOARD_SETUP_AP);
    if (!g_setupApUp) g_setupApUp = WiFi.mode(WIFI_AP) && WiFi.softAP(BOARD_SETUP_AP);
    if (g_setupApUp) {
        Serial.printf("[wifi] setup AP up: join '%s', open http://%s/\n",
                      BOARD_SETUP_AP, WiFi.softAPIP().toString().c_str());
    } else {
        g_setupApFail = true;   // do not spin retrying a mode this radio will not enter
        Serial.println("[wifi] setup AP FAILED - set credentials over serial: wifi <ssid> <password>");
    }
}
#endif

// ---- networking task (core 0): fetch + parse, never touches the display ----
static void adsb_task(void*) {
    std::vector<Aircraft> fresh;
    AircraftSnapshotGate snapshotGate;
    bool retainingEmptySnapshot = false;
    bool wasConnected = false;
    uint32_t lastPoll = 0;
    uint32_t nextWeatherAt = UINT32_MAX;       // armed five seconds after WiFi connects
    uint32_t nextWxRadarAt = UINT32_MAX;
    uint32_t nextCloudImageAt = UINT32_MAX;
    uint32_t lastFeedOk = millis();          // self-heal: time of last good (or no-WiFi) poll
    uint8_t  healStage = 0;                  // 0 none, 1 reassociate attempted
    uint32_t healActionMs = 0;               // when the last rung ran (see the !conn reset)
    for (;;) {
        const bool conn = (WiFi.status() == WL_CONNECTED);
        if (conn && !wasConnected) {
            // disable WiFi modem power-save: on a mains-powered desk gadget it just adds latency
            // and makes RSSI bounce (feed goes stale -> amber bars) even sitting next to the router.
            WiFi.setSleep(false);
            Serial.printf("[adsb] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
            configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");  // local time (web-configurable TZ)
            Serial.println("[web] config: http://" BOARD_HOSTNAME ".local/  (or the IP above)");
            nextWeatherAt = millis() + 5000UL; // let the first ADS-B poll complete before weather TLS
            nextCloudImageAt = millis() + 15000UL;
            nextWxRadarAt = millis() + 12000UL;
            // mDNS + OTA are started on core 1 (loop) to keep all mDNS use on one core
        }
        wasConnected = conn;
        // Self-heal. A long feed outage while WiFi still reports connected has been seen
        // on the P4: every socket fails while WiFi.status() stays WL_CONNECTED and the heap
        // is healthy (245 KB free when it was caught), so the wedge sits below the status
        // API -- in the C6 link, not in allocation. A reboot clears it, and that is what
        // made the device restart on its own; try one cheap thing first.
        //
        // A full station restart (WiFi.disconnect(true) + begin) was tried here as a middle
        // rung and removed: measured on the P4, the station never came back, and because
        // the reboot below is gated on being connected the device sat unreachable instead
        // of recovering. Reassociating is the only intermediate step that is known to
        // return, so it is the only one kept.
        if (!conn) {
            if (healStage == 0) {
                lastFeedOk = millis();          // plain outage: a router reboot must not
                                                // eventually restart the device
            } else if (millis() - healActionMs > 60000UL) {
                // We prodded the radio and it has not come back for a minute. An
                // unreachable device cannot be recovered remotely, so take the reboot.
                Serial.println("[adsb] no association since self-heal -> rebooting");
                delay(100);
                ESP.restart();
            }
        } else {
            const uint32_t stuckMs = millis() - lastFeedOk;
            if (stuckMs > 180000UL) {
                Serial.println("[adsb] feed stuck >180s after reassociating -> rebooting");
                delay(100);
                ESP.restart();
            } else if (stuckMs > 45000UL && healStage < 1) {
                // Cheapest rung: reassociate. Costs a second or two of link and nothing
                // else, and on the P4 it is observed to return within a few seconds.
                Serial.println("[adsb] feed stuck >45s -> reassociating");
                WiFi.reconnect();
                healStage = 1;
                healActionMs = millis();
            }
        }
        if (g_requery) {                          // display range changed (double-tap zoom)
            g_adsb.begin(g_settings.homeLat, g_settings.homeLon, g_requeryKm);
            g_requery = false;
            lastPoll = 0;                         // poll immediately at the new radius
        }
        if (conn) {
            // The live aircraft feed is the primary job, so poll FIRST every cycle. That keeps
            // it refreshing even while the user taps around — a slow route/photo lookup (below)
            // can block this single network task, so it must never get ahead of the feed.
            const uint32_t nowMs = millis();
            const uint32_t pollInterval = g_onBattery ? POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS;
            if (lastPoll == 0 || nowMs - lastPoll >= pollInterval) {  // aircraft feed
                lastPoll = nowMs;
                static int failCount = 0;
                // poll() tries the fallback provider after a primary failure; keep the HUD
                // healthy through isolated misses and warn only after a sustained outage.
                if (g_adsb.poll(fresh)) {
                    Serial.printf("[adsb] fetched %u aircraft\n", (unsigned)fresh.size());
                    failCount = 0;
                    g_feedOk = true;
                    const uint32_t receivedMs = millis();
                    lastFeedOk = receivedMs;
                    healStage = 0;                    // recovered: re-arm the whole ladder

                    // Flight log. Runs here rather than in the UI because it writes to
                    // the card, and a slow write must never land in the LVGL loop. Costs
                    // nothing when there is no card: sd_log_seen() returns immediately.
                    if (sd_mounted()) {
                        const time_t nowT = time(nullptr);
                        const uint32_t ep = (nowT > 1000000000) ? (uint32_t)nowT : 0;
                        for (const Aircraft &a : fresh) {
                            if (a.hex.isEmpty()) continue;
                            const float dKm = geo::haversineKmf((float)g_settings.homeLat,
                                                                (float)g_settings.homeLon,
                                                                (float)a.lat, (float)a.lon);
                            sd_log_seen(a.hex.c_str(), a.flight.c_str(), dKm, ep);
                        }
                    }
                    healActionMs = 0;
                    g_lastFeedOkMs = receivedMs;      // HUD: mark data as fresh

                    const bool publish = snapshotGate.shouldPublish(
                        !fresh.empty(), receivedMs, AC_STALE_MS);
                    if (!publish) {
                        if (!retainingEmptySnapshot) {
                            Serial.printf("[adsb] empty snapshot; retaining contacts for %u ms\n",
                                          (unsigned)AC_STALE_MS);
                            retainingEmptySnapshot = true;
                        }
                    } else {
                        if (retainingEmptySnapshot && fresh.empty())
                            Serial.println("[adsb] empty snapshot persisted; clearing stale contacts");
                        retainingEmptySnapshot = false;
                        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                            g_aircraft.swap(fresh);   // O(1) handoff: no per-Aircraft String copies under the lock
                            g_acDirty = true;
                            xSemaphoreGive(g_ac_mutex);
                        }
                    }
                } else {
                    Serial.println("[adsb] poll failed");
                    if (++failCount >= 5) g_feedOk = false;   // sustained outage -> HUD warning
                }
            }
            // Forecasts change slowly. Fetch only after the live ADS-B poll has had priority.
            if ((int32_t)(nowMs - nextWeatherAt) >= 0) {
                Serial.printf("[weather] fetching %.5f, %.5f...\n",
                              g_settings.homeLat, g_settings.homeLon);
                WeatherSnapshot forecast;
                if (weather_fetch(g_settings.homeLat, g_settings.homeLon, forecast)) {
                    weather_store(forecast);
                    g_weatherDirty = true;
                    nextWeatherAt = millis() + WEATHER_REFRESH_MS;
                    Serial.println("[weather] forecast updated");
                } else {
                    nextWeatherAt = millis() + 60000UL;
                    Serial.println("[weather] fetch failed; retrying in 60s");
                }
            }
            if ((int32_t)(nowMs - nextWxRadarAt) >= 0) {
                if (wx_radar_fetch(g_settings.homeLat, g_settings.homeLon)) {
                    g_wxRadarDirty = true;
                    // One frame per call. While the loop's backlog is still coming down,
                    // come straight back for the next one -- the whole two-hour history is
                    // about 36 KB, so it assembles in half a minute. Once complete, settle
                    // to the normal cadence: RainViewer only publishes a new frame every
                    // ten minutes and the fetcher skips what it already holds.
                    const int left = wx_radar_backlog();
                    nextWxRadarAt = millis() + (left > 0 ? 2500UL : WX_RADAR_REFRESH_MS);
                    if (left > 0) Serial.printf("[wxradar] %d frames to go\n", left);
                } else {
                    nextWxRadarAt = millis() + 60000UL;
                    Serial.println("[wxradar] fetch failed; retrying in 60s");
                }
            }
            if ((int32_t)(nowMs - nextCloudImageAt) >= 0) {
                Serial.println("[clouds] fetching EUMETSAT frame...");
                if (cloud_image_fetch(g_settings.homeLat, g_settings.homeLon)) {
                    g_cloudImageDirty = true;
                    nextCloudImageAt = millis() + CLOUD_IMAGE_REFRESH_MS;
                } else {
                    nextCloudImageAt = millis() + 60000UL;
                    Serial.println("[clouds] fetch failed; retrying in 60s");
                }
            }
            // AIS: non-blocking socket pump; also keeps the subscription box in sync
            // with the scope as the range or home position changes.
            if (ais_has_key()) {
                ais_configure(g_settings.homeLat, g_settings.homeLon, queryRadiusKm());
                ais_loop();
            }
            // Map background: one tile per iteration, so a 9-16 tile build never blocks
            // the live feed. Returns true while more work remains.
            if (map_client_enabled()) {
                static bool mapWasBusy = false;
                map_client_request(g_settings.homeLat, g_settings.homeLon, g_settings.rangeKm);
                const bool busy = map_client_step();
                // The commit happens on the step that finishes the build, and that step
                // reports "no work left" — so flag the repaint on the busy->idle edge.
                if (mapWasBusy && !busy) g_mapDirty = true;
                mapWasBusy = busy;
            }
            // Then the on-demand lookups for the selected aircraft. Their timeouts are kept
            // short (see photo_client / route_client) so a slow photo server can't freeze the
            // feed for long; the next loop iteration polls again as soon as they return.
            char wantCall[12];
            if (route_pending(wantCall, sizeof(wantCall))) {
                char from[40] = "", to[40] = "";
                RouteCoords rc;
                if (route_cache_get(wantCall, from, sizeof(from), to, sizeof(to), &rc)) {
                    route_store_full(wantCall, from, to, rc);              // NVS hit, no network
                    Serial.printf("[route] %s (cache): '%s' -> '%s'\n", wantCall, from, to);
                } else if (route_fetch(wantCall, from, sizeof(from), to, sizeof(to), &rc)) {
                    route_store_full(wantCall, from, to, rc);
                    route_cache_put(wantCall, from, to, &rc);             // remember across reboots
                    Serial.printf("[route] %s (net): '%s' -> '%s'\n", wantCall, from, to);
                } else {
                    route_store(wantCall, from, to);   // empty -> don't refetch this session
                    Serial.printf("[route] %s: no route\n", wantCall);
                }
            }
            char wantHex[10];
            if (photo_pending(wantHex, sizeof(wantHex))) photo_fetch(wantHex);
            // Continental fire map (FIRES screen). Fetch + banded parse; returns
            // true while busy so the UI can repaint on the busy->idle edge.
            // Self-update. Deliberately last in the task: a check is cheap but an
            // install streams 3 MB, and the live feed should have had its turn first.
            updater_poll();
            char wantReg[10];
            if (reg_pending(wantReg, sizeof(wantReg))) {
                char reg[12] = "", ty[24] = "";
                reg_fetch(wantReg, reg, sizeof(reg), ty, sizeof(ty));
                reg_store(wantReg, reg, ty);          // store even when empty: don't refetch
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void loadSettings() {
    Preferences p;
    p.begin("capsuleradar", true);
    g_settings.homeLat = p.getDouble("homeLat", HOME_LAT_DEFAULT);
    g_settings.homeLon = p.getDouble("homeLon", HOME_LON_DEFAULT);
    g_settings.rangeKm = p.getFloat("rangeKm", RANGE_KM_DEFAULT);
    g_brightnessDay    = p.getInt("bright", BRIGHTNESS_DEFAULT);
    g_volume           = p.getInt("vol", 60);
    g_muted            = p.getBool("mute", false);
    // A custom sound is almost certainly why someone uploaded one, so default the
    // emergency cue to it; routine new-contact pings stay on the gentler chime.
    const int defAlert = audio_has_sample() ? AUDIO_PACK_CUSTOM : AUDIO_PACK_CHIME;
    g_packNew          = p.getInt("packnew",   p.getInt("sndpack", AUDIO_PACK_CHIME));
    g_packAlert        = p.getInt("packalert", p.getInt("sndpack", defAlert));
    // Alert mode changed meaning (it is a bitmask now), so it lives under a new key and
    // old installs are migrated rather than silently reinterpreted: the previous value 2
    // meant "new + emergencies", which as a bitmask would read as "emergencies only".
    if (p.isKey("alertmode2")) {
        g_alertMode = p.getInt("alertmode2", ALERT_NEW | ALERT_EMERG);
    } else {
        const int legacy = p.getInt("alertmode", 2);
        g_alertMode = (legacy == 0) ? 0 : (legacy == 1 ? ALERT_EMERG : (ALERT_NEW | ALERT_EMERG));
    }
    g_alertMode = constrain(g_alertMode, 0, 3);
    g_proximityKm      = p.getFloat("proxkm", 0.0f);
    g_useGps           = p.getBool("usegps", false);
    g_trailLen         = p.getInt("traillen", 2);
    g_maxAc            = p.getInt("maxac", 20);
    g_idleDimMs        = p.getUInt("idledim", IDLE_DIM_MS);
    g_units            = p.getInt("units", 0);
    g_tz               = p.getString("tz", TZ_STR);
    g_qhMode           = p.getInt("qhmode", 0);
    g_qhStart          = p.getInt("qhstart", 22 * 60);
    g_qhEnd            = p.getInt("qhend", 7 * 60);
    g_mapBg            = p.getBool("mapbg", false);
    g_mapStyle         = p.getInt("mapstyle", 0);
    g_mapOpa           = p.getInt("mapopa", 85);
    g_trafficMode      = p.getInt("traffic", 0);
    g_aisKey           = p.getString("aiskey", "");
    g_adsbLocal        = p.getString("adsblocal", "");
    g_adsbSrc          = p.getInt("adsbsrc", 0);
    g_time24           = p.getBool("time24", false);
    g_typeIcons        = p.getBool("typeicons", true);
    g_bigText          = p.getBool("bigtext", false);
    p.end();
    ui_set_time_24h(g_time24);
    map_client_set_style(g_mapStyle);
    map_client_enable(g_mapBg);
    ais_set_key(g_aisKey.c_str());
    g_adsb.setLocalHost(g_adsbLocal.c_str());
    g_adsb.setSourceMode(g_adsbSrc);
    // fonts are baked into the widgets at creation time, so the large-text flag must be
    // in place before display::begin() builds the UI (loadSettings runs first in setup)
    ui_set_large_text(g_bigText);
    radar::setLargeText(g_bigText);
}

// Audio alerts. g_alertMode: 0 = off, 1 = emergencies only, 2 = new aircraft + emergencies.
// g_proximityKm > 0 also pings (once) when any aircraft crosses into that radius.
static void checkAudioEvents() {
    if (!audio_present()) return;
    static std::set<std::string> seen, seenProx;
    static bool first = true;
    static uint32_t lastNew = 0;
    std::set<std::string> now, nowProx;
    for (const Aircraft &ac : g_snap) {
        const double d = geo::haversineKm(g_settings.homeLat, g_settings.homeLon, ac.lat, ac.lon);
        if (d > g_settings.rangeKm) continue;                 // in-range only
        const std::string hex = ac.hex.c_str();
        now.insert(hex);
        const bool isNew     = !first && !seen.count(hex);
        const bool emergency = acIsEmergency(ac.squawk) || ac.military;  // military: feed dbFlags

        // proximity: fire once, when an aircraft first crosses into the radius (any aircraft)
        if (g_proximityKm > 0.0f && d <= g_proximityKm) {
            nowProx.insert(hex);
            if (!first && !seenProx.count(hex)) audio_play(AUDIO_ALERT);
        }

        // new-in-range pings (on entry), gated independently by each alert-mode bit
        if (isNew) {
            if (emergency) { if (g_alertMode & ALERT_EMERG) audio_play(AUDIO_ALERT); }
            else if ((g_alertMode & ALERT_NEW) && millis() - lastNew > 3000) {
                audio_play(AUDIO_NEW);                                          // new contact (rate-limited)
                lastNew = millis();
            }
        }
    }
    seen.swap(now);
    seenProx.swap(nowProx);
    first = false;
}

// Feed query radius: wider than the display range (so off-range traffic shows as edge
// arrows) AND wide enough to cover the proximity-alert circle (else an alert radius larger
// than the query would never fire), clamped to keep busy-airspace downloads bounded.
static float queryRadiusKm() {
    float km = g_settings.rangeKm * ADSB_QUERY_MULT;
    if (g_proximityKm > 0.0f && g_proximityKm * 1.2f > km) km = g_proximityKm * 1.2f;
    return constrain(km, ADSB_QUERY_MIN_KM, ADSB_QUERY_MAX_KM);
}

// Double-tap zoom: change the display range, persist it, and ask adsb_task to
// re-query at a matching radius (safely, on its own core). Re-render immediately.
static void onRangeChange(float km) {
    g_settings.rangeKm = km;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putFloat("rangeKm", km);
    p.end();
    g_requeryKm = queryRadiusKm();
    g_requery = true;
    radar::update(g_snap, g_settings);   // instant visual zoom from the last snapshot
    ui_set_range_km(km);
    ui_on_data_updated();
}

// Pinch-zoom preview: re-project the last snapshot at the new range every ~120 ms
// while the gesture runs. No NVS write / feed re-query until the fingers lift
// (ui_pinch_touch then calls onRangeChange with the final value).
static void onRangePreview(float km) {
    g_settings.rangeKm = km;
    radar::update(g_snap, g_settings);
    ui_set_range_km(km);
}

// Persist the visual theme in NVS (called when the user long-presses to switch).
static void saveTheme(int t) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("theme", t);
    p.end();
    ui_theme_changed();          // long-press switches live; re-tint the on-scope controls
}

// Convert a UTC broken-down time to time_t (mktime assumes local TZ, so flip to UTC0).
static time_t utc_to_time(struct tm *utc) {
    setenv("TZ", "UTC0", 1); tzset();
    const time_t t = mktime(utc);
    setenv("TZ", TZ_STR, 1); tzset();   // restore local TZ for getLocalTime()
    return t;
}

// Seed the ESP system clock from the RTC so the clock/date are right before NTP.
static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { Serial.println("[rtc] no valid time stored"); return; }
    const time_t t = utc_to_time(&utc);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[rtc] system clock seeded from RTC");
}

// Quiet-hours window test (minutes since local midnight; window may wrap midnight).
static bool quietWindowNow() {
    if (g_qhMode == 0 || g_qhStart == g_qhEnd) return false;
    struct tm ti;
    if (!getLocalTime(&ti, 0)) return false;          // clock not set yet -> no quiet hours
    const int now = ti.tm_hour * 60 + ti.tm_min;
    if (g_qhStart < g_qhEnd) return now >= g_qhStart && now < g_qhEnd;
    return now >= g_qhStart || now < g_qhEnd;          // overnight window (e.g. 22:00-07:00)
}

// Brightness combines idle auto-dim, quiet hours and face-down sleep (sleep wins).
static bool g_asleep = false;   // face-down
static bool g_idle   = false;   // no touch for a while
static void applyBrightness() {
    int b = g_brightnessDay;
    if (g_idle  && BRIGHTNESS_IDLE  < b) b = BRIGHTNESS_IDLE;   // idle only dims down
    if (g_inQuiet && display::inactiveMs() >= 15000) {           // a touch wakes it for 15 s
        if (g_qhMode == 2) b = 0;                                // screen off
        else if (BRIGHTNESS_IDLE < b) b = BRIGHTNESS_IDLE;       // dim / clock modes
    }
    if (g_asleep) b = 0;                                         // face-down -> screen off
    display::setBrightness(b);
}

// ----------------------------- configuration web --------------------------------
static WebServer g_web(80);

static void handleRoot() {
    const int th = radar::theme();
    // Mirror RANGE_STEPS_KM exactly. Offering ranges the device table lacks looked fine
    // until you touched the on-device range button: ui.cpp snaps to the nearest step, so
    // a web-set 250 km collapsed to 100 and could not be restored without the web page.
    const float *ranges = RANGE_STEPS_KM;
    const int nRanges = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    const float curRange = g_settings.rangeKm;
    const float ufac  = (g_units == 0) ? 0.539957f : (g_units == 2 ? 0.621371f : 1.0f);
    const char *uname = (g_units == 0) ? "nm" : (g_units == 2 ? "mi" : "km");

    auto fmt_opt = [ufac, uname](float km, bool sel) -> String {
        char o[80];
        const float val = km * ufac;
        if (fabsf(val - roundf(val)) < 0.15f) {
            snprintf(o, sizeof(o), "<option value=\"%.2f\"%s>%.0f %s</option>",
                     (double)km, sel ? " selected" : "", (double)roundf(val), uname);
        } else {
            snprintf(o, sizeof(o), "<option value=\"%.2f\"%s>%.1f %s</option>",
                     (double)km, sel ? " selected" : "", (double)val, uname);
        }
        return String(o);
    };

    bool curListed = false;
    for (int i = 0; i < nRanges; ++i) {
        if (fabsf(ranges[i] - curRange) < 0.1f) { curListed = true; break; }
    }

    String ropts;
    bool spliced = curListed;
    for (int i = 0; i < nRanges; ++i) {
        if (!spliced && curRange < ranges[i]) {
            ropts += fmt_opt(curRange, true);
            spliced = true;
        }
        const bool sel = fabsf(ranges[i] - curRange) < 0.1f;
        ropts += fmt_opt(ranges[i], sel);
    }
    if (!spliced) {
        ropts += fmt_opt(curRange, true);
    }
    const char *tnames[] = {"Phosphor", "Orb", "Amber CRT", "Military", "Red CRT"};
    String topts;
    for (int i = 0; i < THEME_COUNT; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == th ? " selected" : "", tnames[i]);
        topts += o;
    }
    const int idleSecs[] = {10, 20, 30, 60, 120, 300, 1800, 3600, 7200, 14400, 28800};
    const int curIdle = (int)(g_idleDimMs / 1000);
    String iopts;
    for (int sV : idleSecs) {
        char lbl[16];
        if      (sV < 60)   snprintf(lbl, sizeof(lbl), "%d s", sV);
        else if (sV < 3600) snprintf(lbl, sizeof(lbl), "%d min", sV / 60);
        else                snprintf(lbl, sizeof(lbl), "%d h", sV / 3600);
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", sV, sV == curIdle ? " selected" : "", lbl);
        iopts += o;
    }
    { char o[64]; snprintf(o, sizeof(o), "<option value=0%s>Never</option>", curIdle == 0 ? " selected" : ""); iopts += o; }
    char sndStatus[96];
    if (audio_sample_len())
        snprintf(sndStatus, sizeof(sndStatus), "Loaded: %.2f s custom sound.",
                 audio_sample_len() / 16000.0);
    else
        snprintf(sndStatus, sizeof(sndStatus), "No custom sound uploaded yet.");
    const char *spnames[] = {"Chime (two-tone)", "Sonar ping", "Marimba pluck",
                             "Aircraft warning", "Beep (classic)", "Custom sample"};
    String npopts, epopts;
    for (int i = 0; i < AUDIO_PACK_COUNT; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>",
                 i, i == g_packNew ? " selected" : "", spnames[i]);
        npopts += o;
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>",
                 i, i == g_packAlert ? " selected" : "", spnames[i]);
        epopts += o;
    }
    const char *tmnames[] = {"Aircraft (ADS-B)", "Marine vessels (AIS)"};
    // ADS-B source selector: auto prefers the local receiver but survives it going away.
    String srcopts;
    {
        static const char *kSrc[3] = { "Auto (receiver, then internet)",
                                       "Local receiver only",
                                       "Internet feeds only" };
        for (int i = 0; i < 3; ++i) {
            srcopts += "<option value='" + String(i) + "'";
            if (g_adsbSrc == i) srcopts += " selected";
            srcopts += ">" + String(kSrc[i]) + "</option>";
        }
    }
    String tmopts;
    for (int i = 0; i < 2; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>",
                 i, i == g_trafficMode ? " selected" : "", tmnames[i]);
        tmopts += o;
    }
    const char *cfnames[] = {"12-hour (AM/PM)", "24-hour"};
    String cfopts;
    for (int i = 0; i < 2; ++i) {
        char o[72];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>",
                 i, (i == (g_time24 ? 1 : 0)) ? " selected" : "", cfnames[i]);
        cfopts += o;
    }
    const char *msnames[] = {"Dark", "Light"};
    String msopts;
    for (int i = 0; i < 2; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_mapStyle ? " selected" : "", msnames[i]);
        msopts += o;
    }
    const char *unames[] = {"Aviation (ft, kt, nm)", "Metric (m, km/h, km)", "Imperial (ft, mph, mi)"};
    String uopts;
    for (int i = 0; i < 3; ++i) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_units ? " selected" : "", unames[i]);
        uopts += o;
    }
    const char *tlnames[] = {"Off", "Short", "Medium", "Long"};
    String tlopts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_trailLen ? " selected" : "", tlnames[i]);
        tlopts += o;
    }
    // Max aircraft on the scope. The top option must reach ADSB_MAX_AIRCRAFT, or the
    // feed parses contacts the scope can never draw.
    const int mxvals[] = {10, 15, 20, 30, 40, 60, 80, 100, 120};
    String mxopts;
    for (int mv : mxvals) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%d</option>", mv, mv == g_maxAc ? " selected" : "", mv);
        mxopts += o;
    }
    // minimum-altitude filter options (stored in ft; labels show ft + km for clarity)
    const struct { int ft; const char *lbl; } mavals[] = {
        {0, "Off"}, {5000, "&gt; 5,000 ft (1.5 km)"}, {10000, "&gt; 10,000 ft (3 km)"},
        {20000, "&gt; 20,000 ft (6 km)"}, {33000, "&gt; 33,000 ft (10 km)"},
    };
    String maopts;
    for (auto &mv : mavals) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", mv.ft, mv.ft == g_minAltFt ? " selected" : "", mv.lbl);
        maopts += o;
    }
    // Order matches the bitmask: 0 off, 1 new, 2 emergencies, 3 both.
    const char *anames[] = {"Off", "New aircraft only", "Emergencies only",
                            "New aircraft + emergencies"};
    String aopts;
    for (int i = 0; i < 4; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_alertMode ? " selected" : "", anames[i]);
        aopts += o;
    }
    const int proxUnit[] = {0, 2, 5, 10, 25};   // 0 = off; rest in the user's distance unit
    String popts;
    for (int pv : proxUnit) {
        const float pkm = (pv == 0) ? 0.0f : (pv / ufac);   // user unit -> km (value submitted)
        const bool  sel = (pv == 0) ? (g_proximityKm <= 0.0f) : (fabsf(g_proximityKm - pkm) < 0.4f);
        char lbl[24];
        if (pv == 0) snprintf(lbl, sizeof(lbl), "Off");
        else         snprintf(lbl, sizeof(lbl), "%d %s", pv, uname);
        char o[80];
        snprintf(o, sizeof(o), "<option value=%.3f%s>%s</option>", pkm, sel ? " selected" : "", lbl);
        popts += o;
    }
    const char *qnames[] = {"Off", "Dim screen", "Screen off", "Clock screen"};
    String qopts;    // quiet-hours mode
    for (int i = 0; i < 4; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_qhMode ? " selected" : "", qnames[i]);
        qopts += o;
    }
    String tzopts;   // time-zone dropdown (value = index into TZOPTS; mapped to POSIX TZ on save)
    for (int i = 0; i < TZOPTS_N; ++i) {
        char o[128];
        snprintf(o, sizeof(o), "<option value=%d data-off=%d data-dst=%d%s>%s</option>",
                 i, TZOPTS[i].offMin, TZOPTS[i].dst, g_tz == TZOPTS[i].tz ? " selected" : "", TZOPTS[i].label);
        tzopts += o;
    }
    String gpsRow;   // only on the -G variant: offer to auto-set the centre from GPS
    if (gps_present()) {
        gpsRow  = "<label><input type=checkbox class=ck ";
        gpsRow += g_useGps ? "checked" : "";
        gpsRow += " onchange='gp(this.checked)'>Use GPS for location</label>";
        gpsRow += "<div style='font-size:12px;opacity:.6;margin:-2px 0 6px'>"
                  "When on, the location above is used until the GPS gets a fix, then it takes over.</div>";
    }
    // ~8.7 KB of markup plus ~4.5 KB of generated <option> lists; sized with headroom
    // because a truncated page renders as a broken form. It lives in PSRAM, so the
    // slack is free. The check after snprintf reports if this ever stops being enough.
    static const size_t BUFSZ = 26624;   // grows with the page; snprintf would silently truncate
    static char *buf = (char *)ps_malloc(BUFSZ);   // PSRAM: keep this big page buffer off the scarce
    if (!buf) return;                              //   internal heap (the contiguous RAM mbedTLS needs)
    const int needed = snprintf(buf, BUFSZ,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>SkyGlass</title>"
        "<link rel=stylesheet href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);color:#cdd6d1;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:14px;margin-bottom:18px}"
        ".device-frame{padding:4px;border-radius:50%%;width:58px;height:58px;"
        "background:linear-gradient(150deg,#2c302f 0%%,#0c0e0d 100%%);box-shadow:0 6px 18px rgba(0,0,0,0.8);position:relative;flex:0 0 auto}"
        ".screen{position:relative;width:100%%;height:100%%;border-radius:50%%;overflow:hidden;background:#000;box-shadow:inset 0 0 16px rgba(0,0,0,0.95)}"
        ".radar-svg{width:100%%;height:100%%;display:block}"
        ".sweep{transform-origin:233px 233px;animation:spin 6.0s linear infinite}"
        "@keyframes spin{to{transform:rotate(360deg)}}"
        ".ac1{animation:glow-ac1 6.0s linear infinite}.ac2{animation:glow-ac2 6.0s linear infinite}.ac3{animation:glow-ac3 6.0s linear infinite}"
        "@keyframes glow-ac1{0%%,10%%{opacity:.4;filter:none}12%%{opacity:1;filter:drop-shadow(0 0 10px #c8ff3c)}28%%{opacity:.8;filter:drop-shadow(0 0 4px #c8ff3c)}45%%,100%%{opacity:.4;filter:none}}"
        "@keyframes glow-ac2{0%%{opacity:.9;filter:drop-shadow(0 0 8px #3ce0ff)}12%%{opacity:.5;filter:none}30%%,94%%{opacity:.4;filter:none}98%%{opacity:1;filter:drop-shadow(0 0 10px #3ce0ff)}}"
        "@keyframes glow-ac3{0%%,68%%{opacity:.4;filter:none}74%%{opacity:1;filter:drop-shadow(0 0 10px #ffb23c)}88%%{opacity:.7;filter:drop-shadow(0 0 4px #ffb23c)}96%%,100%%{opacity:.4;filter:none}}"
        ".screen::after{content:'';position:absolute;inset:0;pointer-events:none;border-radius:50%%;"
        "background:repeating-linear-gradient(0deg,rgba(0,0,0,0) 0 2px,rgba(0,0,0,0.18) 2px 3px);mix-blend-mode:multiply;opacity:.5}"
        "h1{color:#1dff86;font-size:22px;margin:0;letter-spacing:.5px;text-shadow:0 0 10px rgba(29,255,134,.3)}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "label{display:block;margin:12px 0 4px;color:#9affc8;font-size:13px}"
        "input,select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a4a39;"
        "background:#0c1a12;color:#eafff3;font-size:16px}"
        "input:focus,select:focus{outline:none;border-color:#1dff86;box-shadow:0 0 0 2px rgba(29,255,134,.18)}"
        "button{margin-top:16px;width:100%%;padding:12px;border:0;border-radius:8px;background:#1dff86;"
        "color:#04140b;font-weight:700;font-size:16px}button:active{opacity:.85}"
        ".w{background:#ffb23c}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".ft{color:#5f7a6c;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#9affc8}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".sec{background:#0c1a12!important;color:#1dff86!important;border:1px solid #2a4a39!important}"
        "#map{height:220px;border-radius:10px;margin:6px 0 8px;border:1px solid #2a4a39;z-index:0}"
        "</style></head><body>"
        "<div class=hd><div class=device-frame><div class=screen><svg viewBox='0 0 466 466' class=radar-svg>"
        "<defs>"
        "<filter id=glow x='-60%%' y='-60%%' width='220%%' height='220%%'><feGaussianBlur stdDeviation='2.2' result='b'/><feMerge><feMergeNode in='b'/><feMergeNode in='SourceGraphic'/></feMerge></filter>"
        "<radialGradient id=vign cx='50%%' cy='50%%' r='50%%'><stop offset='68%%' stop-color='#000'/><stop offset='100%%' stop-color='#04140b'/></radialGradient>"
        "<linearGradient id=sweepGrad x1='0' y1='0' x2='1' y2='0'><stop offset='0%%' stop-color='#1dff86' stop-opacity='0'/><stop offset='70%%' stop-color='#1dff86' stop-opacity='0.14'/><stop offset='100%%' stop-color='#1dff86' stop-opacity='0.45'/></linearGradient>"
        "</defs>"
        "<circle cx='233' cy='233' r='233' fill='url(#vign)'/>"
        "<g stroke='#1dff86' fill='none'><circle cx='233' cy='233' r='218' stroke-opacity='0.34'/><circle cx='233' cy='233' r='160' stroke-opacity='0.26'/><circle cx='233' cy='233' r='104' stroke-opacity='0.26'/><circle cx='233' cy='233' r='50' stroke-opacity='0.26'/></g>"
        "<g stroke='#1dff86' stroke-opacity='0.16'><line x1='233' y1='22' x2='233' y2='444'/><line x1='22' y1='233' x2='444' y2='233'/></g>"
        "<g class=sweep><path d='M233,233 L100,55 A220,220 0 0 1 326,33.6 Z' fill='url(#sweepGrad)'/><line x1='233' y1='233' x2='326' y2='33.6' stroke='#3dff9a' stroke-width='2' stroke-opacity='0.85' filter='url(#glow)'/></g>"
        "<g font-family='sans-serif' font-weight='700' text-anchor='middle'><text x='233' y='40' font-size='17' fill='#eafff3'>N</text><text x='233' y='441' font-size='17' fill='#9affc8'>S</text><text x='35' y='239' font-size='17' fill='#9affc8'>W</text><text x='431' y='239' font-size='17' fill='#9affc8'>E</text></g>"
        "<g font-family='monospace'><g class=ac1 transform='translate(322,150)' style='color:#c8ff3c'><path d='M0,0 L26,26' stroke='#c8ff3c' stroke-width='2' stroke-opacity='0.22' stroke-dasharray='2 5'/><path d='M0,-9 L2,-1 L9,3 L2,3 L3,8 L0,6.5 L-3,8 L-2,3 L-9,3 L-2,-1 Z' fill='#c8ff3c' transform='rotate(225)'/></g><g class=ac2 transform='translate(252,74)' style='color:#3ce0ff'><path d='M0,0 L0,-26' stroke='#3ce0ff' stroke-width='2' stroke-opacity='0.22' stroke-dasharray='2 5'/><path d='M0,-9 L2,-1 L9,3 L2,3 L3,8 L0,6.5 L-3,8 L-2,3 L-9,3 L-2,-1 Z' fill='#3ce0ff' transform='rotate(180)'/></g><g class=ac3 transform='translate(118,236)' style='color:#ffb23c'><path d='M0,0 L-26,-4' stroke='#ffb23c' stroke-width='2' stroke-opacity='0.22' stroke-dasharray='2 5'/><path d='M0,-9 L2,-1 L9,3 L2,3 L3,8 L0,6.5 L-3,8 L-2,3 L-9,3 L-2,-1 Z' fill='#ffb23c' transform='rotate(90)'/></g></g>"
        "</svg></div></div><div><h1>SkyGlass</h1><p class=sub>Live ADS-B radar &middot; configuration</p></div></div>"
        "<div class=card><div class=t>Location &amp; range</div><form method=POST action=/save>"
        "<label>Center point &mdash; tap the map or drag the pin</label>"
        "<div id=map></div>"
        "<label>Center latitude</label><input id=lat name=lat value='%.5f'>"
        "<label>Center longitude</label><input id=lon name=lon value='%.5f'>"
        "%s"
        "<label>Display range</label><select name=range>%s</select>"
        "<label>Theme</label><select name=theme>%s</select>"
        "<label>Time zone</label><select name=tz>%s</select>"
        "<button>Save &amp; restart</button></form>"
        "<label>Clock format</label><select onchange='cf(this.value)'>%s</select></div>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input type=range min=5 max=255 value='%d' oninput='b(this.value,0)' onchange='b(this.value,1)'>"
        "<label>Dim screen after</label><select onchange='d(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='sw(this.checked)'>Show radar sweep</label>"
        "<label><input type=checkbox class=ck %s onchange='ti(this.checked)'>Aircraft icons by type</label>"
        "<label><input type=checkbox class=ck %s onchange='ap(this.checked)'>Show airports</label>"
        "<label><input type=checkbox class=ck %s onchange='hg(this.checked)'>Hide aircraft on the ground</label>"
        "<label>Minimum altitude</label><select onchange='ma(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='mo(this.checked)'>Military aircraft only</label>"
        "<label>Aircraft trails</label><select onchange='tl(this.value)'>%s</select>"
        "<label>Max aircraft on screen</label><select onchange='mx(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='bt(this.checked)'>Large text (restarts the device)</label>"
        "<label>Screen rotation (degrees clockwise)</label>"
        "<input type=number min=0 max=359 step=1 value='%d' onchange='ro(this.value)'>"
        "<label>Units</label><select onchange='u(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='mb(this.checked)'>Map background</label>"
        "<label>Map style</label><select onchange='ms(this.value)'>%s</select>"
        "<label>Map visibility (<span id=mop-val>%d%%</span>)</label>"
        "<input type=range min=0 max=100 value='%d' oninput='mop(this.value,0)' onchange='mop(this.value,1)'>"
        "<div style='font-size:12px;opacity:.6;margin-top:4px'>Basemap tiles are redrawn "
        "after a zoom or a move, a few seconds behind the scope.</div></div>"
        "<div class=card><div class=t>Sound</div>"
        "<label>Volume</label>"
        "<input type=range min=0 max=100 value='%d' oninput='v(this.value,0)' onchange='v(this.value,1)'>"
        "<label><input type=checkbox class=ck %s onchange='m(this.checked)'>Mute alerts</label>"
        "<label>Alert on</label><select id=am onchange='al(this.value);av()'>%s</select>"
        "<div id=asoff style='display:none;font-size:13px;color:#ffb23c;margin-top:8px'>"
        "Alerts are off &mdash; nothing will sound on live traffic. The test buttons below "
        "still work, so this is worth checking if alerts seem silent.</div>"
        "<div id=nsnd><label>New aircraft sound</label>"
        "<select onchange='sp(0,this.value)'>%s</select>"
        "<button type=button class=sec onclick='t()'>Test new-aircraft sound</button></div>"
        "<div id=esnd><label>Emergency sound</label>"
        "<select onchange='sp(1,this.value)'>%s</select>"
        "<button type=button class=sec onclick='ta()'>Test emergency sound</button></div>"
        "<label>Proximity alert</label><select onchange='px(this.value)'>%s</select>"
        "<div style='font-size:12px;opacity:.6;margin-top:8px'>Proximity alerts use the "
        "emergency sound. Picking a sound plays it so you can compare.</div>"
        "<div class=t style='margin-top:18px'>Custom sound</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 6px'>%s</p>"
        "<input type=file id=sf accept='.wav,audio/wav'>"
        "<button type=button onclick='su()'>Upload WAV</button>"
        "<div id=sbar style='height:10px;background:#0c1a12;border-radius:5px;overflow:hidden;"
        "margin-top:10px;display:none'><div id=sfill style='height:100%%;width:0;background:#1dff86'></div></div>"
        "<div id=smsg style='margin-top:8px;font-size:13px;color:#9affc8'></div>"
        "<button type=button class=w style='margin-top:10px' onclick='sd()'>Remove custom sound</button>"
        "<div style='font-size:12px;opacity:.65;margin-top:10px;line-height:1.5'>"
        "<b>WAV format</b><br>"
        "&bull; Uncompressed <b>PCM</b>, <b>16-bit</b> (not 24/32-bit or float)<br>"
        "&bull; Mono or stereo &mdash; stereo is mixed down automatically<br>"
        "&bull; Any rate from 8 to 48 kHz &mdash; resampled to 16 kHz automatically<br>"
        "&bull; Keep it under <b>4 seconds</b>; longer clips are trimmed with a fade<br>"
        "&bull; Volume is normalised for you, so quiet recordings still come through<br>"
        "In Audacity: <i>File &rarr; Export &rarr; Export as WAV</i>, encoding "
        "<i>Signed 16-bit PCM</i>. MP3s must be converted to WAV first.</div></div>"
        "<div class=card><div class=t>Traffic</div>"
        "<label>Show on the scope</label><select onchange='ai(this.value)'>%s</select>"
        "<div style='font-size:12px;opacity:.6;margin:4px 0 8px'>Aircraft and ships are "
        "plotted separately &mdash; the scope shows one picture at a time. The list view "
        "follows this setting.</div>"
        "<label>aisstream.io API key (marine)</label>"
        "<input value='%s' placeholder='paste your free aisstream key' onchange='ak(this.value)'>"
        "<div style='font-size:12px;opacity:.6;margin-top:6px'>Free key from "
        "<a href='https://aisstream.io' style='color:#9affc8'>aisstream.io</a>. "
        "Coverage is crowd-sourced, so inland locations may see nothing.</div></div>"
        "<div class=card><div class=t>Local ADS-B receiver</div>"
        "<label>Hostname or IP (blank = use the public feeds)</label>"
        "<input value='%s' placeholder='192.168.1.50 or myreceiver.local' onchange='lf(this.value)'>"
        "<label>Source</label>"
        "<select onchange='ls(this.value)'>%s</select>"
        "<div style='font-size:12px;opacity:.6;margin-top:6px'>Point this at your own "
        "dump1090 / readsb / tar1090 receiver &mdash; a PiAware or ADSB Exchange feeder "
        "works as-is (they are usually reachable as <b>adsbexchange.local</b> or "
        "<b>piaware.local</b>). Your own antenna, no internet required. Leave blank if "
        "you do not run one.</div></div>"
#if BOARD_HAS_SD
        // Status is filled in by JS from /diag rather than printf'd into this page: the
        // page is one large format string and threading two more args through it is a
        // needless chance to desync the placeholders from the values.
        "<div class=card><div class=t>Flight log (microSD)</div>"
        "<div id=sdi style='font-size:14px;margin-bottom:8px'>Checking the card&hellip;</div>"
        "<div style='font-size:12px;opacity:.6;margin-bottom:2px'>The radar remembers every "
        "aircraft it has seen and how many times each one has been over. Tap a contact on "
        "the scope to see its history.</div>"
        "<button onclick=\"if(confirm('Erase the flight log? The sighting history is lost. Nothing else on the card is touched.'))"
        "fetch('/sdlog?erase=1').then(()=>location.reload())\">"
        "Erase flight log</button>"
        "<div style='font-size:12px;opacity:.6;margin-top:4px'>Clears the history and starts "
        "counting again. Everything else on the card is left alone.</div>"
        "<button class=w onclick=\"if(confirm('FORMAT THE CARD? This erases everything on it, not just the flight log, and cannot be undone.'))"
        "fetch('/sdfmt?go=1').then(()=>location.reload())\">"
        "Format card</button>"
        "<div style='font-size:12px;opacity:.6;margin-top:4px'>Wipes the whole card. Only needed "
        "if the radar cannot read it &mdash; it needs <b>FAT32</b>.</div></div>"
#endif
        "<div class=card><div class=t>Quiet hours</div>"
        "<label>Mode</label><select onchange='qm(this.value)'>%s</select>"
        "<label>From</label><input type=time value='%02d:%02d' onchange='qs(this.value)'>"
        "<label>Until</label><input type=time value='%02d:%02d' onchange='qe(this.value)'>"
        "<div style='font-size:12px;opacity:.6;margin-top:6px'>During the window the screen dims, "
        "switches off or shows the clock. A touch wakes it for 15 seconds.</div></div>"
        "<div class=card><div class=t>Firmware</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 8px'>Running <b>v" FW_VERSION "</b></p>"
        "<div id=fwst style='font-size:13px;color:#9affc8;margin-bottom:8px'>checking&hellip;</div>"
        "<button type=button class=sec onclick='fwc()'>Check for updates</button>"
        "<button type=button id=fwgo style='display:none' onclick='fwi()'>Install update</button>"
        "<label><input type=checkbox class=ck %s onchange='fwa(this.checked)'>Check automatically</label>"
        "<label><input type=checkbox class=ck %s onchange='fwai(this.checked)'>Install automatically</label>"
        "<div style='font-size:12px;opacity:.65;margin-top:6px'>Updates come from this "
        "project's GitHub Pages build. Installing keeps your settings and uploaded sound; "
        "the device writes to the spare OTA slot and only switches over once the image "
        "verifies, so a failed download leaves the current firmware running. "
        "Automatic installing is off by default &mdash; a bad published build would "
        "otherwise reach the device with no cable in reach. "
        "<a href=/update style='color:#9affc8'>Upload a .bin manually</a></div></div>"
        "<div class=card><div class=t>Network</div>"
#if !BOARD_HAS_WIFIMANAGER
        // Boards without WiFiManager have no captive portal, so credential entry has to
        // live here or there is no way in at all -- which is exactly what happened when
        // the portal was compiled out for the P4.
        "<p style='color:#9affc8;font-size:13px;margin:0 0 4px'>Join a WiFi network. The device reboots to connect.</p>"
        "<form method=POST action=/wifisave>"
        "<label>SSID</label><input name=ssid maxlength=32 autocomplete=off>"
        "<label>Password</label><input name=pass type=password maxlength=63 autocomplete=off>"
        "<button class=w>Save &amp; connect</button></form>"
#endif
        "<p style='color:#9affc8;font-size:13px;margin:8px 0 4px'>Forget the saved WiFi and reopen the setup portal.</p>"
        "<form method=POST action=/wifi><button class=w>Reset WiFi</button></form></div>"
        "<p class=ft>Reach me at <code>" BOARD_HOSTNAME ".local</code> &middot; <a href=/update style='color:#9affc8'>Firmware update</a> &middot; v" FW_VERSION "</p>"
        "<script>"
        "var C=[%.5f,%.5f];var MAP=L.map('map').setView(C,10);"
        "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'(c) OpenStreetMap'}).addTo(MAP);"
        "var MK=L.marker(C,{draggable:true}).addTo(MAP);"
        "function S(p){document.getElementById('lat').value=p.lat.toFixed(5);document.getElementById('lon').value=p.lng.toFixed(5);}"
        "MK.on('dragend',function(){S(MK.getLatLng());});"
        "MAP.on('click',function(e){MK.setLatLng(e.latlng);S(e.latlng);});"
        "setTimeout(function(){MAP.invalidateSize();},300);"
        "function b(v,s){fetch('/bright?v='+v+(s?'&save=1':''))}"
        "function v(x,s){fetch('/vol?v='+x+(s?'&save=1':''))}"
        "function m(c){fetch('/vol?mute='+(c?1:0)+'&save=1')}"
        "function t(){fetch('/vol?test=1')}"
        "function ta(){fetch('/vol?test=3')}"
        "function su(){var f=document.getElementById('sf').files[0];"
        "var m=document.getElementById('smsg');"
        "if(!f){m.innerText='Choose a .wav file first';return}"
        "var x=new XMLHttpRequest(),d=new FormData();d.append('f',f);"
        "document.getElementById('sbar').style.display='block';m.innerText='Uploading...';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)"
        "document.getElementById('sfill').style.width=(e.loaded/e.total*100)+'%%'};"
        "x.onload=function(){m.innerText=(x.status==200)"
        "?'Saved - playing it now. Alert sound set to Custom sample.':('Rejected: '+x.responseText)};"
        "x.onerror=function(){m.innerText='Upload failed'};"
        "x.open('POST','/sound');x.send(d);}"
        "function sd(){fetch('/sounddel',{method:'POST'}).then(function(){"
        "document.getElementById('smsg').innerText='Custom sound removed.'})}"
        "function d(v){fetch('/idle?v='+v+'&save=1')}"
        "function sw(c){fetch('/sweep?v='+(c?1:0)+'&save=1')}"
        "function ap(c){fetch('/airports?v='+(c?1:0)+'&save=1')}"
        "function hg(c){fetch('/ground?v='+(c?1:0)+'&save=1')}"
        "function ma(v){fetch('/altmin?v='+v+'&save=1')}"
        "function mo(c){fetch('/milonly?v='+(c?1:0)+'&save=1')}"
        "function tl(v){fetch('/trail?v='+v+'&save=1')}"
        "function mx(v){fetch('/maxac?v='+v+'&save=1')}"
        "function bt(c){fetch('/bigtext?v='+(c?1:0)+'&save=1')}"
        "function ro(v){fetch('/rotate?v='+v+'&save=1')}"
        "function u(v){fetch('/units?v='+v+'&save=1')}"
        "function al(v){fetch('/alerts?mode='+v+'&save=1')}"
        "function px(v){fetch('/alerts?prox='+v+'&save=1')}"
        "function gp(c){fetch('/gps?v='+(c?1:0)+'&save=1')}"
        // Firmware card. One endpoint returns the whole state, so check/install/toggle
        // and the periodic refresh all share a single render path.
        "function fwr(j){document.getElementById('fwst').innerText=j.status;"
        "document.getElementById('fwgo').style.display=j.avail?'block':'none';}"
        "function fwq(q){fetch('/fwupd'+q).then(function(r){return r.json()}).then(fwr)}"
        "function fwc(){document.getElementById('fwst').innerText='checking...';fwq('?check=1')}"
        "function fwi(){if(!confirm('Install the update and reboot?'))return;"
        "document.getElementById('fwst').innerText='installing - do not power off';fwq('?install=1');"
        "setTimeout(function(){document.getElementById('fwst').innerText="
        "'installing... the page will stop responding, then reconnect'},1500)}"
        "function fwa(c){fwq('?auto='+(c?1:0))}"
        "function fwai(c){fwq('?autoinst='+(c?1:0))}"
        "setInterval(function(){fwq('')},5000);fwq('');"
        "function sp(c,v){fetch('/vol?cue='+c+'&pack='+v+'&save=1')}"
        // Only show the sound picker for a cue that is actually enabled.
        "function av(){var m=+document.getElementById('am').value;"
        "document.getElementById('nsnd').style.display=(m&1)?'block':'none';"
        "document.getElementById('esnd').style.display=(m&2)?'block':'none';"
        "document.getElementById('asoff').style.display=m?'none':'block';}"
        "function cf(v){fetch('/clockfmt?v='+v+'&save=1')}"
        "function ti(c){fetch('/typeicons?v='+(c?1:0)+'&save=1')}"
        "function ai(v){fetch('/ais?v='+v+'&save=1')}"
        "function ak(v){fetch('/ais?key='+encodeURIComponent(v)+'&save=1')}"
        "function lf(v){fetch('/adsblocal?h='+encodeURIComponent(v))}"
        "function ls(v){fetch('/adsblocal?m='+v)}"
        "function mb(c){fetch('/map?v='+(c?1:0)+'&save=1')}"
#if BOARD_HAS_SD
        // Fill the flight-log card's status line from /diag rather than printf-ing it
        // into the page: this page is one big format string and the fewer placeholders
        // it carries, the fewer ways it can desync from its argument list.
        "fetch('/diag').then(r=>r.json()).then(d=>{var e=document.getElementById('sdi');"
        "if(!e)return;var s=d.sd||'',p=s.split(' ');"
        "if(p.length>3&&p[3]=='MB'){var gb=Math.round(p[2]/1024);"
        "e.innerHTML='<b style=color:#1dff86>Card ready</b> &mdash; '+gb+' GB, '+d.sd_recs+"
        "' aircraft remembered<div style=font-size:11px;opacity:.45;margin-top:2px>connected '"
        "+p[0]+' at '+p[1]+'</div>';}"
        "else{e.innerHTML='<b style=color:#ffb23c>No card</b> &mdash; insert a FAT32 card, '"
        "+'then restart the radar<div style=font-size:11px;opacity:.45;margin-top:2px>'+s+'</div>';}"
        "}).catch(()=>{});"
#endif
        "function ms(v){fetch('/map?style='+v+'&save=1')}"
        "function mop(v,s){document.getElementById('mop-val').innerText=v+'%%';fetch('/mapopa?v='+v+(s?'&save=1':''))}"
        "function qm(v){fetch('/quiet?mode='+v+'&save=1')}"
        "function qs(v){fetch('/quiet?start='+encodeURIComponent(v)+'&save=1')}"
        "function qe(v){fetch('/quiet?end='+encodeURIComponent(v)+'&save=1')}"
        // auto-pick the visitor's time zone from their browser clock (only if they haven't set one)
        "var TZSET=%d;(function(){if(TZSET)return;"
        "var d=new Date(),j=new Date(d.getFullYear(),0,1).getTimezoneOffset(),"
        "u=new Date(d.getFullYear(),6,1).getTimezoneOffset(),o=-Math.max(j,u),s=(j!=u)?1:0,"
        "e=document.querySelector('select[name=tz]'),b=-1,i;"
        "for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o&&+e.options[i].dataset.dst===s){b=i;break;}}"
        "if(b<0)for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o){b=i;break;}}"
        "if(b>=0)e.selectedIndex=b;})();"
        "av();"   // apply the show/hide rule to the state the page loaded with
        "</script></body></html>",
        g_settings.homeLat, g_settings.homeLon, gpsRow.c_str(), ropts.c_str(), topts.c_str(),
        tzopts.c_str(), cfopts.c_str(),
        g_brightnessDay, iopts.c_str(), g_showSweep ? "checked" : "",
        g_typeIcons ? "checked" : "",
        g_showAirports ? "checked" : "", g_hideGround ? "checked" : "", maopts.c_str(), g_milOnly ? "checked" : "",
        // order must track the HTML above: trails, max aircraft, large text, rotation,
        // units, map, sound, traffic, quiet hours
        tlopts.c_str(), mxopts.c_str(), g_bigText ? "checked" : "", g_rotation, uopts.c_str(),
        g_mapBg ? "checked" : "", msopts.c_str(), g_mapOpa, g_mapOpa,
        g_volume, g_muted ? "checked" : "", aopts.c_str(),
        npopts.c_str(), epopts.c_str(), popts.c_str(),
        sndStatus,
        tmopts.c_str(), g_aisKey.c_str(),
        g_adsbLocal.c_str(), srcopts.c_str(),
        qopts.c_str(), g_qhStart / 60, g_qhStart % 60, g_qhEnd / 60, g_qhEnd % 60,
        updater_auto_check() ? "checked" : "", updater_auto_install() ? "checked" : "",
        g_settings.homeLat, g_settings.homeLon, (g_tz == TZ_STR ? 0 : 1));
    // A truncated page renders as a half-broken form rather than failing loudly, so say
    // so on serial — it means BUFSZ needs raising after adding controls.
    if (needed >= (int)BUFSZ)
        Serial.printf("[web] config page truncated: needed %d bytes, buffer is %u\n",
                      needed, (unsigned)BUFSZ);
    g_web.send(200, "text/html", buf);
}

static void handleSave() {
    Preferences p;
    p.begin("capsuleradar", false);
    // Reject out-of-range coordinates so a typo can't leave the radar unusable.
    if (g_web.hasArg("lat")) {
        const double lat = g_web.arg("lat").toDouble();
        if (lat >= -90.0 && lat <= 90.0) p.putDouble("homeLat", lat);
    }
    if (g_web.hasArg("lon")) {
        const double lon = g_web.arg("lon").toDouble();
        if (lon >= -180.0 && lon <= 180.0) p.putDouble("homeLon", lon);
    }
    if (g_web.hasArg("range")) p.putFloat("rangeKm", g_web.arg("range").toFloat());
    if (g_web.hasArg("theme")) p.putInt("theme", g_web.arg("theme").toInt());
    if (g_web.hasArg("tz")) {
        const int i = g_web.arg("tz").toInt();
        if (i >= 0 && i < TZOPTS_N) p.putString("tz", TZOPTS[i].tz);
    }
    p.end();
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='4;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Restarting&hellip;</body>");
    delay(400);
    ESP.restart();
}

#if !BOARD_HAS_WIFIMANAGER
// Store credentials and reboot. Kept behind the same guard as the form: boards with
// WiFiManager get its portal instead and never reach this.
static void handleWifiSave() {
    const String ss = g_web.arg("ssid");
    const String pw = g_web.arg("pass");
    if (ss.length()) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putString("wifiSsid", ss);
        p.putString("wifiPass", pw);
        p.end();
        Serial.printf("[wifi] saved credentials for %s; rebooting\n", ss.c_str());
    }
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='6;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Reconnecting&hellip;<br><br>"
        "If this page does not come back, rejoin your normal WiFi and open "
        "<code>" BOARD_HOSTNAME ".local</code>.</body>");
    g_rebootAtMs = millis() + 1200;
}
#endif

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>" BOARD_SETUP_AP "</b> network to reconfigure.</body>");
    delay(400);                     // let the response reach the browser
    // The driver stores the saved AP in its own NVS namespace ("nvs.net80211"). On Arduino
    // core 3.x both wm.resetSettings() and WiFi.disconnect(true,true) can silently no-op
    // (they fail once the driver is off), so v1.3.19's reset still reconnected. Erasing that
    // namespace directly is unconditional — it works whatever state the WiFi driver is in.
#if BOARD_HAS_WIFIMANAGER
    g_wm.resetSettings();           // best-effort driver-level erase first...
#endif
    WiFi.disconnect(false, true);   // ...keep WiFi up so the erase can actually run
    delay(100);
    nvs_handle_t h;                 // ...then the guaranteed path: wipe the driver's namespace
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    delay(300);                     // let NVS finish committing before the reboot
    ESP.restart();
}

static void handleBright() {
    if (g_web.hasArg("v")) {
        g_brightnessDay = constrain((int)g_web.arg("v").toInt(), 0, 255);
        applyBrightness();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("bright", g_brightnessDay);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleVol() {
    if (g_web.hasArg("v"))    { g_volume = constrain((int)g_web.arg("v").toInt(), 0, 100); audio_set_volume(g_volume); }
    if (g_web.hasArg("mute")) { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); }
    if (g_web.hasArg("pack")) {
        // cue=1 selects the emergency sound, anything else the new-contact sound.
        const bool forAlert = g_web.hasArg("cue") && g_web.arg("cue").toInt() == 1;
        const int pack = constrain((int)g_web.arg("pack").toInt(), 0, AUDIO_PACK_COUNT - 1);
        if (forAlert) { g_packAlert = pack; audio_set_pack_for(AUDIO_ALERT, pack); }
        else          { g_packNew   = pack; audio_set_pack_for(AUDIO_NEW,   pack); }
        audio_play(forAlert ? AUDIO_ALERT : AUDIO_NEW);   // audition the cue being edited
    }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("vol", g_volume);
        p.putBool("mute", g_muted);
        p.putInt("packnew", g_packNew);
        p.putInt("packalert", g_packAlert);
        p.end();
    }
    if (g_web.hasArg("test")) {
        const int t = g_web.arg("test").toInt();
        if (t == 2)      audio_selftest();          // long tone, ignores mute
        else if (t == 3) audio_play(AUDIO_ALERT);   // emergency / military cue
        else             audio_play(AUDIO_NEW);     // new-contact cue
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAlerts() {   // what triggers the alert sound (live)
    if (g_web.hasArg("mode")) g_alertMode   = constrain((int)g_web.arg("mode").toInt(), 0, 3);
    if (g_web.hasArg("prox")) {
        g_proximityKm = g_web.arg("prox").toFloat();   // km (0 = off)
        g_requeryKm = queryRadiusKm();                 // the query must cover the new alert circle
        g_requery = true;
    }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("alertmode2", g_alertMode);
        p.putFloat("proxkm", g_proximityKm);
        p.end();
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleIdle() {   // idle auto-dim timeout (seconds; 0 = never)
    if (g_web.hasArg("v")) {
        const long s = g_web.arg("v").toInt();
        g_idleDimMs = (s <= 0) ? 0 : (uint32_t)s * 1000;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putUInt("idledim", g_idleDimMs);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleUnits() {   // measurement units preset (live re-render)
    if (g_web.hasArg("v")) {
        g_units = constrain((int)g_web.arg("v").toInt(), 0, 2);
        ui_set_units(g_units);
        ui_set_range_km(g_settings.rangeKm);   // refresh the zoom-button label
        ui_on_data_updated();                  // re-render card/list/stats in the new units
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("units", g_units);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleSweep() {   // show/hide the rotating sweep line (live)
    if (g_web.hasArg("v")) {
        g_showSweep = g_web.arg("v").toInt() != 0;
        radar::setSweepEnabled(g_showSweep);          // loop()/core 1: safe to touch LVGL
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("sweep", g_showSweep);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTrail() {   // aircraft trail length 0/1/2/3 (live)
    if (g_web.hasArg("v")) {
        g_trailLen = constrain((int)g_web.arg("v").toInt(), 0, 3);
        radar::setTrailLength(g_trailLen);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("traillen", g_trailLen);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAltMin() {   // minimum-altitude feed filter, ft (applies from the next poll)
    if (g_web.hasArg("v")) {
        g_minAltFt = constrain((int)g_web.arg("v").toInt(), 0, 60000);
        g_adsb.setMinAltFt((float)g_minAltFt);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("minalt", g_minAltFt);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMilOnly() {   // military-only feed filter (applies from the next poll)
    if (g_web.hasArg("v")) {
        g_milOnly = g_web.arg("v").toInt() != 0;
        g_adsb.setMilitaryOnly(g_milOnly);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("milonly", g_milOnly);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleBigText() {   // accessibility: large fonts. Fonts are baked at UI creation,
    if (g_web.hasArg("v")) {    // so persist the flag and reboot cleanly to apply it.
        g_bigText = g_web.arg("v").toInt() != 0;
        Preferences p;
        p.begin("capsuleradar", false);
        p.putBool("bigtext", g_bigText);
        p.end();
        g_rebootAtMs = millis() + 1200;   // let this response reach the browser first
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMaxAc() {   // max aircraft drawn on the scope (live)
    if (g_web.hasArg("v")) {
        g_maxAc = constrain((int)g_web.arg("v").toInt(), 1, ADSB_MAX_AIRCRAFT);
        radar::setMaxOnScreen(g_maxAc);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("maxac", g_maxAc);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAirports() {   // show/hide airport markers (live)
    if (g_web.hasArg("v")) {
        g_showAirports = g_web.arg("v").toInt() != 0;
        radar::setAirportsEnabled(g_showAirports);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("airports", g_showAirports);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleClockFmt() {   // 12/24-hour clock (live: HUD, clock face, WX stamps)
    if (g_web.hasArg("v")) {
        g_time24 = g_web.arg("v").toInt() != 0;
        ui_set_time_24h(g_time24);
        ui_on_data_updated();          // repaint anything already showing a timestamp
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("time24", g_time24);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTypeIcons() {   // aircraft silhouettes: by type or one generic glyph
    if (g_web.hasArg("v")) {
        g_typeIcons = g_web.arg("v").toInt() != 0;
        radar::setTypeIcons(g_typeIcons);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("typeicons", g_typeIcons);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAis() {   // traffic mode (aircraft vs marine) + aisstream.io key
    if (g_web.hasArg("v")) {
        g_trafficMode = constrain((int)g_web.arg("v").toInt(), 0, 1);
        radar::setTrafficMode(g_trafficMode);
        ui_on_data_updated();          // HUD count + list follow the mode immediately
    }
    if (g_web.hasArg("key")) {
        g_aisKey = g_web.arg("key");
        g_aisKey.trim();
        ais_set_key(g_aisKey.c_str());     // empty key disconnects and clears the contacts
    }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("traffic", g_trafficMode);
        p.putString("aiskey", g_aisKey);
        p.end();
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAdsbLocal() {   // point the feed at a receiver on the local network
    if (g_web.hasArg("m")) {
        g_adsbSrc = constrain((int)g_web.arg("m").toInt(), 0, 2);
        g_adsb.setSourceMode(g_adsbSrc);
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("adsbsrc", g_adsbSrc);
        p.end();
    }
    if (g_web.hasArg("h")) {
        g_adsbLocal = g_web.arg("h");
        g_adsbLocal.trim();
        g_adsb.setLocalHost(g_adsbLocal.c_str());   // empty falls straight back to the public feeds
        Preferences p;
        p.begin("capsuleradar", false);
        p.putString("adsblocal", g_adsbLocal);
        p.end();
        Serial.printf("[adsb] local receiver set to '%s'\n", g_adsbLocal.c_str());
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMap() {   // map-tile background: on/off + style
    if (g_web.hasArg("v")) {
        g_mapBg = g_web.arg("v").toInt() != 0;
        if (g_mapBg) map_bg_begin();       // allocated on first use, not at boot
        map_client_enable(g_mapBg);
        if (!g_mapBg) radar::update(g_snap, g_settings);   // hide it right away
    }
    if (g_web.hasArg("style")) {
        g_mapStyle = constrain((int)g_web.arg("style").toInt(), 0, 1);
        map_client_set_style(g_mapStyle);
    }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putBool("mapbg", g_mapBg);
        p.putInt("mapstyle", g_mapStyle);
        p.putInt("mapopa", g_mapOpa);
        p.end();
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMapOpa() {
    if (g_web.hasArg("v")) {
        g_mapOpa = constrain((int)g_web.arg("v").toInt(), 0, 100);
        radar::setMapOpacity(g_mapOpa);   // instant, and never touches the tiles
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("mapopa", g_mapOpa);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGround() {   // hide/show on-ground aircraft (applies from the next feed poll)
    if (g_web.hasArg("v")) {
        g_hideGround = g_web.arg("v").toInt() != 0;
        g_adsb.setHideGround(g_hideGround);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("hideground", g_hideGround);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleRotate() {   // arbitrary clockwise display rotation, applied live
    if (g_web.hasArg("v")) {
        g_rotation = constrain((int)g_web.arg("v").toInt(), 0, 359);
        display::setRotation((uint16_t)g_rotation);
        g_rotation = display::rotation();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("rotDeg", g_rotation);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGps() {   // auto-set the centre point from the LC76G GPS (-G variant)
    if (g_web.hasArg("v")) {
        g_useGps = g_web.arg("v").toInt() != 0;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("usegps", g_useGps);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

// Parse an <input type=time> value ("HH:MM") into minutes since midnight; -1 if invalid.
static int parseHhMm(const String &s) {
    const int c = s.indexOf(':');
    if (c < 1) return -1;
    const int h = s.substring(0, c).toInt();
    const int m = s.substring(c + 1).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

static void handleQuiet() {   // quiet hours: mode + window (applies live)
    Preferences p;
    bool save = g_web.hasArg("save");
    if (save) p.begin("capsuleradar", false);
    if (g_web.hasArg("mode")) {
        g_qhMode = constrain((int)g_web.arg("mode").toInt(), 0, 3);
        if (save) p.putInt("qhmode", g_qhMode);
    }
    if (g_web.hasArg("start")) {
        const int v = parseHhMm(g_web.arg("start"));
        if (v >= 0) { g_qhStart = v; if (save) p.putInt("qhstart", v); }
    }
    if (g_web.hasArg("end")) {
        const int v = parseHhMm(g_web.arg("end"));
        if (v >= 0) { g_qhEnd = v; if (save) p.putInt("qhend", v); }
    }
    if (save) p.end();
    // re-evaluate immediately so a change takes effect without waiting for the IMU tick
    const bool quiet = quietWindowNow();
    if (quiet != g_inQuiet) {
        g_inQuiet = quiet;
        if (g_qhMode == 3) ui_show_view(quiet ? 5 : 0);
    }
    applyBrightness();
    g_web.send(200, "text/plain", "ok");
}

// ---- screenshots: GET /shot.bmp, and GET /view?i=N to pick a screen first ----
// Serves the live framebuffer as a 24-bit BMP so it opens anywhere. Rows are emitted
// bottom-up and padded to 4 bytes, per the format. Useful for documentation shots:
// photographing a glossy round AMOLED never looks as good as the real pixels.
static void handleShot() {
    const uint16_t *fb = display::captureFrame();
    if (!fb) { g_web.send(503, "text/plain", "no framebuffer"); return; }

    const int W = SCREEN_W, H = SCREEN_H;
    const int rowBytes = W * 3;
    const int pad = (4 - (rowBytes % 4)) % 4;
    const uint32_t imgSize = (uint32_t)(rowBytes + pad) * H;
    const uint32_t fileSize = 54 + imgSize;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    const uint32_t off = 54;   memcpy(hdr + 10, &off, 4);
    const uint32_t dib = 40;   memcpy(hdr + 14, &dib, 4);
    const int32_t w32 = W, h32 = H;
    memcpy(hdr + 18, &w32, 4); memcpy(hdr + 22, &h32, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &imgSize, 4);

    g_web.setContentLength(fileSize);
    g_web.send(200, "image/bmp", "");
    g_web.sendContent((const char *)hdr, sizeof(hdr));

    // One row at a time: 466*3 bytes is a comfortable chunk, and the whole image would
    // be 650 KB if buffered. RGB565 -> BGR888, expanding the top bits into the low ones
    // so full-scale stays full-scale.
    static uint8_t row[SCREEN_W * 3 + 4];
    for (int y = H - 1; y >= 0; --y) {
        const uint16_t *src = fb + (size_t)y * W;
        uint8_t *o = row;
        for (int x = 0; x < W; ++x) {
            const uint16_t p = src[x];
            const uint8_t r = (uint8_t)((p >> 11) & 0x1F), g = (uint8_t)((p >> 5) & 0x3F), b = (uint8_t)(p & 0x1F);
            *o++ = (uint8_t)((b << 3) | (b >> 2));
            *o++ = (uint8_t)((g << 2) | (g >> 4));
            *o++ = (uint8_t)((r << 3) | (r >> 2));
        }
        for (int i = 0; i < pad; ++i) *o++ = 0;
        g_web.sendContent((const char *)row, rowBytes + pad);
    }
    g_web.sendContent("", 0);
}

static void handleView() {   // pick a screen (0 radar, 1 list, 2 stats, 3 weather, 4 tracked, 5 clock, 6 about)
    // 0..6: ui_show_view() ignores anything above 6, so accepting 7 here only made
    // /view?i=7 look like it worked. The old 7 was about, back when 6 was the fires
    // screen; that screen is gone and about took its place.
    if (g_web.hasArg("i")) ui_show_view(constrain((int)g_web.arg("i").toInt(), 0, 6));
    if (g_web.hasArg("wx")) ui_set_weather_mode(constrain((int)g_web.arg("wx").toInt(), 0, 2));
    if (g_web.hasArg("icon")) ui_preview_weather_icon(g_web.arg("icon").toInt());
    if (g_web.hasArg("mil")) radar::setMilitaryPreview(g_web.arg("mil").toInt() != 0);
    // Select the Nth nearest contact (-1 clears), so the detail card can be captured
    // without someone standing at the device tapping the screen.
    if (g_web.hasArg("sel")) {
        radar::select(g_web.arg("sel").toInt());
        ui_on_data_updated();
    }
    // Track the currently selected contact (1) or clear tracking (0). TRACK is otherwise
    // a button on the detail card, which left the Tracked view uncapturable remotely.
    if (g_web.hasArg("trk")) ui_track_selected(g_web.arg("trk").toInt() != 0);
    // Re-show the boot splash on demand. It normally appears for six seconds before WiFi
    // is up, so it could never be captured with /shot.bmp -- the one screen that had to
    // be photographed by hand.
    if (g_web.hasArg("splash")) ui_splash_show();
    if (g_web.hasArg("selhex")) {          // stable across polls, unlike the index
        const bool hit = radar::selectByHex(g_web.arg("selhex").c_str());
        ui_on_data_updated();
        g_web.send(200, "text/plain", hit ? "ok" : "not in range");
        return;
    }
    g_web.send(200, "text/plain", "ok");
}

// ---- custom alert sound: upload a WAV, converted on the fly to 16 kHz mono PCM ----
static bool g_soundUploadOk = false;

static void handleSoundUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[wav] upload start: %s\n", up.filename.c_str());
        wav_upload_begin();
        g_soundUploadOk = false;
    } else if (up.status == UPLOAD_FILE_WRITE) {
        wav_upload_data(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        g_soundUploadOk = wav_upload_end();
        if (g_soundUploadOk) {
            // A freshly uploaded sound is what you want to hear, and an emergency is the
            // cue people upload for; leave the routine new-contact ping alone.
            g_packAlert = AUDIO_PACK_CUSTOM;
            audio_set_pack_for(AUDIO_ALERT, g_packAlert);
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("packalert", g_packAlert);
            p.end();
        }
    }
}

static void handleSoundDelete() {
    audio_clear_sample();
    if (!audio_has_sample()) {                    // don't leave "Custom" selected with nothing behind it
        Preferences p;
        p.begin("capsuleradar", false);
        if (g_packNew == AUDIO_PACK_CUSTOM) {
            g_packNew = AUDIO_PACK_CHIME;
            audio_set_pack_for(AUDIO_NEW, g_packNew);
            p.putInt("packnew", g_packNew);
        }
        if (g_packAlert == AUDIO_PACK_CUSTOM) {
            g_packAlert = AUDIO_PACK_WARN;
            audio_set_pack_for(AUDIO_ALERT, g_packAlert);
            p.putInt("packalert", g_packAlert);
        }
        p.end();
    }
    g_web.send(200, "text/plain", "removed");
}

// ---- self-update: check / install straight from the settings page ----
static void handleFwUpd() {
    if (g_web.hasArg("check"))   updater_request_check();
    if (g_web.hasArg("install")) updater_request_install();
    if (g_web.hasArg("auto"))    updater_set_auto_check(g_web.arg("auto").toInt() != 0);
    if (g_web.hasArg("autoinst")) updater_set_auto_install(g_web.arg("autoinst").toInt() != 0);

    // Always answer with the current state so the page can poll this one endpoint.
    char latest[16], status[64];
    bool avail = false;
    updater_state(latest, sizeof(latest), status, sizeof(status), &avail);
    char json[192];
    snprintf(json, sizeof(json),
             "{\"cur\":\"%s\",\"latest\":\"%s\",\"status\":\"%s\",\"avail\":%s}",
             FW_VERSION, latest, status, avail ? "true" : "false");
    g_web.send(200, "application/json", json);
}

// ---- browser OTA: upload an app .bin over WiFi and self-flash ----
static void handleUpdatePage() {
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>SkyGlass - Update</title><style>"
        "body{background:radial-gradient(circle at 50% -10%,#0a1f15,#04100a 70%);color:#cdd6d1;"
        "font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        "h1{color:#1dff86;font-size:20px}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px}"
        "input,button{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;margin-top:8px;font-size:16px}"
        "input{background:#0c1a12;color:#eafff3;border:1px solid #2a4a39}"
        "button{border:0;background:#1dff86;color:#04140b;font-weight:700}"
        "#bar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        "#fill{height:100%;width:0;background:#1dff86;transition:width .2s}#msg{margin-top:10px;color:#9affc8;font-size:13px}"
        "a{color:#1dff86}p{color:#9affc8;font-size:13px}"
        "</style></head><body><h1>Firmware update (OTA)</h1><div class=card>"
        "<p>Upload the <b>app firmware</b> <code>SkyGlass-ota.bin</code> from the GitHub release. "
        "Do NOT use the merged flash image here.</p>"
        "<input type=file id=f accept='.bin'>"
        "<button onclick=u()>Update over WiFi</button>"
        "<div id=bar><div id=fill></div></div><div id=msg></div></div>"
        "<p style='text-align:center;margin-top:14px'><a href=/>&larr; Back to settings</a></p>"
        "<script>function u(){var f=document.getElementById('f').files[0];if(!f){return}"
        "var x=new XMLHttpRequest(),fd=new FormData();fd.append('f',f);"
        "document.getElementById('bar').style.display='block';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(e.loaded/e.total*100)+'%'};"
        "x.onload=function(){document.getElementById('msg').innerText=x.responseText+' - rebooting...'};"
        "x.onerror=function(){document.getElementById('msg').innerText='Upload failed'};"
        "x.open('POST','/update');x.send(fd);}</script></body></html>");
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nSkyGlass boot");

#if BOARD_PANEL_QSPI
    if (PIN_LCD_SCLK < 0 || PIN_I2C_SDA < 0) {
#else
    if (PIN_I2C_SDA < 0) {
#endif
        Serial.println("[!] Pins in config.h are still -1. Copy them from the Waveshare demo.");
    }
    Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());

    // Mount before loadSettings(): the default sound pack depends on whether a custom
    // sound exists, and an uploaded one only becomes visible once the filesystem is up.
    if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS mount failed (uploads unavailable)");
    else audio_load_sample();   // restore a previously uploaded alert sound
    // SD is probed from loop() a few seconds in, not here. Nothing depends on it, and
    // doing it during setup() meant a fault in the card path stopped the board before
    // the display, WiFi or OTA existed -- unrecoverable without a cable.

    loadSettings();
    updater_begin();       // restore the auto-check / auto-install preferences
    route_cache_begin();   // clear stale route cache if the label format changed

    // --- Display + LVGL (M0) ----------------------------------------------
    // CO5300 AMOLED over QSPI + LVGL draw buffers in PSRAM, then a hello screen.
    // The panel is powered from the always-on DC1 rail, so it lights without the
    // PMIC. Touch (CST9217 indev) + AXP2101 come in later milestones.
    if (!display::begin()) {
        Serial.println("[!] display::begin() failed — check QSPI pins / power.");
    }

    // restore the saved theme, then persist any future change
    {
        Preferences p;
        p.begin("capsuleradar", true);
        const int t = p.getInt("theme", THEME_PHOSPHOR);
        g_showSweep = p.getBool("sweep", true);
        g_showAirports = p.getBool("airports", true);
        g_hideGround = p.getBool("hideground", false);
        g_minAltFt = p.getInt("minalt", 0);
        g_milOnly = p.getBool("milonly", false);
        // Migrate the old quarter-turn setting (rot=0..3) without changing existing
        // installations' orientation. New firmware stores actual degrees separately.
        g_rotation = p.isKey("rotDeg") ? p.getInt("rotDeg", 0) : p.getInt("rot", 0) * 90;
        g_rotation = constrain(g_rotation, 0, 359);
        p.end();
        radar::setTheme(t);
        ui_theme_changed();          // ...and once at boot for the restored theme
        radar::setSweepEnabled(g_showSweep);
        radar::setAirportsEnabled(g_showAirports);
        radar::setTrafficMode(g_trafficMode);
        radar::setTypeIcons(g_typeIcons);
        radar::setMapOpacity(g_mapOpa);   // the saved value never reached the LVGL layer,
                                          // so visibility only took effect once the
                                          // slider was touched
        g_adsb.setHideGround(g_hideGround);
        g_adsb.setMinAltFt((float)g_minAltFt);
        g_adsb.setMilitaryOnly(g_milOnly);
        radar::setTrailLength(g_trailLen);
        radar::setMaxOnScreen(g_maxAc);
        display::setRotation((uint16_t)g_rotation);
        g_rotation = display::rotation();
    }
    radar::setThemeChangedCb(saveTheme);
    ui_set_range_cb(onRangeChange);              // on-screen zoom button + pinch commit
    ui_set_range_preview_cb(onRangePreview);     // live pinch-zoom preview
    ui_set_units(g_units);                       // apply saved unit preset
    ui_set_range_km(g_settings.rangeKm);         // show the loaded range

    imu_begin();       // face-down sleep (no-op if the IMU isn't detected)
    battery_begin();   // AXP2101 (no-op if not detected / no battery)
    gps_begin();       // LC76G GNSS (no-op if not the -G variant)
    battery_enable_codec_rail();   // power the ES8311 analog rail before audio init

    setenv("TZ", TZ_STR, 1); tzset();   // local time for display even before NTP
    rtc_begin();
    rtc_seed_clock();                   // offline clock/date from the PCF85063
    if (audio_begin()) {                // ES8311 alert pings (no-op if codec absent)
        audio_set_volume(g_volume);
        audio_set_muted(g_muted);
        audio_set_pack_for(AUDIO_NEW,   g_packNew);
        audio_set_pack_for(AUDIO_ALERT, g_packAlert);
    }

    // --- Radar UI ----------------------------------------------------------
    // radar::init() runs inside display::begin() (LVGL must be up first).

    // --- WiFi (captive portal, non-blocking) ------------------------------
    // First boot opens the "SkyGlass-Setup" AP to enter WiFi creds. Non-blocking
    // so the radar keeps animating while you configure WiFi from your phone.
#if BOARD_HAS_WIFIMANAGER
    g_wm.setConfigPortalBlocking(false);
    g_wm.setTitle("SkyGlass");
    // light phosphor-green theme for the captive portal (small CSS, injected into <head>)
    g_wm.setCustomHeadElement(
        "<style>"
        "body{background:#06100a;color:#cdd6d1;font-family:system-ui,sans-serif}"
        "h1,h2,h3{color:#1dff86}"
        "button,input[type=submit],.btn{background:#1dff86!important;color:#04140b!important;"
        "border:0!important;border-radius:8px!important;font-weight:700}"
        "input,select{background:#0c1a12!important;color:#eafff3!important;"
        "border:1px solid #2a4a39!important;border-radius:8px!important}"
        "a{color:#1dff86}.q{filter:hue-rotate(90deg)}"
        "</style>");
    // After the portal saves new credentials, reboot for a clean start: WiFiManager's
    // own port-80 server (and mDNS) don't cleanly hand over to our web server / STA
    // interface in non-blocking mode, so the config page is flaky until a fresh boot.
    g_wm.setSaveConfigCallback([]() {
        Serial.println("[wifi] new credentials saved -> rebooting for a clean web/mDNS start");
        g_rebootAtMs = millis() + 2500;   // let the portal deliver its 'saved' page first
    });
    if (g_wm.autoConnect(BOARD_SETUP_AP))
        Serial.println("[wifi] connected");
    else
        Serial.println("[wifi] config portal open - join 'SkyGlass-Setup' to set WiFi; UI stays live");
#else
    // No WiFiManager on this board. Connect with whatever the config page stored; if
    // nothing is stored yet, raise a SoftAP so that page is reachable to enter it.
    {
        Preferences wp; wp.begin("capsuleradar", true);
        const String ss = wp.getString("wifiSsid", ""), pw = wp.getString("wifiPass", "");
        wp.end();
        if (ss.length()) {
            WiFi.mode(WIFI_STA);
            WiFi.begin(ss.c_str(), pw.c_str());
            Serial.printf("[wifi] connecting to %s\n", ss.c_str());
        } else {
            // Check the result. On this board the radio is an ESP32-C6 over esp_hosted,
            // and AP mode is not a given -- if softAP() fails there is simply no network
            // to join and the setup page is unreachable, which looks identical to the
            // page being broken.
            const bool apOk = WiFi.mode(WIFI_AP) && WiFi.softAP(BOARD_SETUP_AP);
            if (apOk) {
                Serial.printf("[wifi] SoftAP up: join '" BOARD_SETUP_AP "', open http://%s/\n",
                              WiFi.softAPIP().toString().c_str());
            } else {
                Serial.println("[wifi] SoftAP FAILED - no setup network. Set credentials over "
                               "serial instead: send  wifi <ssid> <password>");
            }
        }
    }
#endif

    // --- OTA ---------------------------------------------------------------
    // ArduinoOTA is started from loop() once WiFi connects (see otaUp there).

    // --- ADS-B client + task ----------------------------------------------
    float queryKm = queryRadiusKm();
    g_adsb.begin(g_settings.homeLat, g_settings.homeLon, queryKm);
    wx_radar_begin();
    cloud_image_begin();
    // Only claim the map buffers if the basemap is actually on. They are two full-screen
    // RGB565 images (~868 KB of PSRAM); allocating them for a feature that ships disabled
    // reserved a tenth of the whole PSRAM for nothing. map_bg_begin() is idempotent, so
    // handleMap() calls it again when the user switches the basemap on later.
    if (g_mapBg) map_bg_begin();
    g_ac_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(adsb_task, "adsb", 16384, nullptr, 1, nullptr, 0);  // TLS needs a big stack

    // configuration web page (http://<board hostname>.local/)
    g_web.on("/", handleRoot);
    g_web.on("/save", HTTP_POST, handleSave);
    g_web.on("/wifi", HTTP_POST, handleWifi);
#if !BOARD_HAS_WIFIMANAGER
    g_web.on("/wifisave", HTTP_POST, handleWifiSave);
#endif
    g_web.on("/bright", handleBright);
    g_web.on("/vol", handleVol);
    g_web.on("/alerts", handleAlerts);
    g_web.on("/idle", handleIdle);
    g_web.on("/sweep", handleSweep);
    g_web.on("/airports", handleAirports);
    g_web.on("/ground", handleGround);
    g_web.on("/altmin", handleAltMin);
    g_web.on("/milonly", handleMilOnly);
    g_web.on("/trail", handleTrail);
    g_web.on("/maxac", handleMaxAc);
    g_web.on("/bigtext", handleBigText);
    g_web.on("/rotate", handleRotate);
    g_web.on("/gps", handleGps);
    g_web.on("/quiet", handleQuiet);
    g_web.on("/map", handleMap);
    g_web.on("/mapopa", handleMapOpa);
    g_web.on("/ais", handleAis);
    g_web.on("/adsblocal", handleAdsbLocal);
#if BOARD_HAS_SD
    g_web.on("/sdretry", []() {       // re-arm after a probe crash and try again
        sd_clear_crash_flag();
        g_web.send(200, "text/plain", sd_begin() ? sd_status() : sd_status());
    });
#endif
#if BOARD_HAS_SD
    g_web.on("/sdlog", []() {                 // erase the sighting history only
        const bool ok = g_web.hasArg("erase") && sd_seen_erase();
        g_web.send(200, "text/plain", ok ? "erased" : "no card");
    });
    g_web.on("/sdfmt", []() {                 // reformat the card; destroys everything
        const bool ok = g_web.hasArg("go") && sd_format();
        g_web.send(200, "text/plain", ok ? "formatted" : "failed");
    });
#endif
    g_web.on("/sound", HTTP_POST,
        []() {
            if (g_soundUploadOk) {
                g_web.send(200, "text/plain", "ok");
                audio_play(AUDIO_ALERT);          // play it back so the result is audible
            } else {
                g_web.send(400, "text/plain", wav_upload_error());
            }
        },
        handleSoundUpload);
    g_web.on("/sounddel", HTTP_POST, handleSoundDelete);
    // Health snapshot. Exists because the interesting numbers (contiguous internal heap,
    // PSRAM headroom) previously only reached serial, which needs a cable attached.
    g_web.on("/diag", []() {
        float sfps = 0, savg = 0, smax = 0; uint32_t sdraw = 0;
        radar::sweepPerf(&sfps, &sdraw, &savg, &smax);
        uint32_t sdHit = 0, sdApp = 0, sdRdE = 0;
sd_counters(&sdHit, &sdApp, &sdRdE);
        uint32_t lblUs = 0; uint16_t lblMoves = 0, lblSeen = 0;
        radar::labelPerf(&lblUs, &lblMoves, &lblSeen);
        lv_mem_monitor_t lvmem;
        lv_mem_monitor(&lvmem);      // LVGL pool headroom: exhausting it hangs the UI core
                                     // outright -- LVGL's assert handler is a bare while(1)
        char j[880];
        snprintf(j, sizeof(j),
                 "{\"fw\":\"%s\",\"uptime_s\":%lu,\"heap\":%u,\"heap_min\":%u,"
                 "\"heap_largest\":%u,\"psram\":%u,\"aircraft\":%d,\"max_on_screen\":%d,"
                 "\"feed_cap\":%d,\"lv_free\":%u,\"lv_pct\":%u,"
                 "\"lv_biggest\":%u,\"lv_frag\":%u,"
                 "\"lbl_us\":%u,\"lbl_moves\":%u,\"lbl_seen\":%u,"
                 "\"sd\":\"%s\",\"sd_recs\":%u,\"sd_hit\":%u,\"sd_app\":%u,\"sd_rderr\":%u,\"photo\":\"%s\","
                 "\"fps\":%.1f,\"draw_us\":%u,\"step_avg\":%.2f,\"step_max\":%.2f,\"frame_ms\":%u,"
                 "\"lvgl_ms\":%.1f,\"rest_ms\":%.1f}",
                 FW_VERSION, (unsigned long)(millis() / 1000),
                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)ESP.getFreePsram(), (int)g_snap.size(), g_maxAc,
                 ADSB_MAX_AIRCRAFT, (unsigned)lvmem.free_size, (unsigned)lvmem.used_pct,
                 // free_size alone does not predict an allocation failure: LVGL asks for one
                 // contiguous block, so the biggest free block and the fragmentation are
                 // what decide it. (lvmem.max_used is deliberately not reported -- LVGL
                 // only adjusts its running total in lv_mem_alloc and lv_mem_free, never
                 // in lv_mem_realloc, so it reads about half the real figure.)
                 (unsigned)lvmem.free_biggest_size, (unsigned)lvmem.frag_pct,
                 (unsigned)lblUs, (unsigned)lblMoves, (unsigned)lblSeen,
                 sd_status(), (unsigned)sd_seen_records(),
                 (unsigned)sdHit, (unsigned)sdApp, (unsigned)sdRdE,
                 photo_note_get(), sfps, sdraw, savg, smax, (unsigned)radar::sweepFrameMs(), g_loopLvglMs, g_loopRestMs);
        g_web.send(200, "application/json", j);
    });
    g_web.on("/fwupd", handleFwUpd);
    g_web.on("/shot.bmp", handleShot);
    // Numbered sweep variants, switchable live. Two rounds of tuning by measurement made
    // the motion worse, so the useful loop is: flip between presets, judge by eye.
    g_web.on("/view", handleView);
    g_web.on("/clockfmt", handleClockFmt);
    g_web.on("/typeicons", handleTypeIcons);
    g_web.on("/units", handleUnits);
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) ESP.restart();
        },
        handleUpdateUpload);
    g_web.begin();

    Serial.println("setup done");
}

// Serial credential entry. Exists because the P4 has no captive portal and its SoftAP
// may not come up at all; without this there would be no way to get the board onto a
// network. Format:  wifi <ssid> <password>
static void pollSerialConfig() {
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!line.startsWith("wifi ")) return;
    const int sp = line.indexOf(' ', 5);
    const String ss = (sp > 0) ? line.substring(5, sp) : line.substring(5);
    const String pw = (sp > 0) ? line.substring(sp + 1) : String();
    if (!ss.length()) return;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putString("wifiSsid", ss);
    p.putString("wifiPass", pw);
    p.end();
    Serial.printf("[wifi] stored SSID '%s'; rebooting\n", ss.c_str());
    delay(300);
    ESP.restart();
}

void loop() {
    const uint32_t tLoop0 = micros();
    pollSerialConfig();
    const uint32_t tLv0 = micros();
    display::loop();
    const uint32_t tLv1 = micros();                // drive LVGL (render dirty areas + run timers)
#if BOARD_HAS_WIFIMANAGER
    g_wm.process();                 // service the WiFi config portal (non-blocking)
#endif
    g_web.handleClient();           // serve the configuration web page
    if (g_useGps) gps_poll();       // pull NMEA from the LC76G (only when GPS auto-location is on)

    // scheduled reboot after a fresh WiFi config (see setSaveConfigCallback)
#if BOARD_HAS_SD
    // Probe the card once, twenty seconds in. Three guard rails, each earned: it is not
    // in setup(), so a fault cannot stop the board before the display, WiFi and OTA
    // exist; the mount never touches the shared SDMMC host beyond slot 0, which is what
    // was killing the C6 link; and sd_begin() raises an NVS flag before probing that only
    // clears afterwards, so a probe that does not return leaves the next boot skipping
    // the card entirely. Turned back on because the mount has since run repeatedly
    // without disturbing the radio, and a flight log that never starts is no log at all.
    static bool sdProbed = false;
    if (!sdProbed && millis() > 20000UL) { sdProbed = true; sd_begin(); }
#endif
    if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) { delay(50); ESP.restart(); }

    // OTA: set up once WiFi is up, then service it every loop (flash over the air)
    static bool otaUp = false;
    if (!otaUp && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname(BOARD_HOSTNAME);    // -> <hostname>.local (registers mDNS)
        ArduinoOTA.begin();
        MDNS.addService("http", "tcp", 80);            // advertise the config web page
        otaUp = true;
        Serial.println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
    }
    if (otaUp) ArduinoOTA.handle();

    // Push a fresh ADS-B snapshot to the radar (copy under the mutex, render outside).
    if (g_acDirty) {
        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_snap.swap(g_aircraft);   // O(1) handoff under the lock; render on g_snap outside it.
            g_acDirty = false;         // g_aircraft now holds the previous snapshot (overwritten next poll)
            xSemaphoreGive(g_ac_mutex);
            radar::update(g_snap, g_settings); // rebuild the glyph/trail layer
            ui_on_data_updated();              // refresh card/list/stats
            checkAudioEvents();                // ping new-in-range / emergency / military
        }
    }
    if (g_weatherDirty) {
        g_weatherDirty = false;
        ui_on_data_updated();
    }
    if (g_wxRadarDirty) {
        g_wxRadarDirty = false;
        ui_on_data_updated();
    }
    if (g_cloudImageDirty) {
        g_cloudImageDirty = false;
        ui_on_data_updated();
    }
    if (g_mapDirty) {
        g_mapDirty = false;
        radar::update(g_snap, g_settings);   // bind a newly downloaded basemap image
        ui_on_data_updated();
    }

    // periodic: HUD clock + wifi/battery indicators
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
#if DEBUG_MEM
        static uint32_t lastFrames = 0;
        const uint32_t fr = display_frames();
        const unsigned fps = (fr - lastFrames) / 5;
        lastFrames = fr;
        Serial.printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | aircraft %d | fps %u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
                      (int)g_snap.size(), fps);
#endif
        char clk[12] = "--:--";
        struct tm ti;
        const bool haveTime = getLocalTime(&ti, 0);
        if (haveTime) {
            // No AM/PM in the HUD: the strip is narrow and the date sits right below it.
            ui_format_clock(clk, sizeof(clk), ti.tm_hour, ti.tm_min, false);
            char date[20];
            strftime(date, sizeof(date), "%d %b %Y", &ti);   // e.g. "08 Jun 2026"
            ui_set_date(date);
        }
        const bool wifiUp = (WiFi.status() == WL_CONNECTED);
        const int  rssi   = wifiUp ? (int)WiFi.RSSI() : -127;
        // "fresh" = we got aircraft data recently. Catches a stalled feed (weak WiFi dropping
        // polls intermittently) that never trips the consecutive-fail counter -> aircraft
        // freeze but the icon would otherwise stay white.
        const bool feedFresh = wifiUp && (millis() - g_lastFeedOkMs < 18000UL);
        ui_set_status(wifiUp, feedFresh, rssi, clk);
#if !BOARD_HAS_WIFIMANAGER
        wifi_portal_tick();
#endif
        char net[112];
        if (WiFi.status() == WL_CONNECTED)
            // IP + the active centre point (helps users verify what actually got saved)
            snprintf(net, sizeof(net), "Configure at\n" BOARD_HOSTNAME ".local\n%s  |  %.5f, %.5f",
                     WiFi.localIP().toString().c_str(), g_settings.homeLat, g_settings.homeLon);
#if !BOARD_HAS_WIFIMANAGER
        // Name the network that actually exists. This used to read "join SkyGlass-Setup"
        // on both boards, and neither of them broadcasts that -- the S3 raises
        // SkyGlass-S3-Setup and the P4 SkyGlass-P4-Setup. Following the on-screen
        // instruction found nothing.
        else if (g_setupApUp)
            snprintf(net, sizeof(net), "WiFi setup:\njoin " BOARD_SETUP_AP "\nthen open http://%s/",
                     WiFi.softAPIP().toString().c_str());
        else if (g_setupApFail)
            snprintf(net, sizeof(net), "No WiFi.\nSetup AP unavailable -\nuse USB serial");
        else
            snprintf(net, sizeof(net), "Connecting...\nsetup network in %lus",
                     (unsigned long)((WIFI_PORTAL_AFTER_MS -
                                      LV_MIN(millis() - g_staWaitMs, WIFI_PORTAL_AFTER_MS)) / 1000));
#else
        else
            snprintf(net, sizeof(net), "WiFi setup:\njoin " BOARD_SETUP_AP);
#endif
        ui_set_netinfo(net);
        const bool bpresent = battery_present();
        ui_set_battery(battery_percent(), battery_charging(), bpresent);
        g_onBattery = bpresent && !battery_charging();
        // GPS HUD/Stats: 0 = off/no module (hidden), 1 = acquiring, 2 = fix
        const int gpsState = (!g_useGps || !gps_present()) ? 0 : (gps_has_fix() ? 2 : 1);
        ui_set_gps(gpsState, gps_satellites());
        // once NTP has a real fix, persist it to the RTC (core 1 only)
        if (!g_rtcSynced && time(nullptr) > 1700000000L) {
            time_t now = time(nullptr);
            struct tm utc;
            gmtime_r(&now, &utc);
            if (rtc_write(&utc)) { g_rtcSynced = true; Serial.println("[rtc] saved NTP time"); }
        }
        // GPS auto-location (-G variant): re-centre the radar when the fix moves enough.
        if (g_useGps) {
            double glat, glon;
            if (gps_location(&glat, &glon) &&
                geo::haversineKm(g_settings.homeLat, g_settings.homeLon, glat, glon) > 1.0) {
                g_settings.homeLat = glat; g_settings.homeLon = glon;   // radar/coastline recenter
                // re-query the new area — set the radius too (same formula as boot/zoom), or
                // adsb_task would re-begin with a stale/zero g_requeryKm and fetch 0 aircraft.
                g_requeryKm = queryRadiusKm();
                g_requery = true;                                       // adsb_task re-queries the new area
                Serial.printf("[gps] re-centred to %.4f, %.4f\n", glat, glon);
            }
        }
    }

    // face-down -> screen off (IMU); flip face-up to wake
    static uint32_t lastImu = 0;
    static int fdCount = 0;
    if (millis() - lastImu > 400) {
        lastImu = millis();
        const int fd = imu_facedown();              // 1 down, 0 not, -1 read error
        if (fd > 0)       { if (fdCount < 8) fdCount++; }
        else if (fd == 0) fdCount = 0;              // -1 (I2C hiccup): leave the counter as-is
        const bool sleep = (fdCount >= 4);   // ~1.6 s face-down
        const bool idle  = g_idleDimMs > 0 && display::inactiveMs() > g_idleDimMs;
        // quiet hours: on entry optionally force the clock screen; on exit return to radar
        const bool quiet = quietWindowNow();
        if (quiet != g_inQuiet) {
            g_inQuiet = quiet;
            if (g_qhMode == 3) ui_show_view(quiet ? 5 : 0);
            applyBrightness();
        }
        if (sleep != g_asleep || idle != g_idle) {
            g_asleep = sleep;
            g_idle = idle;
            applyBrightness();
        }
        // while in quiet hours the touch-wake grace period expires on its own -> re-apply
        static bool wasAwakeGrace = false;
        const bool awakeGrace = g_inQuiet && display::inactiveMs() < 15000;
        if (awakeGrace != wasAwakeGrace) { wasAwakeGrace = awakeGrace; applyBrightness(); }
    }

    delay(5);
    // Split the loop into "LVGL" and "everything else" so the bottleneck is a measurement
    // rather than a guess.
    {
        const uint32_t now = micros();
        g_loopLvglUs += (tLv1 - tLv0);
        g_loopRestUs += (now - tLoop0) - (tLv1 - tLv0);
        if (++g_loopCount >= 60) {
            g_loopLvglMs = g_loopLvglUs / g_loopCount / 1000.0f;
            g_loopRestMs = g_loopRestUs / g_loopCount / 1000.0f;
            g_loopLvglUs = g_loopRestUs = g_loopCount = 0;
        }
    }

}
