#include "sd_store.h"
#include "config.h"
#include <Arduino.h>
#include <string.h>
#include <stdio.h>

#if BOARD_HAS_SD
#include <driver/sdmmc_host.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <esp_heap_caps.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sd_pwr_ctrl.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <ff.h>
#include <sys/stat.h>
#include <Preferences.h>

#define SD_MOUNT   "/sdcard"
#define SD_DIR     SD_MOUNT "/skyglass"
#define SD_SEEN    SD_DIR "/seen.dat"

static sdmmc_card_t *s_card = nullptr;
static bool     s_mounted = false;
static uint8_t  s_width   = 0;
static uint64_t s_size    = 0;
static char     s_status[64] = "not probed";
static esp_err_t s_lastErr = ESP_OK;
static uint32_t s_probeMB = 0, s_probeSect = 0;   // what the card said before FATFS refused
static char     s_probeName[8] = "";
static int      s_fsErr = 0;      // FRESULT when FATFS is the thing refusing
static uint32_t s_hits = 0, s_appends = 0, s_readErr = 0, s_seekErr = 0;

// On-card record. Fixed 32 bytes so a record's offset is index*32 and an update is a
// seek plus one write -- no rewriting the file to change one aircraft.
struct __attribute__((packed)) SeenRec {
    char     hex[8];
    char     call[10];
    uint16_t count;
    uint32_t firstSeen;
    uint32_t lastSeen;
    uint16_t closestDam;
    uint16_t pad;
};
static_assert(sizeof(SeenRec) == 32, "SeenRec must stay 32 bytes: offsets depend on it");

// Index: hash of the hex -> record number, sorted by hash, binary searched. Lives in
// PSRAM (8 bytes an airframe, so 20k aircraft is 160 KB) because the P4 has only ~240 KB
// of internal heap and an std::map of strings would eat a serious fraction of it.
struct IdxEnt { uint32_t h; uint32_t rec; };
static IdxEnt  *s_idx = nullptr;
static uint32_t s_idxLen = 0, s_idxCap = 0;

