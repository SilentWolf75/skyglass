// MIPI-DSI panel bring-up for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C.
//
// Drives the panel through ESP-IDF's esp_lcd_mipi_dsi, which Arduino-ESP32 3.x exposes.
// (An earlier note here claimed Arduino_GFX has no DSI path. That was wrong -- the
// vendor demo ships a fork with Arduino_ESP32DSIPanel. Going straight to esp_lcd keeps
// this independent of that fork.) Structure follows the standard
// IDF sequence: power the PHY's LDO, create the DSI bus, create a DBI channel to send
// the panel's init commands, create the DPI video panel, then hand frames to LVGL.
//
// Brought up on real hardware: DSI reports up, backlight lights, GT9271 touch answers.
// Init sequence and video timings are transcribed from the
// vendor demo's own 4INCH-DSI configuration. See docs/PORT_P4.md.
#include "config.h"
#if defined(BOARD_WAVESHARE_P4_LCD_4C)

#include <Arduino.h>
#include <lvgl.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <driver/ledc.h>
#include <esp_heap_caps.h>
#include <esp_cache.h>
#include "display_p4.h"

static esp_lcd_dsi_bus_handle_t  s_bus   = nullptr;
static esp_lcd_panel_io_handle_t s_io    = nullptr;
static esp_lcd_panel_handle_t    s_panel = nullptr;
static esp_ldo_channel_handle_t  s_ldo   = nullptr;

#include "boards/p4_panel_init.h"   // 197 vendor init registers, transcribed verbatim

bool display_p4_begin(void) {
    // 0) Reset the panel before talking to it. The vendor passes lcd_rst to the driver,
    //    which pulses it; going straight to the init sequence leaves the controller in
    //    whatever state it powered up in.
    if (PIN_LCD_RST >= 0) {
        pinMode(PIN_LCD_RST, OUTPUT);
        digitalWrite(PIN_LCD_RST, HIGH);
        delay(10);
        digitalWrite(PIN_LCD_RST, LOW);
        delay(20);
        digitalWrite(PIN_LCD_RST, HIGH);
        delay(120);
    }

    // 1) The DSI PHY is fed by an internal LDO. Without this the whole chain reports
    //    success and the screen stays black -- the exact symptom reported by someone
    //    running this board under ESPHome.
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id    = DSI_LDO_CHANNEL;
    ldo_cfg.voltage_mv = DSI_LDO_MV;
    if (esp_ldo_acquire_channel(&ldo_cfg, &s_ldo) != ESP_OK) {
        Serial.println("[p4lcd] DSI PHY LDO acquire failed");
        return false;
    }

    // 2) DSI bus
    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id             = 0;
    bus_cfg.num_data_lanes     = DSI_LANES;
    bus_cfg.phy_clk_src        = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_cfg.lane_bit_rate_mbps = (uint32_t)(DSI_LANE_BITRATE_HZ / 1000000UL);
    if (esp_lcd_new_dsi_bus(&bus_cfg, &s_bus) != ESP_OK) {
        Serial.println("[p4lcd] dsi bus create failed");
        return false;
    }

    // 3) DBI channel: the low-speed path used only to push the init registers.
    esp_lcd_dbi_io_config_t dbi_cfg = {};
    dbi_cfg.virtual_channel = 0;
    dbi_cfg.lcd_cmd_bits    = 8;
    dbi_cfg.lcd_param_bits  = 8;
    if (esp_lcd_new_panel_io_dbi(s_bus, &dbi_cfg, &s_io) != ESP_OK) {
        Serial.println("[p4lcd] dbi io create failed");
        return false;
    }

    // 4) DPI video panel, timings from the board header (the vendor's own values).
    esp_lcd_dpi_panel_config_t dpi_cfg = {};
    dpi_cfg.virtual_channel     = 0;
    dpi_cfg.dpi_clk_src         = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz  = (uint32_t)(DSI_PCLK_HZ / 1000000UL);
    dpi_cfg.pixel_format        = LCD_COLOR_PIXEL_FORMAT_RGB565;
    // Also set the in/out formats. Leaving them zero left the DPI panel with an
    // unspecified format, which is one way to get a lit panel showing nothing.
    dpi_cfg.in_color_format     = LCD_COLOR_FMT_RGB565;
    dpi_cfg.out_color_format    = LCD_COLOR_FMT_RGB565;
    dpi_cfg.num_fbs             = 1;
    dpi_cfg.video_timing.h_size = SCREEN_W;
    dpi_cfg.video_timing.v_size = SCREEN_H;
    dpi_cfg.video_timing.hsync_pulse_width = DSI_HSYNC_PULSE;
    dpi_cfg.video_timing.hsync_front_porch = DSI_HSYNC_FRONT;
    dpi_cfg.video_timing.hsync_back_porch  = DSI_HSYNC_BACK;
    dpi_cfg.video_timing.vsync_pulse_width = DSI_VSYNC_PULSE;
    dpi_cfg.video_timing.vsync_front_porch = DSI_VSYNC_FRONT;
    dpi_cfg.video_timing.vsync_back_porch  = DSI_VSYNC_BACK;
    dpi_cfg.flags.use_dma2d = true;    // the P4's 2D accelerator does the blit
    if (esp_lcd_new_panel_dpi(s_bus, &dpi_cfg, &s_panel) != ESP_OK) {
        Serial.println("[p4lcd] dpi panel create failed");
        return false;
    }
    // Init registers go out AFTER the DPI panel exists. Sending them first -- the
    // intuitive order, and what this driver did initially -- left the panel lit but
    // blank. The vendor's Arduino_ESP32DSIPanel::begin() creates the DPI panel, then
    // pushes the vendor sequence, then calls panel_init; this mirrors that exactly.
    // Check every write. The vendor wraps each in ESP_ERROR_CHECK; silently ignoring
    // failures here means a panel that never gets configured and simply stays blank.
    int txFail = 0;
    for (size_t i = 0; i < sizeof(kPanelInit) / sizeof(kPanelInit[0]); ++i) {
        const uint8_t v = kPanelInit[i].val;
        const esp_err_t e = esp_lcd_panel_io_tx_param(s_io, kPanelInit[i].reg, &v, 1);
        if (e != ESP_OK) {
            if (txFail < 3) Serial.printf("[p4lcd] init[%u] reg 0x%02X failed: %s\n",
                                          (unsigned)i, kPanelInit[i].reg, esp_err_to_name(e));
            ++txFail;
        }
        if (kPanelInit[i].delay_ms) delay(kPanelInit[i].delay_ms);
    }
    Serial.printf("[p4lcd] init sequence: %u commands, %d failed\n",
                  (unsigned)(sizeof(kPanelInit) / sizeof(kPanelInit[0])), txFail);

    if (esp_lcd_panel_init(s_panel) != ESP_OK) {
        Serial.println("[p4lcd] panel init failed");
        return false;
    }

    display_p4_backlight(true);
    Serial.printf("[p4lcd] DSI up: %dx%d, %u lanes, %u Mbps\n",
                  SCREEN_W, SCREEN_H, (unsigned)DSI_LANES, (unsigned)bus_cfg.lane_bit_rate_mbps);
    return true;
}

