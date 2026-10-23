// aisstream.io live AIS feed.
//
// One persistent WSS connection, subscribed to a bounding box around the scope. The
// socket is pumped from the same core-0 network task as the ADS-B poll — arduinoWebSockets
// is non-blocking, so ais_loop() returns immediately and never delays the aircraft feed.
//
// Only PositionReport and ShipStaticData are requested: position reports give movement,
// static reports give the ship's name. Anything else would just burn heap on this device.
#include "ais_client.h"
#include "vessel.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

// On the P4 the Serial log never reaches the USB port; mirror [ais] lines through the ROM
// printf (same as ADSB_LOG). The ROM printf has no float formatting -- keep these to
// %d / %u / %s.
#if defined(BOARD_WAVESHARE_P4_LCD_4C)
#  include <esp_rom_sys.h>
#  define AIS_LOG(...) do { Serial.printf(__VA_ARGS__); esp_rom_printf(__VA_ARGS__); } while (0)
#else
#  define AIS_LOG(...) Serial.printf(__VA_ARGS__)
#endif

// Diagnostics: how much the stream is actually delivering.
static uint32_t s_msgs = 0;         // JSON frames (text or binary) received since connecting
static uint32_t s_frag = 0;         // fragmented frames (not reassembled; counted to spot them)
static uint32_t s_other = 0;        // pings/pongs and anything else
static uint32_t s_used = 0;         // ...of which carried a usable position
static uint32_t s_lastStatus = 0;
static uint32_t s_lastDiscLog = 0;

#define AIS_HOST "stream.aisstream.io"
#define AIS_PORT 443
#define AIS_PATH "/v0/stream"

static WebSocketsClient s_ws;
static char   s_key[48] = "";
static bool   s_started = false;
static bool   s_connected = false;
static bool   s_needSub = false;
static double s_lat = 0, s_lon = 0;
static float  s_rangeKm = 0;
static uint32_t s_lastExpire = 0;

// Parse JSON out of PSRAM so the internal heap stays free for the TLS sessions.
struct AisPsramAlloc : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static AisPsramAlloc s_alloc;

static void send_subscription(void) {
    if (!s_connected || !s_key[0]) return;
    // aisstream wants [[[swLat, swLon], [neLat, neLon]]].
    const double dLat = (double)s_rangeKm / 111.0;
    const double cosLat = cos(s_lat * M_PI / 180.0);
    const double dLon = dLat / (cosLat < 0.15 ? 0.15 : cosLat);
    char msg[320];
    snprintf(msg, sizeof(msg),
             "{\"APIKey\":\"%s\",\"BoundingBoxes\":[[[%.4f,%.4f],[%.4f,%.4f]]],"
             "\"FilterMessageTypes\":[\"PositionReport\",\"ShipStaticData\"]}",
             s_key, s_lat - dLat, s_lon - dLon, s_lat + dLat, s_lon + dLon);
    s_needSub = false;
    const bool sent = s_ws.sendTXT(msg);
    if (sent)
        AIS_LOG("[ais] subscribed: box +/-%d km around home, key %u chars\n",
                (int)s_rangeKm, (unsigned)strlen(s_key));
    else
        AIS_LOG("[ais] subscription NOT sent (socket closed while sending)\n");
}

static void handle_message(const uint8_t *payload, size_t len) {
    ++s_msgs;
    // aisstream answers a bad key or a malformed subscription with {"error": "..."} and
    // then closes the socket. That used to be dropped silently -- say what it was.
    if (len > 0 && len < 400 && memmem(payload, len, "\"error\"", 7)) {
        char txt[160];
        const size_t n = len < sizeof(txt) - 1 ? len : sizeof(txt) - 1;
        memcpy(txt, payload, n); txt[n] = 0;
        AIS_LOG("[ais] server says: %s\n", txt);
        return;
    }

    JsonDocument filter(&s_alloc);
    filter["MessageType"] = true;
    filter["MetaData"]["MMSI"] = true;
    filter["MetaData"]["ShipName"] = true;
    filter["MetaData"]["latitude"] = true;
    filter["MetaData"]["longitude"] = true;
    filter["Message"]["PositionReport"]["Sog"] = true;
    filter["Message"]["PositionReport"]["Cog"] = true;

    JsonDocument doc(&s_alloc);
    if (deserializeJson(doc, payload, len, DeserializationOption::Filter(filter))) return;

    JsonObjectConst meta = doc["MetaData"].as<JsonObjectConst>();
    if (meta.isNull()) return;
    const uint32_t mmsi = meta["MMSI"] | 0u;
    if (!mmsi) return;

    Vessel v;
    v.mmsi = mmsi;
    v.lat = meta["latitude"] | 0.0;
    v.lon = meta["longitude"] | 0.0;
    if (v.lat == 0.0 && v.lon == 0.0) return;
    v.sogKt = doc["Message"]["PositionReport"]["Sog"] | NAN;
    v.cogDeg = doc["Message"]["PositionReport"]["Cog"] | NAN;
    if (v.sogKt > 102.0f) v.sogKt = NAN;        // 102.3 is the AIS "not available" code
    if (v.cogDeg > 360.0f) v.cogDeg = NAN;
    snprintf(v.name, sizeof(v.name), "%s", (const char *)(meta["ShipName"] | ""));
    // AIS pads names with trailing spaces / @ fill characters.
    for (int i = (int)strlen(v.name) - 1; i >= 0; --i) {
        if (v.name[i] == ' ' || v.name[i] == '@') v.name[i] = 0;
        else break;
    }
    v.updatedMs = millis();
    vessel_upsert(v);
    ++s_used;
}

