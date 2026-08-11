// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept, and hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar
// then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include "geo.h"           // haversineKm — keep the nearest N aircraft
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

// NetworkClient::readBytes() treats a transient negative TLS read as end-of-input,
// which makes ArduinoJson intermittently report IncompleteInput. Deliberately wrap
// the client without overriding readBytes(): Stream's timed byte reader retries
// temporary no-data reads until the configured timeout.
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return _source.available(); }
    int read() override {
        const int value = _source.read();
        if (value >= 0) ++_bytesRead;
        return value;
    }
    int peek() override { return _source.peek(); }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    Stream& _source;
    size_t _bytesRead = 0;
};

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    // Counts consecutive polls where the primary answered normally but with no aircraft.
    // Only ever touched from adsb_task, so a plain static is fine.
    static uint32_t s_emptyStreak = 0;

    // Your own receiver wins whenever it is configured and answering: no internet round
    // trip, sub-second data, and it shows whatever your antenna hears.
    if (_localHost.length() && _srcMode != 2) {
        std::vector<Aircraft> localList;
        const bool ok = fetchFrom(_localHost.c_str(), localList, true);
        if (ok && !localList.empty()) {
            s_emptyStreak = 0;
            _lastWasLocal = true;
            out.swap(localList);
            return true;
        }
        // Local-only is a deliberate choice, so honour it even when the answer is "nothing".
        // An empty sky over your own antenna is a real result; quietly topping it up from
        // the APIs would make the receiver look better than it is.
        if (_srcMode == 1) {
            _lastWasLocal = ok;
            if (ok) { out.swap(localList); return true; }
            return false;
        }
    }
    _lastWasLocal = false;
    if (_srcMode == 1) return false;          // local-only with no host configured

    std::vector<Aircraft> primaryList;
    bool pOk = fetchFrom(ADSB_PRIMARY_HOST, primaryList);

    if (pOk && !primaryList.empty()) {
        s_emptyStreak = 0;
        out.swap(primaryList);
        return true;
    }

    // Decide whether to spend a second HTTPS round-trip on the fallback feed.
    //
    // A primary *error* always earns one. A primary that answered cleanly with zero
    // aircraft usually means the sky really is empty -- and at POLL_INTERVAL_MS that
    // state can hold all night, so probing every poll would sustain double the request
    // rate against two free non-commercial APIs. Probe on the first empty (to catch a
    // feed that has gone silently quiet), then back off.
    //
    // The streak resets whenever the fallback *does* return aircraft, which collapses
    // the backoff to every-poll for exactly the case that justifies it: a primary that
    // is broken while the fallback still has traffic.
    bool tryFallback = true;
    if (pOk) {
        tryFallback = (s_emptyStreak % ADSB_EMPTY_RECHECK_POLLS) == 0;
        ++s_emptyStreak;
    }

    std::vector<Aircraft> fallbackList;
    bool fOk = false;
    if (tryFallback) {
        fOk = fetchFrom(ADSB_FALLBACK_HOST, fallbackList);
        if (fOk && !fallbackList.empty()) {
            s_emptyStreak = 0;
            out.swap(fallbackList);
            return true;
        }
    }

    if (pOk) { out.swap(primaryList); return true; }
    if (fOk) { out.swap(fallbackList); return true; }
    return false;
}