void display_p4_backlight_level(uint8_t level) {
    if (PIN_LCD_BL < 0) return;
    static bool inited = false;
    if (!inited) {
        ledc_timer_config_t t = {};
        t.speed_mode      = LEDC_LOW_SPEED_MODE;
        t.duty_resolution = LEDC_TIMER_8_BIT;
        t.timer_num       = LEDC_TIMER_0;
        t.freq_hz         = 5000;
        t.clk_cfg         = LEDC_AUTO_CLK;
        ledc_timer_config(&t);
        ledc_channel_config_t c = {};
        c.gpio_num   = PIN_LCD_BL;
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel    = LEDC_CHANNEL_0;
        c.timer_sel  = LEDC_TIMER_0;
        c.duty       = 0;
        ledc_channel_config(&c);
        inited = true;
    }
    // ACTIVE LOW. The vendor test defines TEST_LCD_BK_LIGHT_ON_LEVEL (0), so driving the
    // pin high — the obvious reading of "on" — is what keeps the panel dark. That also
    // means the duty is inverted: full brightness is duty 0, off is duty 255.
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255 - (uint32_t)level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_p4_backlight(bool on) { display_p4_backlight_level(on ? 255 : 0); }

// Screenshots. The DPI panel's framebuffer *is* what the panel is scanning out, so a
// capture is just a read of it -- no separate mirror buffer needed as on the S3. The
// cache has to be invalidated first: LVGL's blits reach PSRAM via DMA, so the CPU's
// view of that memory is stale until told otherwise.
const uint16_t *display_p4_framebuffer(void) {
    if (!s_panel) return nullptr;
    void *fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) return nullptr;
    esp_cache_msync(fb, (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return (const uint16_t *)fb;
}

// LVGL flush. The DPI panel owns its framebuffer, so this is a straight blit; the
// area is inclusive on both ends in LVGL and exclusive at the end in esp_lcd.
void display_p4_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    if (s_panel) {
        esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                                  area->x2 + 1, area->y2 + 1, px);
    }
    lv_disp_flush_ready(drv);
}

#endif // BOARD_WAVESHARE_P4_LCD_4C
