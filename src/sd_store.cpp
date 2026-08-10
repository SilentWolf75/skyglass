#include "sd_store.h"
#include "config.h"
#include <string.h>
#include <stdio.h>

// Arduino.h lives inside the guard: the simulator builds this file too (ui.cpp asks the
// log for an aircraft's history) and there is no Arduino there, only the stubs below.
#if BOARD_HAS_SD
#include <Arduino.h>
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
#include <dirent.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define SD_MOUNT   "/sdcard"
#define SD_DIR     SD_MOUNT "/skyglass"
#define SD_SEEN    SD_DIR "/seen.dat"
// Photos live beside the log rather than in a photos/ subdirectory. The subdirectory
// looked fine -- mkdir returned, fopen inside it succeeded, fwrite wrote every byte --
// but stat() could not then see the file and opendir() listed nothing, so the cache
// could never find what it had just written. "p_" plus a six-digit hex is exactly eight
// characters, so these stay inside 8.3 and need no long-filename support.
#define SD_PHOTOS  SD_DIR

// A cap rather than a size limit: FAT directory scans get slower the more entries there
// are, and the eviction below has to walk the directory. 1000 photos at ~30 KB is about
// 30 MB, which is nothing on the cards these boards take, and keeps the scan quick.
#define SD_PHOTO_MAX 1000

static sdmmc_card_t *s_card = nullptr;
static bool     s_mounted = false;
static uint8_t  s_width   = 0;
static uint64_t s_size    = 0;
static char     s_status[64] = "not probed";
static esp_err_t s_lastErr = ESP_OK;
static int      s_freqKhz = 0;    // clock the link actually trained at
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
// Carries the address itself, not just a hash. Confirming a match used to mean reading
// the record back off the card on every single lookup -- thousands a minute -- and a
// freshly appended record is not yet visible to a new fopen(), so its own aircraft looked
// unknown and got appended again. Holding the hex in RAM makes the check exact, immune to
// write visibility, and free. 16 bytes an airframe: 320 KB of PSRAM at 20k, which the P4
// has in abundance and its internal heap does not.
struct IdxEnt {
    uint32_t h;
    uint32_t rec;
    char     hex[8];
    uint32_t lastWrite;    // epoch when this record was last pushed to the card
};

// How stale a record may get before it is written back. The card and the ESP32-C6 radio
// share one SDMMC controller (the C6 is an SDIO device on the other slot), and hammering
// it drowned the link: the driver logged "handle_idle_state_events unhandled" continuously
// and the feed went stale until the self-heal rebooted the board every few minutes. This
// used to do a read and a write per aircraft per poll -- around forty file operations a
// second, for data that only has to survive a power cut.
#define SD_WRITEBACK_S 120
static IdxEnt  *s_idx = nullptr;
static uint32_t s_idxLen = 0, s_idxCap = 0;
// Records in the file, tracked in RAM. Neither ftell() before the write nor stat()
// after it gave a usable record number on this FATFS -- the size lags the write, so
// every index entry pointed one record short and matched the previous aircraft. We are
// the only writer, so counting is exact and owes the filesystem nothing.
static uint32_t s_fileRecs = 0;

// One card, two tasks. The feed task writes the flight log and now caches photos; the
// UI task reads a contact history when its card opens. Left unsynchronised that wedged
// the board -- it kept answering ping while the LVGL loop sat blocked inside FATFS,
// which is the worst kind of hang because the device still looks alive. Every entry
// point that touches the card takes this first.
static SemaphoreHandle_t s_lock = nullptr;

