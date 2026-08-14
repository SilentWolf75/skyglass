// Board shims for the ESP32-P4-WIFI6-Touch-LCD-4C.
//
// main.cpp is shared across boards and calls two things this board does not have in the
// same shape:
//
//   * the display:: namespace, which on the S3 wraps Arduino_GFX over QSPI. Here the
//     panel is MIPI-DSI and lives in display_p4.cpp, so these forward to it.
//   * QMI8658 IMU, PCF85063 RTC and AXP2101 PMIC, none of which are fitted. Rather than
//     scatter #ifdefs through main.cpp, they resolve to honest no-ops: no motion, no
//     battery, no RTC. The BOARD_HAS_* flags say which, so behaviour degrades visibly
//     (the HUD hides the battery, the clock waits for NTP) instead of reporting nonsense.
#include "config.h"
#if defined(BOARD_WAVESHARE_P4_LCD_4C)

#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include "display.h"
#include "display_p4.h"
#include "touch_gt911.h"
#include "ui.h"
#include <esp_heap_caps.h>

namespace display {

// Mirrors what display.cpp does on the S3. Bringing up the DSI panel alone was not
// enough: main.cpp expects display::begin() to leave LVGL initialised, a display driver
// registered, touch attached and the UI built. Without that the panel powers on and
// stays black, which is exactly what the first flash did.
static lv_disp_draw_buf_t s_dbuf;
static lv_disp_drv_t      s_ddrv;
static lv_indev_drv_t     s_idrv;
static lv_color_t        *s_buf = nullptr;

bool begin() {
    if (!display_p4_begin()) {
        Serial.println("[p4] DSI bring-up failed");
        return false;
    }

    lv_init();
    const size_t px = (size_t)SCREEN_W * LVGL_BUF_LINES;
    // Internal DMA RAM, not PSRAM. LVGL alpha-blends into this buffer pixel by pixel, and
    // this board has 512 KB of internal RAM largely unused -- unlike the S3, where the
    // same buffer competes with mbedTLS for a scarce contiguous block.
    s_buf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_buf) {
        Serial.println("[p4] internal draw buffer failed; falling back to PSRAM");
        s_buf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_buf) { Serial.println("[p4] draw buffer alloc failed"); return false; }
    lv_disp_draw_buf_init(&s_dbuf, s_buf, nullptr, px);

    lv_disp_drv_init(&s_ddrv);
    s_ddrv.hor_res  = SCREEN_W;
    s_ddrv.ver_res  = SCREEN_H;
    s_ddrv.flush_cb = display_p4_flush;
    s_ddrv.draw_buf = &s_dbuf;
    lv_disp_drv_register(&s_ddrv);

    // Also starts the I2C bus. It has to happen here, before main.cpp probes the codec
    // and RTC -- nothing else calls Wire.begin() on this board.
    if (touch_gt911_begin()) {
        lv_indev_drv_init(&s_idrv);
        s_idrv.type    = LV_INDEV_TYPE_POINTER;
        s_idrv.read_cb = touch_gt911_lvgl_read;
        lv_indev_drv_register(&s_idrv);
    }

    Serial.printf("[p4] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    ui_create();
    Serial.println("[p4] LVGL ready");
    return true;
}

void loop() { lv_timer_handler(); }

// Backlight is a PWM output here rather than a panel command, so brightness is the LEDC
// duty cycle. This used to collapse to on/off, which meant the brightness slider did
// nothing and the idle dim looked broken: the timer fired and applyBrightness() ran, but
// every non-zero level came out as full brightness.
void setBrightness(uint8_t v) {
    // Zero is a real "off" and must pass through; everything else is lifted to the
    // lowest level this backlight will actually light at.
    if (v > 0 && v < BACKLIGHT_MIN_ON) v = BACKLIGHT_MIN_ON;
    display_p4_backlight_level(v);
}

// Rotation is not implemented for DSI yet: the S3 path rotates in the flush callback by
// transposing blocks, which does not apply to a panel that owns its own framebuffer.
// Report 0 rather than pretend, so the config page shows the truth.
void setRotation(uint16_t) {}
uint16_t rotation() { return 0; }

// The DPI panel owns a full framebuffer, so /shot.bmp can read it back directly --
// simpler than the S3, which has to mirror flushes into a separate buffer.
const uint16_t *captureFrame() { return display_p4_framebuffer(); }

uint32_t inactiveMs() { return lv_disp_get_inactive_time(nullptr); }

} // namespace display

// ---- peripherals this board does not carry -------------------------------------
#if !BOARD_HAS_IMU
bool imu_begin()    { return false; }
int  imu_facedown() { return -1; }   // -1 = unavailable, so the caller leaves state alone
#endif

#if !BOARD_HAS_PMIC
bool battery_begin()    { return false; }
bool battery_present()  { return false; }
bool battery_charging() { return false; }
int  battery_percent()  { return -1; }   // -1 = unknown; the HUD hides the indicator
// On the S3 this switches ALDO1 on for the ES8311's analog rail. The 4C has no PMIC,
// so the codec rail is either always on or switched elsewhere -- nothing to do here.
void battery_enable_codec_rail() {}
#endif

#if !BOARD_HAS_RTC
bool rtc_begin()             { return false; }
bool rtc_write(const tm *)   { return false; }
bool rtc_read(tm *)          { return false; }
#endif

#endif // BOARD_WAVESHARE_P4_LCD_4C
