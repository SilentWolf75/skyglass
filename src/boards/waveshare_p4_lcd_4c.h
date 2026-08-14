#pragma once
// Waveshare ESP32-P4-WIFI6-Touch-LCD-4C — 4" round IPS, 720x720, MIPI-DSI.
//
// STATUS: running on hardware. Display, touch (GT9271), audio codec, WiFi via the C6,
// the ADS-B feed, the web config page and OTA are all confirmed working. See
// docs/PORT_P4.md.
//
// Hardware, from the vendor wiki (waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-4C):
//   ESP32-P4NRW32   dual-core RISC-V (HP) + 40 MHz RISC-V (LP)
//   32 MB PSRAM in package, 32 MB NOR flash over QSPI
//   4" round IPS 720x720, MIPI-DSI 2-lane
//   ESP32-C6-MINI-1 Wi-Fi 6 / BT5 co-processor, attached over SDIO -- the P4 itself
//     has NO radio. Networking goes through esp_hosted, not the native Wi-Fi stack.
//   MIPI-CSI 2-lane camera header, SDIO 3.0 TF slot, ES8311 audio codec
//
// Provenance is tagged per value:
//   [VENDOR]    from ESP32-P4-WIFI6-Touch-LCD-XC-Demo.zip (Arduino/libraries/displays)
//   [COMMUNITY] from an ESPHome config for this board; plausible, not vendor-confirmed
// Per CLAUDE.md nothing here is guessed: it is sourced or it stays -1.

// mDNS name and setup AP, named after the chip so the address says which board
// you are looking at. Both used to answer to skyglass.local, which meant only
// one was reachable when two were powered on.
#define BOARD_HOSTNAME      "skyglass-p4"
#define BOARD_SETUP_AP      "SkyGlass-P4-Setup"
#define BOARD_NAME          "Waveshare ESP32-P4-WIFI6-Touch-LCD-4C"
#define BOARD_PANEL_QSPI    0
#define BOARD_PANEL_DSI     1        // esp_lcd_mipi_dsi, NOT Arduino_GFX

// ---------- Screen geometry ----------
// 720x720 is 2.4x the pixels of the 1.75" board. Radar radius keeps the same proportion
// of the panel (218/466 = 0.468) so the scope fills the round bezel identically.
#define SCREEN_W            720
#define SCREEN_H            720
#define SCREEN_CX           360
#define SCREEN_CY           360
#define RADAR_R_OUTER_PX    337
// Weather image. At the S3's 360 it filled only the middle half of this panel; the full
// fetched tile fills the face and still leaves a band top and bottom for the chrome.
#define WX_RADAR_SIZE       720   // full bleed: the panel width
#define WX_RADAR_SOURCE_SIZE 768  // fetched a little larger, then centre-cropped
// The full two-hour RainViewer history: 13 frames x 450 KB = 5.9 MB of 25 MB free.
#define WX_RADAR_FRAMES     13
#define LCD_COL_OFFSET      0
#define LCD_ROW_OFFSET      0

// ---------- Shared I2C ----------
// Confirmed in the vendor wiki: "sets the GPIO number of the serial clock bus (SCL),
// which is 8" / "the serial data bus (SDA), which is 7". External pullups are fitted,
// so internal pullups stay off.
#define PIN_I2C_SDA         7
#define PIN_I2C_SCL         8

// ---------- Panel / touch ----------
// MIPI-DSI needs no databus GPIOs (the PHY is dedicated). All of these are [VENDOR],
// cross-confirmed between displays_config.h and the ESP-IDF panel test in the demo.
#define PIN_LCD_RST         27             // [VENDOR] displays_config.h .lcd_rst
#define PIN_LCD_BL          26             // [VENDOR] TEST_PIN_NUM_BK_LIGHT
#define PIN_TP_INT          -1             // [VENDOR] GPIO_NUM_NC - not connected
#define PIN_TP_RST          23             // [VENDOR] gt911.h EXAMPLE_PIN_NUM_TOUCH_RST
// [VENDOR] gt911.cpp sets swap_xy = 0, mirror_x = 0, mirror_y = 0 for this panel.
#define TP_MIRROR_X         false
#define TP_MIRROR_Y         false
#define TP_SWAP_XY          false
#define I2C_ADDR_TOUCH      0x5D           // [VENDOR] GT911 default; 0x14 if INT high at reset
#define I2C_ADDR_TOUCH_ALT  0x14
#define I2C_CLOCK_HZ        100000         // [VENDOR] .i2c_clock_speed - NOT 400 kHz

// MIPI-DSI panel timing. [VENDOR] -- these are the demo's own 4INCH-DSI values, taken
// from displays_config.h. An earlier note here called them suspect on the grounds that
// the implied refresh looked high for a 4" panel; that reasoning was wrong. They match
// the vendor exactly.
#define DSI_LANES           2
#define DSI_LANE_BITRATE_HZ 1500000000UL
#define DSI_PCLK_HZ         80000000UL
#define DSI_HSYNC_PULSE     20
#define DSI_HSYNC_FRONT     40
#define DSI_HSYNC_BACK      20
#define DSI_VSYNC_PULSE     4
#define DSI_VSYNC_FRONT     24
#define DSI_VSYNC_BACK      12
// The DSI PHY is powered from an internal LDO that must be switched on explicitly.
// Forgetting it is the classic "everything logs fine, screen stays black" failure.
// [VENDOR] TEST_MIPI_DSI_PHY_PWR_LDO_CHAN / _VOLTAGE_MV.
#define DSI_LDO_CHANNEL     3
#define DSI_LDO_MV          2500