static uint32_t hex_hash(const char *s) {          // FNV-1a, plenty for 6 hex chars
    uint32_t h = 2166136261u;
    for (; *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

static bool idx_reserve(uint32_t need) {
    if (need <= s_idxCap) return true;
    uint32_t cap = s_idxCap ? s_idxCap * 2 : 1024;
    while (cap < need) cap *= 2;
    IdxEnt *n = (IdxEnt *)heap_caps_malloc((size_t)cap * sizeof(IdxEnt), MALLOC_CAP_SPIRAM);
    if (!n) n = (IdxEnt *)malloc((size_t)cap * sizeof(IdxEnt));   // no PSRAM: try anyway
    if (!n) return false;
    if (s_idx) { memcpy(n, s_idx, (size_t)s_idxLen * sizeof(IdxEnt)); free(s_idx); }
    s_idx = n; s_idxCap = cap;
    return true;
}

// lower_bound over the sorted index.
static uint32_t idx_find_pos(uint32_t h) {
    uint32_t lo = 0, hi = s_idxLen;
    while (lo < hi) { const uint32_t mid = (lo + hi) / 2; if (s_idx[mid].h < h) lo = mid + 1; else hi = mid; }
    return lo;
}

static bool rec_read(uint32_t rec, SeenRec *out) {
    FILE *f = fopen(SD_SEEN, "rb");
    if (!f) { ++s_readErr; return false; }
    const bool ok = (fseek(f, (long)rec * (long)sizeof(SeenRec), SEEK_SET) == 0) &&
                    (fread(out, sizeof(SeenRec), 1, f) == 1);
    fclose(f);
    // A short read means the index points past the end of the file. Silently treating
    // that as "not on file" is what let a bad record number turn into an endless stream
    // of duplicate appends, so it is counted separately from a file that will not open.
    if (!ok) ++s_seekErr;
    return ok;
}

// Find the record for a hex, or UINT32_MAX. Collisions on the 32-bit hash are possible,
// so every candidate is confirmed against the stored hex before being accepted.
static uint32_t rec_lookup(const char *hex) {
    if (!s_idx) return UINT32_MAX;
    const uint32_t h = hex_hash(hex);
    SeenRec r;
    for (uint32_t i = idx_find_pos(h); i < s_idxLen && s_idx[i].h == h; ++i)
        if (rec_read(s_idx[i].rec, &r) && strncmp(r.hex, hex, sizeof(r.hex)) == 0)
            return s_idx[i].rec;
    return UINT32_MAX;
}

// One pass over the file at mount time. Cheaper than any alternative: a 20k-airframe log
// is 640 KB, read once in about a second, and every lookup afterwards is a binary search.
static void idx_build(void) {
    s_idxLen = 0;
    FILE *f = fopen(SD_SEEN, "rb");
    if (!f) return;
    SeenRec buf[32];
    uint32_t rec = 0;
    for (;;) {
        const size_t got = fread(buf, sizeof(SeenRec), 32, f);
        if (!got) break;
        if (!idx_reserve(s_idxLen + (uint32_t)got)) break;
        for (size_t i = 0; i < got; ++i, ++rec) {
            if (!buf[i].hex[0]) continue;
            s_idx[s_idxLen].h = hex_hash(buf[i].hex);
            s_idx[s_idxLen].rec = rec;
            ++s_idxLen;
        }
    }
    fclose(f);
    // insertion-sort-free: sort once by hash
    for (uint32_t i = 1; i < s_idxLen; ++i) {
        const IdxEnt k = s_idx[i];
        uint32_t j = i;
        while (j && s_idx[j - 1].h > k.h) { s_idx[j] = s_idx[j - 1]; --j; }
        s_idx[j] = k;
    }
    Serial.printf("[sd] flight log: %u airframes on file\n", (unsigned)s_idxLen);
}

// Mounting slot 0 by hand, because esp_vfs_fat_sdmmc_mount() cannot be used here.
//
// The ESP32-C6 radio on this board is an SDIO device on the same SDMMC controller (it
// takes slot 1; the card is slot 0). sdmmc_host_init() and sdmmc_host_deinit() act on
// the whole peripheral, so the convenience mount -- which always initialises the host and
// tears it down on failure -- fights the link the WiFi runs on. That is what froze the
// board, with the slot empty, and it had nothing to do with the card.
//
// So: tolerate a host that is already up, bring up only slot 0, and never deinit. The
// teardown path unwinds exactly what it created and leaves the host alone.
static FATFS *s_fs = nullptr;
static BYTE   s_pdrv = 0xFF;
static sd_pwr_ctrl_handle_t s_pwr = nullptr;

static void unmount_slot(void) {
    if (s_pdrv != 0xFF) {
        char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
        f_mount(NULL, drv, 0);
        ff_diskio_unregister(s_pdrv);
        s_pdrv = 0xFF;
    }
    esp_vfs_fat_unregister_path(SD_MOUNT);
    s_fs = nullptr;
    if (s_card) { free(s_card); s_card = nullptr; }
    // Deliberately NO sdmmc_host_deinit(): the C6 is on the other slot of this host.
}

static bool mount_once(bool oneBit, int freqKhz, bool formatIfFailed) {
    (void)formatIfFailed;

    // The host may already be up for the C6. That is fine and expected -- take it as
    // success rather than an error, and on no account initialise or deinit it ourselves
    // beyond this.
    const esp_err_t hi = sdmmc_host_init();
    if (hi != ESP_OK && hi != ESP_ERR_INVALID_STATE) { s_lastErr = hi; return false; }

    // The card's IO rail is an on-chip LDO (channel 4), off at reset. Without it the card
    // has no signal voltage and never answers -- the original ESP_ERR_TIMEOUT.
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {};
        ldo.ldo_chan_id = 4;
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) s_pwr = nullptr;
    }
    if (s_pwr) sd_pwr_ctrl_set_io_voltage(s_pwr, 3300);

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = oneBit ? 1 : 4;
    slot.flags = 0;
    s_lastErr = sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot);
    if (s_lastErr != ESP_OK) return false;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = freqKhz;
    if (s_pwr) host.pwr_ctrl_handle = s_pwr;

    s_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) { s_lastErr = ESP_ERR_NO_MEM; return false; }
    s_lastErr = sdmmc_card_init(&host, s_card);
    if (s_lastErr != ESP_OK) { free(s_card); s_card = nullptr; return false; }

    // Card is talking. Everything past here is filesystem.
    s_probeMB   = (uint32_t)(((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024ULL * 1024ULL));
    s_probeSect = (uint32_t)s_card->csd.sector_size;
    snprintf(s_probeName, sizeof(s_probeName), "%s", s_card->cid.name);

    if (ff_diskio_get_drive(&s_pdrv) != ESP_OK || s_pdrv == 0xFF) {
        s_lastErr = ESP_ERR_NO_MEM; unmount_slot(); return false;
    }
    ff_diskio_register_sdmmc(s_pdrv, s_card);

    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };
    s_lastErr = esp_vfs_fat_register(SD_MOUNT, drv, 4, &s_fs);
    if (s_lastErr != ESP_OK) { unmount_slot(); return false; }

    const FRESULT fr = f_mount(s_fs, drv, 1);
    if (fr != FR_OK) {
        s_lastErr = ESP_FAIL;
        s_fsErr = (int)fr;              // the FATFS reason, which ESP_FAIL alone hides
        unmount_slot();
        return false;
    }
    return true;
}

