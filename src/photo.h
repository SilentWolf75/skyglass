#pragma once
// Shared aircraft-photo state (portable). The actual fetch+JPEG decode lives in
// photo_client (device-only); this just holds the decoded RGB565 image + request
// handshake, guarded for cross-thread access (like route.*).
#include <lvgl.h>
#include <stddef.h>

void photo_request(const char *hex);                 // UI: I want this hex's photo
bool photo_pending(char *hexOut, size_t n);          // network task: hex to fetch (deduped)
lv_color_t *photo_buffer(int *maxW, int *maxH);      // PSRAM RGB565 buffer to decode into
void photo_commit(int w, int h, const char *hex, const char *credit);  // ready (w=0 => none)
// Report a failed fetch. `transient` (a dropped connection, a decode error) leaves the
// request open so the next poll retries; a definitive "this airframe has no photo" is
// cached like a success so it is not asked for again. Without the distinction one
// flaky download made an aircraft permanently photo-less for the session.
void photo_fail(const char *hex, bool transient);
// Mark the shared buffer as being rewritten; photo_get() will refuse until the next
// photo_commit(). Must be called before the decoder writes a single pixel.
void photo_invalidate(void);
bool photo_get(const char *hex, int *w, int *h, char *credit, size_t cn);  // ready & matches?
bool photo_done(const char *hex);                    // fetch finished for this hex (with or without a photo)

// Why the last fetch ended the way it did. Every branch in photo_client records one,
// so "no photo on the card" can be diagnosed over WiFi (/diag) instead of only from a
// serial cable. Purely diagnostic; nothing behavioural depends on it.
void photo_note(const char *why);
const char *photo_note_get(void);
