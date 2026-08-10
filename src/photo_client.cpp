// Aircraft photo via planespotters.net (free, non-commercial, attribution).
//   GET /pub/photos/hex/{icao}  -> JSON with a thumbnail URL + photographer
//   download the JPEG -> decode (TJpgDec) into photo_buffer() as RGB565.
// Device-only. All network is on core 0 (adsb_task); the UI just displays the buffer.
#include "photo_client.h"
#include "photo.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include "sd_store.h"

#define PS_UA "SkyGlass/1.0 (+https://github.com/SilentWolf75/skyglass)"

// JPEG decode target (set just before drawJpg)
static lv_color_t *s_dst = nullptr;
static int s_dstW = 0, s_dstH = 0;

static bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= s_dstH) continue;
        for (int i = 0; i < w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= s_dstW) continue;
            s_dst[yy * s_dstW + xx].full = bmp[j * w + i];   // RGB565 -> lv_color_t
        }
    }
    return true;
}

// GET a URL into a freshly heap_caps(PSRAM) buffer. Caller frees with heap_caps_free.
static bool http_get(const char *url, uint8_t **out, size_t *outLen, size_t maxLen) {
    *out = nullptr; *outLen = 0;
    WiFiClientSecure cli;
    cli.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);    // keep short: this runs on the feed task, a slow photo
    http.setTimeout(6000);           // server must not freeze the live aircraft poll for long
    if (!http.begin(cli, url)) { Serial.println("[photo]   http.begin failed"); cli.stop(); return false; }
    http.setUserAgent(PS_UA);   // planespotters rejects the default UA; set the canonical one
    const int code = http.GET();
    if (code != 200) { Serial.printf("[photo]   HTTP %d\n", code); http.end(); cli.stop(); return false; }

    const int len = http.getSize();                  // >0 = Content-Length; -1 = chunked/unknown
    uint8_t *buf = nullptr;
    size_t got = 0;

    if (len > 0) {
        // Known length: stream the body straight into a PSRAM buffer.
        const size_t cap = ((size_t)len <= maxLen) ? (size_t)len : maxLen;
        buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { http.end(); cli.stop(); return false; }
        WiFiClient *stream = http.getStreamPtr();
        uint32_t last = millis();
        while (got < cap && (millis() - last) < 9000) {
            const size_t avail = stream->available();
            if (avail) {
                const size_t want = (cap - got < avail) ? (cap - got) : avail;
                const int r = stream->readBytes(buf + got, want);
                if (r > 0) { got += r; last = millis(); }
            } else if (!http.connected()) {
                break;
            } else {
                delay(5);
            }
            if (got >= cap) break;
        }
    } else {
        // Chunked / unknown length: getStreamPtr() does NOT undo chunked transfer
        // encoding, so the raw body would contain chunk-size markers and corrupt the
        // parse. getString() performs the chunk decode. planespotters serves its JSON
        // chunked over HTTP/1.1 (Cloudflare), and that JSON is small, so the transient
        // String on the internal heap is cheap. (Image thumbnails carry Content-Length
        // and take the branch above, so a big binary never lands here.)
        String body = http.getString();
        got = body.length();
        if (got > maxLen) got = maxLen;
        if (got > 0) {
            buf = (uint8_t *)heap_caps_malloc(got, MALLOC_CAP_SPIRAM);
            if (buf) memcpy(buf, body.c_str(), got);
            else got = 0;
        }
    }
    http.end();
    cli.stop();
    if (got == 0) { if (buf) heap_caps_free(buf); return false; }
    *out = buf; *outLen = got;
    return true;
}

// Parse JSON in PSRAM (keep internal RAM free for TLS).
struct PsramAlloc : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramAlloc s_jsonPsram;

static bool decode_and_commit(const char *hex, uint8_t *img, size_t ilen,
                              const char *credit, bool fromNetwork);

