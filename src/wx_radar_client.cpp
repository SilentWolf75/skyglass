#include "wx_radar_client.h"
#include "wx_radar.h"
#include "net_fetch.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>

static PNG *s_png = nullptr;
// Frames still missing after the last fetch. main() polls faster while this is non-zero
// so the two-hour loop assembles in half a minute instead of over an hour.
static int s_pending = 0;
int wx_radar_backlog(void) { return s_pending; }
static uint32_t s_decodedPixels = 0;
static uint32_t s_sourcePixels = 0;
static int s_minX = WX_RADAR_SOURCE_SIZE, s_minY = WX_RADAR_SOURCE_SIZE;
static int s_maxX = -1, s_maxY = -1;

static bool ensure_decoder(void) {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) { Serial.println("[wxradar] PSRAM decoder allocation failed"); return false; }
    s_png = new (mem) PNG();
    Serial.printf("[wxradar] PNG decoder in PSRAM (%u bytes)\n", (unsigned)sizeof(PNG));
    return true;
}

// ---- NEXRAD recolour -----------------------------------------------------------------
// RainViewer serves its "Universal Blue" ramp and nothing else: the colour-scheme index
// in the tile URL is ignored now -- every scheme from 0 to 8 returns a byte-identical
// PNG -- so a National Weather Service look has to be produced here, from the pixels we
// are given.
//
// The source ramp, read off a storm tile: #88DDEE (weakest) darkening through blue to
// #004768, then jumping to #FFEE00 yellow, through orange, to #5D0000 maroon at the top.
// A separate low-saturation beige family fringes every echo -- the weakest returns.
// Mapping the blue half onto greens rather than onto NWS's own cyan/blue low end is
// deliberate: that blue covers light *and* moderate rain, and leaving it blue is
// precisely what stops the screen looking like a weather radar.
#define WXC(r, g, b) (uint16_t)(((b) >> 3) | (((g) >> 2) << 5) | (((r) >> 3) << 11))

static const uint16_t kNexrad[16] = {
    WXC(0xB4, 0xF0, 0xB4), WXC(0x80, 0xE8, 0x80), WXC(0x4C, 0xE0, 0x4C), WXC(0x02, 0xFD, 0x02),
    WXC(0x01, 0xE0, 0x01), WXC(0x01, 0xC5, 0x01), WXC(0x00, 0xA8, 0x00), WXC(0x00, 0x8E, 0x00),
    WXC(0xFD, 0xF8, 0x02), WXC(0xF0, 0xD0, 0x00), WXC(0xE5, 0xBC, 0x00), WXC(0xFD, 0x95, 0x00),
    WXC(0xFD, 0x50, 0x00), WXC(0xFD, 0x00, 0x00), WXC(0xBC, 0x00, 0x00), WXC(0xF8, 0x00, 0xFD),
};
// The tan fringe around every echo is the weakest band, not snow: it is present
// identically with RainViewer's snow option on and off (that parameter is ignored too),
// and it was showing over Kansas in August. Palest green, one step below the ladder.
static const uint16_t kNexradTrace = WXC(0xD8, 0xFF, 0xD8);

static inline uint16_t nexrad565(uint8_t r, uint8_t g, uint8_t b) {
    if (r < 8 && g < 8 && b < 8) return 0;                 // nothing here
    const uint8_t mx = (r > g ? (r > b ? r : b) : (g > b ? g : b));
    const uint8_t mn = (r < g ? (r < b ? r : b) : (g < b ? g : b));
    if (mx - mn < 70 && mx > 60) return kNexradTrace;      // tan fringe = trace returns
    if (r > 200 && b > 200) return kNexrad[15];            // rare magenta caps = extreme
    int t;                                                  // 0..1023 up the scale
    if (r >= 100 && r > b + 40) {                          // warm half
        t = (g >= 8) ? ((238 - (int)g) * 665) / 238        // yellow -> orange -> red
                     : 665 + ((255 - (int)r) * 335) / 162; // red -> deep maroon
        if (t < 0) t = 0;
        if (t > 1000) t = 1000;
        return kNexrad[8 + (t * 8) / 1001];
    }
    t = ((221 - (int)g) * 1000) / 150;                     // cool half, lightest first
    if (t < 0) t = 0;
    if (t > 1000) t = 1000;
    return kNexrad[(t * 8) / 1001];
}