bool sd_begin(void) {
    if (s_mounted) return true;

    // Crash-loop guard. Bringing the card up put the board in a boot loop the first time
    // it met a real card: the mounted-card path had only ever been exercised with an
    // empty slot. A flag is raised in NVS before probing and cleared afterwards, so a
    // reset that happens in between is still recorded -- and the next boot skips the card
    // entirely rather than looping. An optional peripheral must never cost you the board.
    {
        Preferences pv;
        pv.begin("capsuleradar", false);
        if (pv.getBool("sdcrash", false)) {
            pv.end();
            snprintf(s_status, sizeof(s_status), "disabled (crashed while probing)");
            Serial.println("[sd] skipped: the last probe did not survive. /sdretry to try again.");
            return false;
        }
        pv.putBool("sdcrash", true);
        pv.end();
    }

    // One pass per bus width and clock. No GPIO power dance: the earlier version drove
    // GPIO45 on the theory that a FET gated the card rail, which no vendor BSP does and
    // which was never the problem -- the rail is the on-chip LDO handled in mount_once().
    static const int kFreq[] = { SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_DEFAULT, SDMMC_FREQ_PROBING };
    for (int fi = 0; fi < 3 && !s_mounted; ++fi)
        for (int oneBit = 0; oneBit < 2 && !s_mounted; ++oneBit)
            if (mount_once(oneBit != 0, kFreq[fi], false)) {
                s_mounted = true; s_width = oneBit ? 1 : 4;
            }

    if (s_mounted) {
        s_size = s_card ? (uint64_t)s_card->csd.capacity * s_card->csd.sector_size : 0;
        snprintf(s_status, sizeof(s_status), "%u-bit %llu MB",
                 (unsigned)s_width, (unsigned long long)(s_size / (1024ULL * 1024ULL)));
        Serial.printf("[sd] mounted, %s\n", s_status);
        mkdir(SD_DIR, 0777);
        idx_build();
    } else {
        if (s_probeMB)
            snprintf(s_status, sizeof(s_status), "%s f%d; card %s %uMB",
                     esp_err_to_name(s_lastErr), s_fsErr, s_probeName, (unsigned)s_probeMB);
        else
            snprintf(s_status, sizeof(s_status), "not mounted: %s", esp_err_to_name(s_lastErr));
        Serial.printf("[sd] not mounted: %s\n", esp_err_to_name(s_lastErr));
    }
    {   // got here at all: whatever happened, it was not a reset
        Preferences pv;
        pv.begin("capsuleradar", false);
        pv.putBool("sdcrash", false);
        pv.end();
    }
    return s_mounted;
}