bool AdsbClient::fetchFrom(const char* host, std::vector<Aircraft>& out, bool local) {
    char url[192];
    if (local) {
        // tar1090/readsb serve the whole picture the receiver hears -- there is no radius
        // query, so the nearest-N cull during parse does that job instead. Plain HTTP: it
        // is your own LAN, and TLS to a Pi would buy a handshake every poll for nothing.
        snprintf(url, sizeof(url), "http://%s%s", host, ADSB_LOCAL_PATH);
    } else {
        const double nm = ceil((double)_rangeKm * 0.539957);        // km -> nautical miles (rounded up)
        snprintf(url, sizeof(url), "https://%s/v2/point/%.4f/%.4f/%.0f", host, _lat, _lon, nm > 1.0 ? nm : 1.0);
    }

    bool hostChanged = (_lastHost == nullptr || strcmp(_lastHost, host) != 0);
    if (hostChanged) {
        _http.end();
        if (!local) _client.setInsecure();
        _http.setReuse(true);
        _http.setConnectTimeout(local ? 2500 : 6000);   // a box on the LAN answers fast or not at all
        _http.setTimeout(local ? 4000 : 8000);
        _lastHost = host;
    }

    if (!_http.begin(local ? (WiFiClient &)_plain : (WiFiClient &)_client, url)) {
        Serial.printf("[adsb] begin failed (%s)\n", host);
        if (local) _plain.stop(); else _client.stop();
        return false;
    }
    _http.addHeader("User-Agent", ADSB_USER_AGENT);
    _http.addHeader("Accept", "application/json");

    const int code = _http.GET();
    if (code != 200) {
        char tls[128] = "";
        const int tlsCode = _client.lastError(tls, sizeof(tls));
        Serial.printf("[adsb] HTTP %d (%s) tls=%d '%s' heap=%u largest=%u psram=%u\n",
                      code, host, tlsCode, tls,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        _http.end();
        _client.stop();
        return false;
    }

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro", "alt_geom",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    const int expectedBytes = _http.getSize();
    NetworkClient& responseStream = _http.getStream();
    ReliableJsonStream jsonStream(responseStream);
    DeserializationError err = deserializeJson(doc, jsonStream,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[adsb] JSON parse failed (%s): %s; expected=%d read=%u available=%d connected=%d\n",
                      host, err.c_str(), expectedBytes, (unsigned)jsonStream.bytesRead(),
                      responseStream.available(), responseStream.connected());
        _http.end();
        _client.stop();
        return false;
    }
    _http.end();
    _client.stop();

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

    // Keep the ADSB_MAX_AIRCRAFT *nearest* aircraft
    std::vector<Aircraft> tmp;
    std::vector<float>     dist;             // parallel array: km from home for each kept aircraft
    tmp.reserve(ADSB_MAX_AIRCRAFT);
    dist.reserve(ADSB_MAX_AIRCRAFT);
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if (a["lat"].isNull() || a["lon"].isNull()) continue;   // need a position
        const double lat = a["lat"].as<double>();
        const double lon = a["lon"].as<double>();

        // Robust altitude parsing: preference alt_baro, fallback to alt_geom
        bool onGround = false;
        float altFt = 0.0f;
        if (a["alt_baro"].is<const char*>()) {
            const char* s = a["alt_baro"].as<const char*>();
            if (s && (strcasecmp(s, "ground") == 0)) onGround = true;
        }
        if (!onGround) {
            if (!a["alt_baro"].isNull())     altFt = a["alt_baro"].as<float>();
            else if (!a["alt_geom"].isNull()) altFt = a["alt_geom"].as<float>();
        }

        if (_hideGround && onGround) continue;
        // optional filters (applied before the cap, so slots only go to matching aircraft)
        if (_minAltFt > 0.0f && (onGround || altFt < _minAltFt)) continue;
        if (_milOnly && (((a["dbFlags"] | 0u) & 0x1) == 0)) continue;

        const float d = (float)geo::haversineKm(_lat, _lon, lat, lon);

        // nearest-N gate: if the buffer is full and this one isn't closer than the farthest kept,
        // drop it now — before any string allocation.
        int farIdx = -1;
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) {
            farIdx = 0;
            for (int i = 1; i < (int)dist.size(); ++i) if (dist[i] > dist[farIdx]) farIdx = i;
            if (d >= dist[farIdx]) continue;
        }

        Aircraft ac;
        ac.hex = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat = lat; ac.lon = lon;
        ac.onGround = onGround;
        ac.altBaro  = altFt;
        ac.track    = !a["track"].isNull() ? a["track"].as<float>() : (!a["true_heading"].isNull() ? a["true_heading"].as<float>() : NAN);
        ac.gs       = !a["gs"].isNull() ? a["gs"].as<float>() : NAN;
        ac.baroRate = !a["baro_rate"].isNull() ? a["baro_rate"].as<float>() : NAN;
        if (a["squawk"].is<const char*>()) ac.squawk = atoi(a["squawk"].as<const char*>());
        else if (!a["squawk"].isNull())    ac.squawk = a["squawk"].as<int>();
        else                               ac.squawk = -1;
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        if (farIdx >= 0) { tmp[farIdx] = std::move(ac); dist[farIdx] = d; }   // replace the farthest kept
        else             { tmp.push_back(std::move(ac)); dist.push_back(d); }
    }

    out.swap(tmp);
    _lastOkMs = now;
    return true;
}