// ---------- ES8311 codec over I2S ----------
// [COMMUNITY] reported working on this board.
#define PIN_I2S_MCLK        13
#define PIN_I2S_BCLK        12
#define PIN_I2S_LRCLK       10
#define PIN_I2S_DOUT        9              // ESP32 -> codec (speaker)
#define PIN_I2S_DIN         11             // codec -> ESP32 (mics)
#define PIN_AUDIO_PA        53             // BSP_POWER_AMP_IO
#define I2C_ADDR_AUDIO      0x18
#define PIN_BOOT_BUTTON     -1

// ---------- GNSS over UART (optional add-on) ----------
// A plain NMEA module wired to the 40-pin expansion header (J2). GPIO20 and GPIO21 are
// brought out there and are claimed by nothing else on this board: the panel is MIPI-DSI
// (no GPIOs), touch and the codec sit on 7/8 and 9-13, the ESP32-C6 link uses 14-19, the
// CH343P debug UART is 37/38 and the audio PA is 53.
// [VENDOR] ESP32-P4-WIFI6-Touch-LCD-XC schematic, connector J2.
#define PIN_GPS_RX          20             // P4 input  <- module TXD
#define PIN_GPS_TX          21             // P4 output -> module RXD (unused unless we configure it)
#define GPS_UART_NUM        1              // UART0 is the debug console
#define GPS_BAUD            9600           // GAM-1818B-GKBD default, 8N1

// ---------- microSD (SDMMC slot 0) ----------
// Not a board choice: the P4 multiplexes SDMMC_HOST_SLOT_0 onto GPIO39-48 through the
// IO MUX, so these signals cannot be routed anywhere else (ESP32-P4 datasheet, "SD/MMC
// Host Controller > Pin Assignment"). Free on this board -- touch is 7/8, the codec
// 9-13, the C6 link 14-19, GPS 20/21, the debug UART 37/38 and the audio PA 53.
// [VENDOR] ESP32-P4-WIFI6-Touch-LCD-XC schematic, MicroSD Card block (SD1).
#define BOARD_HAS_SD        1
#define PIN_SD_CLK          43
#define PIN_SD_CMD          44
#define PIN_SD_D0           39
#define PIN_SD_D1           40
#define PIN_SD_D2           41
#define PIN_SD_D3           42
// Card rail via an AO3401 P-FET, gate pulled up so the card is off at reset. Driving it
// low powers the card. The FET is a board choice rather than a chip constraint, so
// sd_begin() tries both polarities before giving up.
#define PIN_SD_PWR          45

// ---------- Peripherals present ----------
// No IMU, RTC or PMIC on this board: face-down sleep, battery reporting and the
// pre-WiFi clock all need to degrade gracefully rather than be assumed.
#define BOARD_HAS_IMU       0
#define BOARD_HAS_RTC       0
#define BOARD_HAS_PMIC      0
#define BOARD_HAS_AUDIO     1
// WiFiManager references CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER, which only exists on a
// chip with a native PHY. The P4 has none (the C6 is the radio), so it cannot build
// there and the captive portal needs a different mechanism.
#define BOARD_HAS_WIFIMANAGER 0

// LVGL draw buffer height. A taller buffer was tried on the theory that fewer, larger
// chunks would cut per-chunk overhead: 160 lines made no difference at all -- 85 ms a
// frame either way -- so the cost is pixel work, not chunk setup. Left at 40.
//
// The sweep tick is left at the shared default too. A finer 12 ms tick was tried on the
// assumption this panel renders in ~3-5 ms; measuring frame_ms showed ~85 ms, so asking
// for 12 only invites LVGL to run the timer more than once per rendered frame.
#define LVGL_BUF_LINES      40
// Trail length left at the default 55 deg. Shortening it to 36 made frames ~25% cheaper
// and looked worse: the trail works as motion blur, and cutting it exposed each step.
//
// 17.6 px at this rim is the same angle the board ran before -- 3 deg. Reducing it to
// 11 px gave smoother arithmetic and a slower, more obviously stepped sweep.
#define SWEEP_MAX_STEP_PX   17.6f
#define I2C_ADDR_IMU        -1
#define I2C_ADDR_RTC        -1
#define I2C_ADDR_PMIC       -1

// Networking is via the C6 over SDIO (esp_hosted). [VENDOR] The demo's own Arduino
// example (GFX_ESPWiFiAnalyzer.ino) uses plain WiFi.h -- WiFi.mode/scanNetworks -- with
// no hosted-specific init, so the standard API is expected to work unchanged. The flag
// stays for code that may still want to know there is no native radio.
#define BOARD_WIFI_HOSTED   1

#if (PIN_I2C_SDA < 0) || (PIN_I2C_SCL < 0)
#  error "board: I2C pins are placeholders (-1). Take them from the Waveshare demo."
#endif