struct SdLock {
    bool held;
    explicit SdLock(uint32_t waitMs = 4000)
        : held(s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(waitMs)) == pdTRUE) {}
    ~SdLock() { if (held) xSemaphoreGive(s_lock); }
};

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
static uint32_t rec_lookup(const char *hex, uint32_t *slot = nullptr) {
    if (!s_idx) return UINT32_MAX;
    const uint32_t h = hex_hash(hex);
    for (uint32_t i = idx_find_pos(h); i < s_idxLen && s_idx[i].h == h; ++i)
        if (strncmp(s_idx[i].hex, hex, sizeof(s_idx[i].hex)) == 0) {
            if (slot) *slot = i;
            return s_idx[i].rec;
        }
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
            memcpy(s_idx[s_idxLen].hex, buf[i].hex, sizeof(s_idx[s_idxLen].hex));
            s_idx[s_idxLen].lastWrite = buf[i].lastSeen;
            ++s_idxLen;
        }
    }
    fclose(f);
    s_fileRecs = rec;            // total records in the file, holes included
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
    // Width is the outer preference, clock the inner one. The other way round -- which is
    // how this was first written -- meant a 4-bit link that would not train at 40 MHz fell
    // straight to 1-bit at 40 MHz, when 4-bit at 20 MHz was available and strictly better.
    // That is exactly what happened: one boot came up 1-bit having never tried a slower
    // 4-bit. Exhaust every clock at four wires before giving up a wire.
    static const int kFreq[] = { SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_DEFAULT, SDMMC_FREQ_PROBING };
    for (int oneBit = 0; oneBit < 2 && !s_mounted; ++oneBit)
        for (int fi = 0; fi < 3 && !s_mounted; ++fi)
            if (mount_once(oneBit != 0, kFreq[fi], false)) {
                s_mounted = true; s_width = oneBit ? 1 : 4;
                s_freqKhz = kFreq[fi];
            }

    if (s_mounted) {
        s_size = s_card ? (uint64_t)s_card->csd.capacity * s_card->csd.sector_size : 0;
        snprintf(s_status, sizeof(s_status), "%u-bit %dMHz %llu MB",
                 (unsigned)s_width, s_freqKhz / 1000,
                 (unsigned long long)(s_size / (1024ULL * 1024ULL)));
        Serial.printf("[sd] mounted, %s\n", s_status);
        if (!s_lock) s_lock = xSemaphoreCreateMutex();
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
    if (rderr) *rderr = s_readErr + s_seekErr;
}

bool sd_format(void) {
    // Long wait on purpose: a format takes seconds and the feed task must not slip a log
    // write in halfway through. Missing this lock is what made pressing Format crash the
    // board and reboot it without formatting anything.
    SdLock lk(20000);
    if (!lk.held) { Serial.println("[sd] format: card busy"); return false; }
    Serial.println("[sd] formatting card");

    if (!s_mounted) {
        // A card this device cannot read is exactly when a format is wanted, so mount
        // with format_if_mount_failed and let that write a fresh filesystem.
        for (int oneBit = 0; oneBit < 2 && !s_mounted; ++oneBit)
            if (mount_once(oneBit != 0, SDMMC_FREQ_HIGHSPEED, true)) {
                s_mounted = true; s_width = oneBit ? 1 : 4; s_freqKhz = SDMMC_FREQ_HIGHSPEED;
            }
        if (!s_mounted) { Serial.println("[sd] format FAILED (no usable card)"); return false; }
        s_size = s_card ? (uint64_t)s_card->csd.capacity * s_card->csd.sector_size : 0;
        snprintf(s_status, sizeof(s_status), "%u-bit %dMHz %llu MB", (unsigned)s_width,
                 s_freqKhz / 1000, (unsigned long long)(s_size / (1024ULL * 1024ULL)));
        mkdir(SD_DIR, 0777);
        s_idxLen = 0; s_fileRecs = 0;
        Serial.println("[sd] format ok (via mount)");
        return true;
    }

    // Format the way we mounted -- by hand. esp_vfs_fat_sdcard_format() keeps its own
    // record of a mount made by esp_vfs_fat_sdmmc_mount(), which is not how this volume
    // was brought up (see mount_once), so it unwinds bookkeeping that does not exist.
    if (s_pdrv == 0xFF) { Serial.println("[sd] format FAILED (no drive)"); return false; }
    char drv[3] = { (char)('0' + s_pdrv), ':', 0 };

    void *work = heap_caps_malloc(FF_MAX_SS, MALLOC_CAP_SPIRAM);
    if (!work) work = malloc(FF_MAX_SS);
    if (!work) { Serial.println("[sd] format FAILED (no memory for the work buffer)"); return false; }

    f_mount(NULL, drv, 0);                       // drop the volume first
    MKFS_PARM opt = {};
    opt.fmt = FM_FAT32 | FM_SFD;                 // FAT32, no partition table
    opt.au_size = 32 * 1024;
    const FRESULT fr = f_mkfs(drv, &opt, work, FF_MAX_SS);
    free(work);

    const bool ok = (fr == FR_OK) && (f_mount(s_fs, drv, 1) == FR_OK);
    s_idxLen = 0;
    s_fileRecs = 0;
    if (ok) { mkdir(SD_DIR, 0777); idx_build(); }
    Serial.printf("[sd] format %s (f_mkfs=%d)\n", ok ? "ok" : "FAILED", (int)fr);
    return ok;
}

