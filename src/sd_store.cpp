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

#define SD_MOUNT   "/sdcard"
#define SD_DIR     SD_MOUNT "/skyglass"
#define SD_SEEN    SD_DIR "/seen.dat"

static sdmmc_card_t *s_card = nullptr;
static bool     s_mounted = false;
static uint8_t  s_width   = 0;
static uint64_t s_size    = 0;
static char     s_status[64] = "not probed";
static esp_err_t s_lastErr = ESP_OK;

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
    if (!f) return false;
    bool ok = (fseek(f, (long)rec * (long)sizeof(SeenRec), SEEK_SET) == 0) &&
              (fread(out, sizeof(SeenRec), 1, f) == 1);
    fclose(f);
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

static bool mount_once(bool oneBit, int freqKhz, bool formatIfFailed) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = freqKhz;

    // The bit that makes this work at all. On the P4 the SD bus IO rail is fed by an
    // on-chip LDO (channel 4) which is OFF at reset, so without this the card has no
    // signal voltage, never answers, and the mount fails with ESP_ERR_TIMEOUT -- which
    // is exactly what this board did. Same shape as the DSI PHY, which runs off internal
    // LDO channel 3. Taken from Waveshare's own BSP for this P4 family.
    static sd_pwr_ctrl_handle_t s_pwr = nullptr;
    if (!s_pwr) {
        sd_pwr_ctrl_ldo_config_t ldo = {};
        ldo.ldo_chan_id = 4;
        if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) {
            Serial.println("[sd] could not open the on-chip LDO for the SD rail");
            s_pwr = nullptr;
        }
    }
    // Attaching the handle is enough: the SDMMC driver sets the rail voltage through it
    // during init. The vendor BSP also calls set_io_voltage() by hand, but the driver
    // struct is opaque in this IDF, and that call is belt-and-braces rather than load
    // bearing -- the handle is what turns the LDO on.
    if (s_pwr) host.pwr_ctrl_handle = s_pwr;

    // Slot 0 is routed through the IO MUX, so the pins are fixed and are deliberately
    // NOT set here -- the vendor BSP does the same. PIN_SD_* stay in the board header as
    // documentation of where those signals physically go.
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.width = oneBit ? 1 : 4;
    slot.flags = 0;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {};
    cfg.format_if_mount_failed = formatIfFailed;   // only ever true for an explicit format
    cfg.max_files = 4;
    cfg.allocation_unit_size = 64 * 1024;

    s_lastErr = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &cfg, &s_card);
    if (s_lastErr == ESP_OK) return true;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
    s_card = nullptr;
    return false;
}

bool sd_begin(void) {
    if (s_mounted) return true;

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
        snprintf(s_status, sizeof(s_status), "not mounted: %s", esp_err_to_name(s_lastErr));
        Serial.printf("[sd] not mounted: %s\n", esp_err_to_name(s_lastErr));
    }
    return s_mounted;
}

bool        sd_mounted(void)    { return s_mounted; }
uint8_t     sd_bus_width(void)  { return s_width; }
uint64_t    sd_size_bytes(void) { return s_size; }
const char *sd_status(void)     { return s_status; }
uint32_t    sd_seen_records(void) { return s_idxLen; }

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

void sd_log_seen(const char *hex, const char *callsign, float distKm, uint32_t nowEpoch) {
    if (!s_mounted || !hex || !hex[0]) return;

    const uint16_t dam = (distKm > 0.0f && distKm < 650.0f)
                             ? (uint16_t)(distKm * 100.0f)   // km -> units of 10 m
                             : 0;
    const uint32_t rec = rec_lookup(hex);

    if (rec != UINT32_MAX) {
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
    SeenRec r = {};
    strncpy(r.hex, hex, sizeof(r.hex) - 1);
    if (callsign && callsign[0]) strncpy(r.call, callsign, sizeof(r.call) - 1);
    r.count = 1;
    r.firstSeen = nowEpoch;
    r.lastSeen = nowEpoch;
    r.closestDam = dam;

    FILE *f = fopen(SD_SEEN, "a+b");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
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
uint32_t    sd_seen_records(void) { return 0; }
bool        sd_seen_erase(void) { return false; }
bool        sd_seen_lookup(const char *, SdSeen *) { return false; }
void        sd_log_seen(const char *, const char *, float, uint32_t) {}

#endif
