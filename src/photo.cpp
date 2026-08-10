#include "photo.h"
#include <mutex>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

#define PH_MAXW 232
#define PH_MAXH 156

static std::mutex  s_m;
static lv_color_t *s_buf = nullptr;
static char s_want[10] = "", s_doneHex[10] = "", s_credit[40] = "";
static char s_note[56] = "";
static int  s_w = 0, s_h = 0;
static bool s_ready = false;

static lv_color_t *ensure_buf() {
    if (!s_buf) {
        const size_t sz = (size_t)PH_MAXW * PH_MAXH * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
        s_buf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_buf = (lv_color_t *)malloc(sz);
#endif
    }
    return s_buf;
}

void photo_request(const char *hex) {
    std::lock_guard<std::mutex> g(s_m);
    snprintf(s_want, sizeof(s_want), "%s", hex ? hex : "");
}

bool photo_pending(char *o, size_t n) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_want[0] && strcmp(s_want, s_doneHex) != 0) { snprintf(o, n, "%s", s_want); return true; }
    return false;
}

lv_color_t *photo_buffer(int *mw, int *mh) {
    if (mw) *mw = PH_MAXW;
    if (mh) *mh = PH_MAXH;
    return ensure_buf();
}

#define PHOTO_MAX_TRIES 3      // bound the retries: a permanently broken image must settle

void photo_fail(const char *hex, bool transient) {
    std::lock_guard<std::mutex> g(s_m);
    static char lastHex[10] = "";
    static int  tries = 0;
    if (!hex) hex = "";
    if (strcmp(hex, lastHex) != 0) { snprintf(lastHex, sizeof(lastHex), "%s", hex); tries = 0; }
    ++tries;
    if (transient && tries < PHOTO_MAX_TRIES) return;   // leave pending -> retried next poll
    snprintf(s_doneHex, sizeof(s_doneHex), "%s", hex);
    s_credit[0] = 0;
    s_w = s_h = 0;
    s_ready = false;
}

void photo_commit(int w, int h, const char *hex, const char *credit) {
    std::lock_guard<std::mutex> g(s_m);
    snprintf(s_doneHex, sizeof(s_doneHex), "%s", hex ? hex : "");
    snprintf(s_credit,  sizeof(s_credit),  "%s", credit ? credit : "");
    s_w = w; s_h = h;
    s_ready = (w > 0 && h > 0);
}

// Called on the network task the instant the decoder starts writing into the shared
// buffer. There is only one buffer for every aircraft, and s_doneHex still names the
// PREVIOUS aircraft until photo_commit() runs at the end of the decode -- so without
// this, tapping back to that previous aircraft mid-decode makes photo_get() succeed and
// LVGL renders half-written pixels of a different airframe. Clearing s_ready alone is
// enough: photo_get() tests it first. s_doneHex is deliberately left intact so
// photo_done() still reports the fetch as finished and nothing re-requests in a loop.
void photo_invalidate(void) {
    std::lock_guard<std::mutex> g(s_m);
    s_ready = false;
}

bool photo_get(const char *hex, int *w, int *h, char *credit, size_t cn) {
    std::lock_guard<std::mutex> g(s_m);
    if (hex && s_ready && s_doneHex[0] && strcmp(hex, s_doneHex) == 0) {
        if (w) *w = s_w;
        if (h) *h = s_h;
        if (credit) snprintf(credit, cn, "%s", s_credit);
        return true;
    }
    return false;
}

void photo_note(const char *why) {
    std::lock_guard<std::mutex> g(s_m);
    snprintf(s_note, sizeof(s_note), "%s", why ? why : "");
}

const char *photo_note_get(void) { return s_note; }

bool photo_done(const char *hex) {
    std::lock_guard<std::mutex> g(s_m);
    return hex && s_doneHex[0] && strcmp(hex, s_doneHex) == 0;   // committed (photo or not)
}