static bool is_icao_hex(const char *h);   // defined with the log below

// Oldest-first eviction, by modification time. Only runs when the cache is full, so the
// directory walk is rare; the alternative -- letting it grow forever -- eventually makes
// every directory operation slow and fills a small card.
static void photo_evict_one(void) {
    DIR *d = opendir(SD_PHOTOS);
    if (!d) return;
    char oldest[64] = "";
    time_t oldestT = 0;
    uint32_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        if (e->d_name[0] != 'p' && e->d_name[0] != 'P') continue;   // not a photo
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", SD_PHOTOS, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        ++n;
        if (!oldest[0] || st.st_mtime < oldestT) {
            oldestT = st.st_mtime;
            snprintf(oldest, sizeof(oldest), "%s", e->d_name);
        }
    }
    closedir(d);
    if (n < SD_PHOTO_MAX || !oldest[0]) return;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SD_PHOTOS, oldest);
    remove(path);
    // the credit sidecar goes with it
    char *dot = strrchr(path, '.');
    if (dot) { strcpy(dot, ".txt"); remove(path); }
}

bool sd_photo_save(const char *hex, const void *jpg, size_t len, const char *credit) {
    SdLock lk; if (!lk.held) return false;
    if (!s_mounted || !hex || !hex[0] || !jpg || !len) return false;
    if (!is_icao_hex(hex)) return false;
    photo_evict_one();

    char path[96];
    snprintf(path, sizeof(path), "%s/p_%s.jpg", SD_PHOTOS, hex);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const bool ok = (fwrite(jpg, 1, len, f) == len);
    fclose(f);
    if (!ok) { remove(path); return false; }

    // Attribution travels with the picture. Planespotters is free for non-commercial use
    // on the condition the photographer is credited, so a cached photo that lost its
    // credit would be a photo we are not allowed to show.
    if (credit && credit[0]) {
        snprintf(path, sizeof(path), "%s/p_%s.txt", SD_PHOTOS, hex);
        FILE *c = fopen(path, "wb");
        if (c) { fwrite(credit, 1, strlen(credit), c); fclose(c); }
    }
    return true;
}

bool sd_photo_load(const char *hex, unsigned char **jpg, size_t *len,
                   char *credit, size_t creditLen) {
    SdLock lk; if (!lk.held) return false;
    if (jpg) *jpg = nullptr;
    if (len) *len = 0;
    if (credit && creditLen) credit[0] = 0;
    if (!s_mounted || !hex || !hex[0] || !jpg || !len) return false;

    char path[96];
    snprintf(path, sizeof(path), "%s/p_%s.jpg", SD_PHOTOS, hex);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 512 * 1024) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    unsigned char *buf = (unsigned char *)heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return false; }
    const size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) { heap_caps_free(buf); return false; }

    if (credit && creditLen) {
        snprintf(path, sizeof(path), "%s/p_%s.txt", SD_PHOTOS, hex);
        FILE *c = fopen(path, "rb");
        if (c) {
            const size_t n = fread(credit, 1, creditLen - 1, c);
            credit[n] = 0;
            fclose(c);
        }
    }
    *jpg = buf;
    *len = got;
    return true;
}

void sd_photo_forget(const char *hex) {
    SdLock lk; if (!lk.held) return;
    if (!s_mounted || !hex || !hex[0]) return;
    char path[96];
    snprintf(path, sizeof(path), "%s/p_%s.jpg", SD_PHOTOS, hex);
    remove(path);
    snprintf(path, sizeof(path), "%s/p_%s.txt", SD_PHOTOS, hex);
    remove(path);
}