// Library payloads are not NUL-terminated; copy into something printable.
static const char *payload_text(const uint8_t *payload, size_t len, char *buf, size_t cap) {
    if (!payload || !len) return "no reason given";
    const size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(buf, payload, n); buf[n] = 0;
    return buf;
}

static void ws_event(WStype_t type, uint8_t *payload, size_t len) {
    char why[96];
    switch (type) {
        case WStype_CONNECTED:
            s_connected = true;
            s_msgs = s_used = s_frag = s_other = 0;
            AIS_LOG("[ais] connected to %s\n", AIS_HOST);
            // Subscribe from ais_loop(), not from inside the library's callback: sending
            // while the library is still finishing the handshake is fragile, and aisstream
            // allows 3 s for the subscription, so the next pump is soon enough.
            s_needSub = true;
            break;
        case WStype_DISCONNECTED:
            // Log every drop, including connection attempts that never got through (TLS
            // or DNS failures used to be completely silent). Throttled: the library retries
            // every 15 s on its own.
            if (s_connected || millis() - s_lastDiscLog > 14000UL) {
                s_lastDiscLog = millis();
                AIS_LOG("[ais] %s (%s); retrying in 15 s\n",
                        s_connected ? "disconnected" : "could not connect",
                        payload_text(payload, len, why, sizeof(why)));
            }
            s_connected = false;
            break;
        case WStype_TEXT:
        case WStype_BIN:
            // aisstream sends its JSON in BINARY frames. Only TEXT was handled, so every
            // message was dropped uncounted -- connected and subscribed, yet "0 messages".
            handle_message(payload, len);
            break;
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
            ++s_frag;
            break;
        case WStype_ERROR:
            AIS_LOG("[ais] socket error (%s)\n", payload_text(payload, len, why, sizeof(why)));
            break;
        default:
            ++s_other;
            break;
    }
}

void ais_set_key(const char *apiKey) {
    char next[sizeof(s_key)];
    snprintf(next, sizeof(next), "%s", apiKey ? apiKey : "");
    if (strcmp(next, s_key) == 0) return;
    memcpy(s_key, next, sizeof(s_key));
    if (!s_key[0]) { ais_stop(); return; }
    if (s_started) { ais_stop(); }              // reconnect with the new credentials
}

bool ais_has_key(void) { return s_key[0] != 0; }
bool ais_connected(void) { return s_connected; }

void ais_stop(void) {
    if (!s_started) return;
    s_ws.disconnect();
    s_started = false;
    s_connected = false;
    vessel_clear();
}

void ais_configure(double lat, double lon, float rangeKm) {
    // Re-subscribe only on a meaningful move/zoom; the box is generous, so small drifts
    // do not need a new subscription.
    if (fabs(lat - s_lat) < 0.05 && fabs(lon - s_lon) < 0.05 &&
        fabsf(rangeKm - s_rangeKm) < 5.0f) return;
    s_lat = lat; s_lon = lon; s_rangeKm = rangeKm;
    s_needSub = true;
}

void ais_loop(void) {
    if (!s_key[0]) return;
    if (WiFi.status() != WL_CONNECTED) return;

    if (!s_started) {
        s_ws.beginSSL(AIS_HOST, AIS_PORT, AIS_PATH);
        s_ws.onEvent(ws_event);
        s_ws.setReconnectInterval(15000);
        s_started = true;
        AIS_LOG("[ais] connecting to %s (key %u chars)\n", AIS_HOST, (unsigned)strlen(s_key));
    }
    s_ws.loop();
    if (s_needSub && s_connected) send_subscription();   // after connect, or on a move/zoom

    const uint32_t now = millis();
    if (now - s_lastStatus > 30000UL) {   // heartbeat: is anything arriving at all?
        s_lastStatus = now;
        AIS_LOG("[ais] %s, %u messages (%u with a position), %d ships tracked"
                " [fragments %u, other %u]\n",
                s_connected ? "connected" : "NOT connected",
                (unsigned)s_msgs, (unsigned)s_used, vessel_count(),
                (unsigned)s_frag, (unsigned)s_other);
    }
    if (now - s_lastExpire > 60000UL) {   // prune stale contacts once a minute
        s_lastExpire = now;
        vessel_expire(now);
    }
}