static int radar_png_line(PNGDRAW *draw) {
    uint16_t *dst = wx_radar_back_buffer();
    const int crop = (WX_RADAR_SOURCE_SIZE - WX_RADAR_SIZE) / 2;
    if (!dst) return 1;
    uint16_t line[WX_RADAR_SOURCE_SIZE];
    if (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8) {
        const uint8_t *src = draw->pPixels;
        for (int x = 0; x < draw->iWidth; ++x, src += 4) {
            line[x] = (src[3] < 8) ? 0 : nexrad565(src[0], src[1], src[2]);
        }
    } else {
        s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
        for (int x = 0; x < draw->iWidth; ++x) {
            const uint16_t v = line[x];
            if (!v) continue;
            line[x] = nexrad565((uint8_t)(((v >> 11) & 0x1F) << 3),
                                (uint8_t)(((v >> 5) & 0x3F) << 2),
                                (uint8_t)((v & 0x1F) << 3));
        }
    }
    for (int x = 0; x < draw->iWidth; ++x) if (line[x]) {
        ++s_sourcePixels;
        if (x < s_minX) s_minX = x;
        if (x > s_maxX) s_maxX = x;
        if (draw->y < s_minY) s_minY = draw->y;
        if (draw->y > s_maxY) s_maxY = draw->y;
    }
    if (draw->y < crop || draw->y >= crop + WX_RADAR_SIZE) return 1;
    const int outY = draw->y - crop;
    const int c = WX_RADAR_SIZE / 2;
    const int dy = outY - c;
    for (int outX = 0; outX < WX_RADAR_SIZE; ++outX) {
        const int dx = outX - c;
        const uint16_t pixel =
            (dx * dx + dy * dy <= (c - 2) * (c - 2)) ? line[outX + crop] : 0;
        dst[outY * WX_RADAR_SIZE + outX] = pixel;
        if (pixel) ++s_decodedPixels;
    }
    return 1;
}

struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

class PsramStream : public Stream {
private:
    uint8_t* _buffer;
    size_t _capacity;
    size_t _writePos;
    size_t _readPos;

public:
    PsramStream(size_t capacity) {
        _capacity = capacity;
        _buffer = (uint8_t*)heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
        _writePos = 0;
        _readPos = 0;
    }

    ~PsramStream() {
        if (_buffer) heap_caps_free(_buffer);
    }

    bool isOk() const { return _buffer != nullptr; }

    size_t write(uint8_t c) override {
        if (_writePos < _capacity) {
            _buffer[_writePos++] = c;
            return 1;
        }
        return 0;
    }

    size_t write(const uint8_t *buffer, size_t size) override {
        if (!_buffer) return 0;
        size_t space = _capacity - _writePos;
        size_t toWrite = (size < space) ? size : space;
        memcpy(_buffer + _writePos, buffer, toWrite);
        _writePos += toWrite;
        return toWrite;
    }

    int read() override {
        if (_readPos < _writePos) {
            return _buffer[_readPos++];
        }
        return -1;
    }

    int peek() override {
        if (_readPos < _writePos) {
            return _buffer[_readPos];
        }
        return -1;
    }

    int available() override {
        return (int)(_writePos - _readPos);
    }
};

static bool https_get_json(const char *url, JsonDocument &doc, int timeoutMs) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setReuse(false);
    http.setConnectTimeout(3500);
    http.setTimeout(timeoutMs);
    if (!http.begin(client, url)) { client.stop(); return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        char tls[128] = "";
        const int tlsCode = client.lastError(tls, sizeof(tls));
        Serial.printf("[wxradar] HTTP %d tls=%d '%s' heap=%u largest=%u psram=%u\n",
                      status, tlsCode, tls, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        http.end();
        client.stop();
        return false;
    }

    PsramStream psramStream(49152); // 48 KB buffer in PSRAM
    if (!psramStream.isOk()) {
        Serial.println("[wxradar] PSRAM stream allocation failed");
        http.end();
        client.stop();
        return false;
    }

    http.writeToStream(&psramStream);
    http.end();
    client.stop();

    DeserializationError err = deserializeJson(doc, psramStream);
    return !err;
}