bool photo_fetch(const char *hex) {
    if (!hex || !hex[0] || WiFi.status() != WL_CONNECTED) { photo_fail(hex, true); return false; }

    // Memory guard: a photo fetch needs a TLS handshake + JPEG decode. If the largest
    // contiguous internal block is tight, skip it (degrade gracefully, never crash).
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 28000) {
        char nb[56]; snprintf(nb,sizeof(nb),"skip: low mem, largest=%u",
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        photo_note(nb);
        Serial.println("[photo] low memory, skipping");
        photo_fail(hex, true);
        return false;
    }

    // 0) the card, if we have seen this airframe before. Skips both requests, so a
    //    known aircraft shows its photo immediately and works with no internet at all.
    {
        unsigned char *cached = nullptr; size_t clen = 0; char ccredit[64] = "";
        if (sd_photo_load(hex, &cached, &clen, ccredit, sizeof(ccredit)) && cached) {
            if (decode_and_commit(hex, cached, clen, ccredit, false)) return true;
            // A cached file that will not decode is a corrupt one; drop it and fetch.
            sd_photo_forget(hex);
        }
    }

    // 1) planespotters lookup (JSON)
    char url[128];
    snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", hex);
    uint8_t *jbuf = nullptr; size_t jlen = 0;
    if (!http_get(url, &jbuf, &jlen, 8192)) { photo_note("planespotters request failed"); Serial.printf("[photo] %s: planespotters request failed\n", hex); photo_fail(hex, true); return false; }

    JsonDocument filter(&s_jsonPsram);
    filter["photos"][0]["thumbnail_large"]["src"] = true;
    filter["photos"][0]["photographer"] = true;
    JsonDocument doc(&s_jsonPsram);
    const DeserializationError err = deserializeJson(doc, jbuf, jlen, DeserializationOption::Filter(filter));
    heap_caps_free(jbuf);
    if (err) { photo_note("planespotters json parse error"); Serial.printf("[photo] %s: json err %s\n", hex, err.c_str()); photo_fail(hex, true); return false; }

    const char *imgUrl = doc["photos"][0]["thumbnail_large"]["src"] | "";
    char credit[40];
    snprintf(credit, sizeof(credit), "%s", (const char *)(doc["photos"][0]["photographer"] | ""));
    // The only definitive answer: planespotters replied and has nothing for this
    // airframe. Cache it so we stop asking; every other failure above is transient.
    if (!imgUrl[0]) { photo_note("planespotters has none for this airframe"); Serial.printf("[photo] %s: no photo available\n", hex); photo_fail(hex, false); return false; }

    // 2) download the JPEG thumbnail.
    // planespotters serves *progressive* JPEGs, which TJpgDec cannot decode. Route the
    // image through the weserv.nl image proxy, which re-encodes to baseline JPEG and
    // resizes to our canvas width — the result is small (~5 KB) and decodable.
    const char *bare = imgUrl;
    if      (strncmp(bare, "https://", 8) == 0) bare += 8;
    else if (strncmp(bare, "http://",  7) == 0) bare += 7;
    int canvasW = 232, canvasH = 156;
    photo_buffer(&canvasW, &canvasH);                 // resize to fit the canvas (preserve aspect)
    char proxUrl[256];
    snprintf(proxUrl, sizeof(proxUrl),
             "https://images.weserv.nl/?url=%s&w=%d&h=%d&fit=inside&output=jpg", bare, canvasW, canvasH);

    uint8_t *img = nullptr; size_t ilen = 0;
    if (!http_get(proxUrl, &img, &ilen, 65536)) { photo_note("weserv proxy download failed"); Serial.printf("[photo] %s: image download failed\n", hex); photo_fail(hex, true); return false; }

    // Save before decoding: this is the only point the JPEG exists in one piece, and
    // next time it spares both network requests and the wait.
    sd_photo_save(hex, img, ilen, credit);

    return decode_and_commit(hex, img, ilen, credit, true);
}

// Shared by the cached and the freshly downloaded path: the bytes are the same JPEG
// either way, only their origin differs. Takes ownership of img and frees it.
static bool decode_and_commit(const char *hex, uint8_t *img, size_t ilen,
                              const char *credit, bool fromNetwork) {
    // 3) decode into the shared PSRAM buffer, scaled to fit
    int maxW = 0, maxH = 0;
    lv_color_t *dst = photo_buffer(&maxW, &maxH);
    uint16_t jw = 0, jh = 0;
    if (TJpgDec.getJpgSize(&jw, &jh, img, ilen) != JDR_OK || jw == 0 || jh == 0) {
        photo_note("getJpgSize failed");
        Serial.printf("[photo] %s: getJpgSize failed\n", hex);
        heap_caps_free(img); photo_fail(hex, true); return false;
    }
    uint8_t scale = 1;
    while ((jw / scale) > (uint16_t)maxW || (jh / scale) > (uint16_t)maxH) { scale <<= 1; if (scale >= 8) break; }
    s_dstW = (int)(jw / scale); if (s_dstW > maxW) s_dstW = maxW;
    s_dstH = (int)(jh / scale); if (s_dstH > maxH) s_dstH = maxH;
    s_dst = dst;
    // From here the shared buffer holds a half-drawn image of THIS aircraft while
    // s_doneHex still names the previous one. Tell the UI to stop rendering it.
    photo_invalidate();
    for (int i = 0; i < s_dstW * s_dstH; ++i) s_dst[i].full = 0;   // clear

    TJpgDec.setJpgScale(scale);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(jpg_out);
    const JRESULT jr = TJpgDec.drawJpg(0, 0, img, ilen);
    heap_caps_free(img);

    if (jr != JDR_OK) { photo_note("jpeg decode failed"); photo_fail(hex, true); return false; }
    photo_note(fromNetwork ? "ok" : "ok (cached)");
    photo_commit(s_dstW, s_dstH, hex, credit);
    Serial.printf("[photo] %s: %dx%d (scale 1/%d) by %s\n", hex, s_dstW, s_dstH, scale, credit);
    return true;
}