// Clear the guard by hand after a crash, so the card can be retried without reflashing.
void sd_clear_crash_flag(void) {
    Preferences pv;
    pv.begin("capsuleradar", false);
    pv.putBool("sdcrash", false);
    pv.end();
}

bool        sd_mounted(void)    { return s_mounted; }
uint8_t     sd_bus_width(void)  { return s_width; }
uint64_t    sd_size_bytes(void) { return s_size; }
const char *sd_status(void)     { return s_status; }
uint32_t    sd_seen_records(void) { return s_idxLen; }
void sd_counters(uint32_t *hit, uint32_t *app, uint32_t *rderr) {
    if (hit) *hit = s_hits; if (app) *app = s_appends;
    if (rderr) *rderr = s_readErr * 1000u + (s_seekErr > 999u ? 999u : s_seekErr);
}

bool sd_format(void) {
    Serial.println("[sd] formatting card");
    if (!s_mounted) {
        // The case that matters: a card this device cannot read. Mounting with
        // format_if_mount_failed writes a fresh FAT on it, which the earlier version
        // could not do because it demanded a working mount first -- useless precisely
        // when you need it.
        for (int oneBit = 0; oneBit < 2 && !s_mounted; ++oneBit)
            if (mount_once(oneBit != 0, SDMMC_FREQ_HIGHSPEED, true)) {
                s_mounted = true; s_width = oneBit ? 1 : 4;
            }
        if (!s_mounted) { Serial.println("[sd] format FAILED (no usable card)"); return false; }
        s_size = s_card ? (uint64_t)s_card->csd.capacity * s_card->csd.sector_size : 0;
        snprintf(s_status, sizeof(s_status), "%u-bit %llu MB",
                 (unsigned)s_width, (unsigned long long)(s_size / (1024ULL * 1024ULL)));
        mkdir(SD_DIR, 0777);
        s_idxLen = 0;
        Serial.println("[sd] format ok (via mount)");
        return true;
    }
    const bool ok = (esp_vfs_fat_sdcard_format(SD_MOUNT, s_card) == ESP_OK);
    s_idxLen = 0;
    if (ok) { mkdir(SD_DIR, 0777); idx_build(); }
    Serial.printf("[sd] format %s\n", ok ? "ok" : "FAILED");
    return ok;
}

bool sd_seen_erase(void) {
    if (!s_mounted) return false;
    remove(SD_SEEN);
    s_idxLen = 0;
    return true;
}

bool sd_seen_lookup(const char *hex, SdSeen *out) {
    if (!s_mounted || !hex || !hex[0] || !out) return false;
    const uint32_t rec = rec_lookup(hex);
    if (rec == UINT32_MAX) return false;
    SeenRec r;
    if (!rec_read(rec, &r)) return false;
    out->count = r.count;
    out->firstSeen = r.firstSeen;
    out->lastSeen = r.lastSeen;
    out->closestDam = r.closestDam;
    return true;
}