bool wx_radar_fetch(double lat, double lon) {
    if (WiFi.status() != WL_CONNECTED || !wx_radar_back_buffer() || !ensure_decoder()) return false;

    JsonDocument doc(&s_jsonPsram);
    if (!https_get_json("https://api.rainviewer.com/public/weather-maps.json", doc, 6500)) {
        Serial.println("[wxradar] metadata fetch failed"); return false;
    }
    const char *host = doc["host"] | "";
    JsonArrayConst past = doc["radar"]["past"].as<JsonArrayConst>();
    if (!host[0] || past.size() == 0) { Serial.println("[wxradar] no radar frames"); return false; }

    // Fetch ONE frame we do not already hold, newest first, so the loop's most useful
    // frames land soonest and the backlog fills over successive polls. Downloading all
    // thirteen in a row would monopolise the single network task and stall the live feed.
    const char *path = nullptr;
    uint32_t frameTime = 0;
    int wanted = 0;
    const int keep = wx_radar_capacity();
    const int first = (int)past.size() > keep ? (int)past.size() - keep : 0;
    for (int i = (int)past.size() - 1; i >= first; --i) {
        JsonObjectConst f = past[i].as<JsonObjectConst>();
        const uint32_t t = f["time"] | 0;
        if (!t) continue;
        if (wx_radar_has_frame(t)) continue;
        ++wanted;
        if (!path) { path = f["path"] | ""; frameTime = t; }
    }
    if (!path) return true;              // nothing missing: the loop is complete
    s_pending = wanted - 1;

    char url[320];
    snprintf(url, sizeof(url), "%s%s/512/7/%.5f/%.5f/2/1_1.png",
             host, path, lat, lon);
    // Stream the tile straight into PSRAM (net_fetch): a 512px PNG can be 100+ KB, and an
    // internal-heap String that big starves the live feed's TLS handshake.
    uint8_t *image = nullptr; size_t imageLen = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &image, &imageLen, 260000, 3500, 8500)) {
        Serial.println("[wxradar] tile fetch failed"); return false;
    }
    memset(wx_radar_back_buffer(), 0, WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
    s_decodedPixels = 0;
    s_sourcePixels = 0;
    s_minX = s_minY = WX_RADAR_SOURCE_SIZE;
    s_maxX = s_maxY = -1;
    const int opened = s_png->openRAM(image, imageLen, radar_png_line);
    if (opened != PNG_SUCCESS) {
        Serial.printf("[wxradar] PNG open error %d\n", opened);
        heap_caps_free(image); return false;
    }
    Serial.printf("[wxradar] PNG %dx%d bpp=%d type=%d alpha=%d\n",
                  s_png->getWidth(), s_png->getHeight(), s_png->getBpp(),
                  s_png->getPixelType(), s_png->hasAlpha());
    if (s_png->getWidth() != WX_RADAR_SOURCE_SIZE || s_png->getHeight() != WX_RADAR_SOURCE_SIZE) {
        Serial.println("[wxradar] unexpected tile dimensions");
        s_png->close(); heap_caps_free(image); return false;
    }
    const int decoded = s_png->decode(nullptr, 0);
    s_png->close();
    heap_caps_free(image);           // PNG fully decoded (or failed) — buffer no longer needed
    if (decoded != PNG_SUCCESS) { Serial.printf("[wxradar] PNG decode error %d\n", decoded); return false; }
    if (s_decodedPixels == 0) {
        Serial.println("[wxradar] decoded tile is empty (0 coloured pixels)");
    }
    wx_radar_commit(frameTime, lat, lon);
    Serial.printf("[wxradar] frame %lu updated (%u bytes, source=%lu bbox=%d,%d-%d,%d crop=%lu)\n",
                  (unsigned long)frameTime, (unsigned)imageLen,
                  (unsigned long)s_sourcePixels, s_minX, s_minY, s_maxX, s_maxY,
                  (unsigned long)s_decodedPixels);
    return true;
}