uint32_t sd_photo_count(void) {
    SdLock lk; if (!lk.held) return 0;
    if (!s_mounted) return 0;
    DIR *d = opendir(SD_PHOTOS);
    if (!d) return 0;
    uint32_t n = 0;
    struct dirent *e;
    // Case-insensitive: FATFS hands back uppercase 8.3 names when long filenames are
    // off, so a literal ".jpg" test counts nothing even with a full cache.
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        if (e->d_name[0] != 'p' && e->d_name[0] != 'P') continue;
        const char *dot = strrchr(e->d_name, '.');
        if (dot && (dot[1] | 32) == 'j' && (dot[2] | 32) == 'p' && (dot[3] | 32) == 'g') ++n;
    }
    closedir(d);
    return n;
}

bool sd_seen_erase(void) {
    SdLock lk; if (!lk.held) return false;
    if (!s_mounted) return false;
    remove(SD_SEEN);
    s_idxLen = 0;
    s_fileRecs = 0;
    return true;
}

bool sd_seen_lookup(const char *hex, SdSeen *out) {
    SdLock lk; if (!lk.held) return false;
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
    SdLock lk; if (!lk.held) return;
    if (!s_mounted || !hex || !hex[0]) return;
    if (!is_icao_hex(hex)) return;

    const uint16_t dam = (distKm > 0.0f && distKm < 650.0f)
                             ? (uint16_t)(distKm * 100.0f)   // km -> units of 10 m
                             : 0;
    uint32_t slot = UINT32_MAX;
    const uint32_t rec = rec_lookup(hex, &slot);

    if (rec != UINT32_MAX) {
        ++s_hits;
        // Between polls nothing meaningful changes: the same aircraft is simply still
        // overhead. Touch the card only when the record has gone stale or a new visit has
        // begun -- that is what keeps the SDMMC bus free for the radio sharing it.
        const bool newVisit = (nowEpoch && slot != UINT32_MAX && s_idx[slot].lastWrite &&
                               nowEpoch - s_idx[slot].lastWrite > SD_VISIT_GAP_S);
        const bool stale = (nowEpoch && slot != UINT32_MAX &&
                            nowEpoch - s_idx[slot].lastWrite >= SD_WRITEBACK_S);
        if (!newVisit && !stale) return;
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
        if (slot != UINT32_MAX && nowEpoch) s_idx[slot].lastWrite = nowEpoch;
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

    // Append with "ab", and take the record number from the file's size *after* the
    // write rather than from ftell() before it. The offset arithmetic was the bug: every
    // appended record failed to read back, so each contact was re-appended on every poll
    // while the file grew 32 bytes a time. stat() after close is the authoritative
    // answer and does not depend on how this FATFS treats seek and tell in append mode.
    const uint32_t newRec = s_fileRecs;      // where this one is about to land
    FILE *f = fopen(SD_SEEN, "ab");
    if (!f) return;
    const bool wrote = (fwrite(&r, sizeof(r), 1, f) == 1);
    fclose(f);
    if (!wrote) return;
    ++s_fileRecs;
    if (!idx_reserve(s_idxLen + 1)) return;

    const uint32_t h = hex_hash(r.hex);
    const uint32_t at = idx_find_pos(h);
    memmove(&s_idx[at + 1], &s_idx[at], (size_t)(s_idxLen - at) * sizeof(IdxEnt));
    s_idx[at].h = h; s_idx[at].rec = newRec;
    memcpy(s_idx[at].hex, r.hex, sizeof(s_idx[at].hex));
    s_idx[at].lastWrite = nowEpoch;
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
bool        sd_photo_save(const char *, const void *, size_t, const char *) { return false; }
bool        sd_photo_load(const char *, unsigned char **j, size_t *l, char *c, size_t cn) {
    if (j) *j = nullptr; if (l) *l = 0; if (c && cn) c[0] = 0; return false;
}
void        sd_photo_forget(const char *) {}
uint32_t    sd_photo_count(void) { return 0; }
void        sd_counters(uint32_t *h, uint32_t *a, uint32_t *r) {
    if (h) *h = 0; if (a) *a = 0; if (r) *r = 0;
}
uint32_t    sd_seen_records(void) { return 0; }
bool        sd_seen_erase(void) { return false; }
bool        sd_seen_lookup(const char *, SdSeen *) { return false; }
void        sd_log_seen(const char *, const char *, float, uint32_t) {}

#endif
