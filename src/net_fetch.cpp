// Shared streamed-download helper (see net_fetch.h). Mirrors the proven pattern in
// photo_client.cpp: Content-Length -> stream into PSRAM; chunked -> getString decode.
#include "net_fetch.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if defined(BOARD_WAVESHARE_P4_LCD_4C)
#define NET_TLS_MAX_CONCURRENT 3
#else
#define NET_TLS_MAX_CONCURRENT 2
#endif

static SemaphoreHandle_t s_tlsSem = nullptr;

struct TlsSlot {
    bool held;
    explicit TlsSlot(uint32_t waitMs = 3000) {
        if (!s_tlsSem) s_tlsSem = xSemaphoreCreateCounting(NET_TLS_MAX_CONCURRENT, NET_TLS_MAX_CONCURRENT);
        held = (s_tlsSem && xSemaphoreTake(s_tlsSem, pdMS_TO_TICKS(waitMs)) == pdTRUE);
    }
    ~TlsSlot() { if (held && s_tlsSem) xSemaphoreGive(s_tlsSem); }
};

bool net_fetch_psram(const char *url, const char *userAgent,
                     uint8_t **out, size_t *outLen, size_t maxLen,
                     int connectTimeoutMs, int totalTimeoutMs) {
    *out = nullptr; *outLen = 0;
    if (WiFi.status() != WL_CONNECTED) return false;

    TlsSlot slot(3000);
    if (!slot.held) {
        Serial.println("[net] TLS slot busy, postponing fetch");
        return false;
    }

    WiFiClientSecure cli;
    cli.setInsecure();                        // hobby device (matches the other clients)
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(connectTimeoutMs);
    http.setTimeout(totalTimeoutMs);
    if (!http.begin(cli, url)) { cli.stop(); return false; }
    if (userAgent) http.setUserAgent(userAgent);

    const int code = http.GET();
    if (code != 200) { Serial.printf("[net] HTTP %d\n", code); http.end(); cli.stop(); return false; }

    const int len = http.getSize();           // >0 = Content-Length; -1 = chunked/unknown
    uint8_t *buf = nullptr;
    size_t got = 0;

    if (len > 0) {
        // Known length: stream the body straight into a PSRAM buffer.
        const size_t cap = ((size_t)len <= maxLen) ? (size_t)len : maxLen;
        buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        if (!buf) { http.end(); cli.stop(); return false; }
        WiFiClient *stream = http.getStreamPtr();
        uint32_t last = millis();
        while (got < cap && (millis() - last) < (uint32_t)totalTimeoutMs) {
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
        }
    } else {
        // Chunked/unknown: getString() performs the chunk decode; only small payloads
        // are expected here (image CDNs send Content-Length), so the String is cheap.
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
