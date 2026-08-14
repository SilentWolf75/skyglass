#include "wx_radar.h"
#include <mutex>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

// A ring of decoded frames kept in time order, plus one scratch buffer the decoder writes
// into. Slots are allocated lazily and the count is whatever PSRAM actually granted, so a
// board that cannot spare the memory degrades to a shorter loop -- or to a single frame --
// rather than failing to show weather at all.
static std::mutex s_mutex;

struct Frame { uint16_t *px; uint32_t time; };
static Frame     s_frames[WX_RADAR_FRAMES];
static int       s_count = 0;          // frames held, oldest first
static int       s_cap = 0;            // slots successfully allocated
static uint16_t *s_back = nullptr;     // decode scratch
static uint32_t  s_version = 0;
static double    s_lat = 0, s_lon = 0;
static int       s_zoom = WX_ZOOM_DEF;

int wx_radar_zoom(void) { return s_zoom; }

float wx_radar_range_km(void) {
    // 106.7 km across 512 px at zoom 7; every step up halves it.
    float km = 106.7f * (float)WX_RADAR_SIZE / 512.0f;
    for (int z = 7; z < s_zoom; ++z) km *= 0.5f;
    for (int z = s_zoom; z < 7; ++z) km *= 2.0f;
    return km;
}

void wx_radar_set_zoom(int z) {
    if (z < WX_ZOOM_MIN) z = WX_ZOOM_MIN;
    if (z > WX_ZOOM_MAX) z = WX_ZOOM_MAX;
    if (z == s_zoom) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    s_zoom = z;
    // Every held frame is at the old scale. Keeping them would animate a loop that jumps
    // between two ground scales, which reads as the map twitching rather than as weather.
    s_count = 0;
    for (int i = 0; i < s_cap; ++i) s_frames[i].time = 0;
    ++s_version;
}

static uint16_t *alloc_pixels(void) {
    const size_t bytes = WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
#ifdef ARDUINO
    return (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return (uint16_t *)malloc(bytes);
#endif
}

void wx_radar_begin(void) {
    if (s_back) return;
    const size_t bytes = WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
    s_back = alloc_pixels();
    if (s_back) memset(s_back, 0, bytes);
    for (int i = 0; i < WX_RADAR_FRAMES; ++i) {
        s_frames[i].px = alloc_pixels();
        if (!s_frames[i].px) break;        // take what we got and stop asking
        memset(s_frames[i].px, 0, bytes);
        s_frames[i].time = 0;
        s_cap = i + 1;
    }
}

uint16_t *wx_radar_back_buffer(void) { return s_back; }
int wx_radar_capacity(void) { return s_cap; }

int wx_radar_frame_count(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_count;
}

bool wx_radar_has_frame(uint32_t frameTime) {
    std::lock_guard<std::mutex> lock(s_mutex);
    for (int i = 0; i < s_count; ++i) if (s_frames[i].time == frameTime) return true;
    return false;
}

void wx_radar_commit(uint32_t frameTime, double lat, double lon) {
    if (!s_back || s_cap == 0) return;
    std::lock_guard<std::mutex> lock(s_mutex);

    // Frames arrive out of order while the backlog fills, so find where this one belongs
    // in time rather than appending.
    int at = s_count;
    bool inPlace = false;
    for (int i = 0; i < s_count; ++i) {
        if (frameTime == s_frames[i].time) { at = i; inPlace = true; break; }
        if (frameTime <  s_frames[i].time) { at = i; break; }
    }

    if (!inPlace) {
        if (s_count == s_cap) {
            if (at == 0) return;          // older than everything held: not worth a slot
            // Evict the oldest: its buffer is recycled into the insertion point.
            uint16_t *recycled = s_frames[0].px;
            for (int i = 0; i < at - 1; ++i) s_frames[i] = s_frames[i + 1];
            s_frames[at - 1].px = recycled;
            at -= 1;
        } else {
            uint16_t *spare = s_frames[s_count].px;
            for (int i = s_count; i > at; --i) s_frames[i] = s_frames[i - 1];
            s_frames[at].px = spare;
            ++s_count;
        }
    }

    // Swap rather than copy: the scratch buffer becomes the frame and the frame's old
    // buffer becomes scratch, so a commit costs a pointer exchange.
    uint16_t *old = s_frames[at].px;
    s_frames[at].px = s_back;
    s_frames[at].time = frameTime;
    s_back = old;

    s_lat = lat; s_lon = lon; ++s_version;
}

bool wx_radar_frame(int idx, const uint16_t **pixels, uint32_t *frameTime) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (idx < 0 || idx >= s_count) return false;
    if (pixels)    *pixels = s_frames[idx].px;
    if (frameTime) *frameTime = s_frames[idx].time;
    return true;
}

bool wx_radar_front(const uint16_t **pixels, uint32_t *frameTime,
                    double *lat, double *lon, uint32_t *version) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_count == 0) return false;
    const Frame &f = s_frames[s_count - 1];
    if (pixels)    *pixels = f.px;
    if (frameTime) *frameTime = f.time;
    if (lat)       *lat = s_lat;
    if (lon)       *lon = s_lon;
    if (version)   *version = s_version;
    return true;
}