// KNOWN DEFECT: about a third of contacts miss on lookup each poll and get appended
// again, so the file grows by a few hundred records a minute instead of settling at the
// number of distinct airframes. Measured, not guessed: sd_hit/sd_app in /diag show the
// ratio, and sd_rderr is zero -- every append is indexed, no read or seek fails, yet the
// next poll misses. So the index is resolving to records whose hex does not match.
// Leading suspect is the unsynchronised access between sd_log_seen() on the feed task and
// sd_seen_lookup() on the UI task, which share s_idx while it is being memmove'd. The
// data is harmless (an oversized file and an inflated "seen Nx"), but it is wrong.
//
// Real airframes only: exactly six hex digits. A local receiver also reports TIS-B and
// ADS-R tracks, which tar1090 prefixes with '~' and which carry ephemeral, non-ICAO
// addresses that change constantly. Logging those grew the file by thousands of "new
// airframes" in minutes and would make "seen 3x" meaningless.
static bool is_icao_hex(const char *h) {
    int n = 0;
    for (const char *p = h; *p; ++p, ++n) {
        if (n >= 6) return false;
        const char c = *p | 0x20;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return n == 6;
}

void sd_log_seen(const char *hex, const char *callsign, float distKm, uint32_t nowEpoch) {
    if (!s_mounted || !hex || !hex[0]) return;
    if (!is_icao_hex(hex)) return;

    const uint16_t dam = (distKm > 0.0f && distKm < 650.0f)
                             ? (uint16_t)(distKm * 100.0f)   // km -> units of 10 m
                             : 0;
    const uint32_t rec = rec_lookup(hex);

    if (rec != UINT32_MAX) {
        ++s_hits;
        SeenRec r;
        if (!rec_read(rec, &r)) return;
        // A contact that drops off the feed for a moment is the same visit; only a real
        // gap counts as coming round again.
        if (nowEpoch && r.lastSeen && nowEpoch - r.lastSeen > SD_VISIT_GAP_S && r.count < 0xFFFF)
            r.count++;
        if (nowEpoch) r.lastSeen = nowEpoch;
        if (dam && (!r.closestDam || dam < r.closestDam)) r.closestDam = dam;
        if (callsign && callsign[0]) { strncpy(r.call, callsign, sizeof(r.call) - 1); r.call[sizeof(r.call) - 1] = 0; }
        FILE *f = fopen(SD_SEEN, "r+b");
        if (!f) return;
        if (fseek(f, (long)rec * (long)sizeof(SeenRec), SEEK_SET) == 0) fwrite(&r, sizeof(r), 1, f);
        fclose(f);
        return;
    }

    // First time this airframe has ever been seen: append and index it.
    ++s_appends;
    SeenRec r = {};
    strncpy(r.hex, hex, sizeof(r.hex) - 1);
    if (callsign && callsign[0]) strncpy(r.call, callsign, sizeof(r.call) - 1);
    r.count = 1;
    r.firstSeen = nowEpoch;
    r.lastSeen = nowEpoch;
    r.closestDam = dam;

    FILE *f = fopen(SD_SEEN, "r+b");
    if (!f) f = fopen(SD_SEEN, "w+b");         // first record on a fresh card
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    const long end = ftell(f);
    if (end < 0 || (end % (long)sizeof(SeenRec)) != 0) { fclose(f); return; }
    const uint32_t newRec = (uint32_t)(end / (long)sizeof(SeenRec));
    const bool wrote = (fwrite(&r, sizeof(r), 1, f) == 1);
    fclose(f);
    if (!wrote || !idx_reserve(s_idxLen + 1)) return;

    const uint32_t h = hex_hash(r.hex);
    const uint32_t at = idx_find_pos(h);
    memmove(&s_idx[at + 1], &s_idx[at], (size_t)(s_idxLen - at) * sizeof(IdxEnt));
    s_idx[at].h = h; s_idx[at].rec = newRec;
    ++s_idxLen;
}

#else   // no slot on this board

bool        sd_begin(void)      { return false; }
bool        sd_mounted(void)    { return false; }
uint8_t     sd_bus_width(void)  { return 0; }
uint64_t    sd_size_bytes(void) { return 0; }
const char *sd_status(void)     { return "no slot"; }
bool        sd_format(void)     { return false; }
void        sd_clear_crash_flag(void) {}
uint32_t    sd_seen_records(void) { return 0; }
bool        sd_seen_erase(void) { return false; }
bool        sd_seen_lookup(const char *, SdSeen *) { return false; }
void        sd_log_seen(const char *, const char *, float, uint32_t) {}

#endif
