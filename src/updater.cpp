#include "updater.h"
#include "config.h"
#include "ui.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <mutex>

static std::mutex s_m;
static bool  s_autoCheck   = true;
static bool  s_autoInstall = false;
static char  s_latest[16]  = "";
static char  s_status[64]  = "";
static bool  s_available   = false;
static bool  s_wantCheck   = false;
static bool  s_wantInstall = false;
static uint32_t s_nextCheckMs = 0;

static void set_status(const char *s) {
    std::lock_guard<std::mutex> g(s_m);
    snprintf(s_status, sizeof(s_status), "%s", s ? s : "");
}

// "1.8.4" -> 10804, so versions compare as plain integers. Anything unparseable sorts
// lowest, which means a malformed manifest can never look like an upgrade.
static long version_key(const char *v) {
    int a = 0, b = 0, c = 0;
    if (!v || sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return -1;
    return (long)a * 10000 + (long)b * 100 + c;
}

void updater_begin(void) {
    Preferences p;
    p.begin("capsuleradar", true);
    s_autoCheck   = p.getBool("upauto", true);
    s_autoInstall = p.getBool("upinst", false);
    p.end();
    s_nextCheckMs = millis() + 60000UL;      // let the radar settle before the first check
    set_status("not checked yet");
}

bool updater_auto_check(void)   { return s_autoCheck; }
bool updater_auto_install(void) { return s_autoInstall; }

void updater_set_auto_check(bool on) {
    s_autoCheck = on;
    Preferences p; p.begin("capsuleradar", false); p.putBool("upauto", on); p.end();
}
void updater_set_auto_install(bool on) {
    s_autoInstall = on;
    Preferences p; p.begin("capsuleradar", false); p.putBool("upinst", on); p.end();
}

void updater_request_check(void)   { s_wantCheck = true; }
void updater_request_install(void) { s_wantInstall = true; }

void updater_state(char *latest, size_t ln, char *status, size_t sn, bool *avail) {
    std::lock_guard<std::mutex> g(s_m);
    if (latest) snprintf(latest, ln, "%s", s_latest);
    if (status) snprintf(status, sn, "%s", s_status);
    if (avail)  *avail = s_available;
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
        if (_buffer && _writePos < _capacity) {
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

// --- check: fetch the manifest the flasher workflow publishes ---------------------
static void do_check(void) {
    WiFiClientSecure cli;
    cli.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(cli, UPDATE_MANIFEST_URL)) { set_status("could not reach the update server"); cli.stop(); return; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int code = http.GET();
    if (code != 200) {
        http.end();
        cli.stop();
        char m[64]; snprintf(m, sizeof(m), "update check failed (HTTP %d)", code);
        set_status(m);
        return;
    }

    PsramStream psramStream(4096); // 4 KB buffer in PSRAM
    if (!psramStream.isOk()) {
        Serial.println("[update] PSRAM stream allocation failed");
        http.end();
        cli.stop();
        set_status("update check failed (memory)");
        return;
    }

    http.writeToStream(&psramStream);
    http.end();
    cli.stop();

    JsonDocument filter(&s_jsonPsram); filter["version"] = true;
    JsonDocument doc(&s_jsonPsram);
    const DeserializationError err = deserializeJson(doc, psramStream,
                                                     DeserializationOption::Filter(filter));
    if (err) { set_status("update manifest unreadable"); return; }

    const char *ver = doc["version"] | "";
    const long remote = version_key(ver), local = version_key(FW_VERSION);
    {
        std::lock_guard<std::mutex> g(s_m);
        snprintf(s_latest, sizeof(s_latest), "%s", ver);
        s_available = (remote > 0 && local > 0 && remote > local);
    }
    if (remote < 0)          set_status("update server returned no version");
    else if (remote > local) { char m[64]; snprintf(m, sizeof(m), "v%s available", ver); set_status(m); }
    else                     set_status("up to date");
    Serial.printf("[update] local v%s, server v%s -> %s\n",
                  FW_VERSION, ver, (remote > local) ? "update available" : "current");
}

// --- install: stream the image into the inactive OTA slot -------------------------
static void do_install(void) {
    Serial.println("[update] downloading firmware...");
    set_status("downloading...");
    ui_show_flash_screen("Downloading update...", 0);

    WiFiClientSecure cli;
    cli.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(5000);
    http.setTimeout(15000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(cli, UPDATE_FIRMWARE_URL)) { set_status("download failed to start"); cli.stop(); return; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int code = http.GET();
    if (code != 200) { http.end(); cli.stop(); set_status("download failed"); return; }

    const int total = http.getSize();
    if (total <= 0) { http.end(); cli.stop(); set_status("server sent no length"); return; }
    if (!Update.begin((size_t)total)) {
        http.end();
        cli.stop();
        set_status("not enough space for the update");
        Serial.printf("[update] Update.begin(%d) failed\n", total);
        return;
    }

    // Stream in chunks: the image is >3 MB and must never be buffered whole. The buffer
    // is small and on the stack of the network task, which has 16 KB.
    WiFiClient *stream = http.getStreamPtr();
    uint8_t buf[1024];
    int written = 0;
    uint32_t lastData = millis();
    while (written < total) {
        const size_t avail = stream->available();
        if (avail) {
            const int r = stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
            if (r > 0) {
                if (Update.write(buf, r) != (size_t)r) { Update.abort(); break; }
                written += r;
                lastData = millis();
                static int lastPct = -1;
                const int pct = (int)((long)written * 100 / total);
                if (pct != lastPct) {
                    lastPct = pct;
                    char m[48]; snprintf(m, sizeof(m), "Installing... %d%%", pct);
                    set_status(m);
                    ui_show_flash_screen(m, pct);
                    if (pct % 10 == 0) Serial.printf("[update] %d%%\n", pct);
                }
            }
        } else if (!http.connected() || millis() - lastData > 20000) {
            break;                                    // stalled or the server hung up
        } else {
            delay(5);                                 // let the scheduler breathe
        }
    }
    http.end();

    if (written != total || !Update.end(true)) {
        Update.abort();
        set_status("update failed; keeping current firmware");
        Serial.printf("[update] failed after %d/%d bytes\n", written, total);
        return;
    }
    Serial.println("[update] installed; rebooting");
    set_status("installed - rebooting");
    ui_show_flash_screen("Rebooting device...", 100);
    delay(600);
    ESP.restart();
}

bool updater_poll(void) {
    if (WiFi.status() != WL_CONNECTED) return false;

    if (s_wantInstall) {
        s_wantInstall = false;
        do_install();                                 // reboots on success
        return true;
    }

    const bool due = s_autoCheck && (int32_t)(millis() - s_nextCheckMs) >= 0;
    if (s_wantCheck || due) {
        s_wantCheck = false;
        s_nextCheckMs = millis() + UPDATE_CHECK_INTERVAL_MS;
        do_check();
        // Opt-in only. Left off by default deliberately: a bad build published to the
        // update site would otherwise install itself on every device with no way back
        // that does not involve a cable.
        bool avail; { std::lock_guard<std::mutex> g(s_m); avail = s_available; }
        if (avail && s_autoInstall) do_install();
        return true;
    }
    return false;
}
