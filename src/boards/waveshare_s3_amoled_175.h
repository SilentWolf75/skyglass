#pragma once
// Waveshare ESP32-S3-Touch-AMOLED-1.75 — the original target board.
//
// ESP32-S3R8, 8 MB PSRAM, 16 MB flash. CO5300 AMOLED 466x466 over QSPI, CST9217 touch,
// QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, ES8311 audio.
//
// Values here were taken from the Waveshare board definition and a working Arduino_GFX
// port, never guessed. See docs/HARDWARE.md.

// mDNS name and setup AP, named after the chip so the address says which board
// you are looking at. Both used to answer to skyglass.local, which meant only
// one was reachable when two were powered on.
#define BOARD_HOSTNAME      "skyglass-s3"
#define BOARD_SETUP_AP      "SkyGlass-S3-Setup"
#define BOARD_NAME          "Waveshare ESP32-S3-Touch-AMOLED-1.75"
#define BOARD_PANEL_QSPI    1        // Arduino_GFX + CO5300 over QSPI
#define BOARD_PANEL_DSI     0

// ---------- Screen geometry ----------
#define SCREEN_W            466
#define SCREEN_H            466
#define SCREEN_CX           233
#define SCREEN_CY           233
#define RADAR_R_OUTER_PX    218            // outer ring radius in pixels
// Weather image (a centre crop of the 512 px source tile). At 360 the header and the
// source line had nowhere to go but on top of the imagery; 320 leaves a band at each end.
#define WX_RADAR_SIZE       466   // full bleed: the panel width
// One hour of loop. 6 frames x 200 KB = 1.2 MB; this board has ~5 MB of PSRAM left and
// shares it with the photo cache and the map tiles, so the whole 13 would be greedy.
#define WX_RADAR_FRAMES     6
// Twelve rows, not the default twenty-four. Measured on this board: visiting the list
// took the LVGL pool from 25.2 KB free to 14.7 KB -- 10.5 KB, which is 24 rows at the
// ~437 bytes each costs, and the single largest consumer in the whole UI. Twelve is
// still more than the panel shows at once, and it hands back five kilobytes of a pool
// that was sitting at 79% used with the assert handler wired to reboot.
//
// The pool itself cannot simply be grown: it is a fixed slice of internal RAM, and this
// board only has ~32 KB contiguous left for mbedTLS as it is.
#define UI_LIST_MAX_ROWS    12
#define LCD_COL_OFFSET      6              // CO5300 column (x) gap on this panel (esp_lcd set_gap 0x06)
#define LCD_ROW_OFFSET      0              // no row (y) gap
#define LCD_QSPI_HZ         80000000       // CO5300 QSPI clock (vendor uses 40 MHz)

// ---------- Panel / touch ----------
#define PIN_LCD_CS          12
#define PIN_LCD_RST         39
#define PIN_TP_INT          11
#define PIN_TP_RST          40
#define TP_MIRROR_X         true
#define TP_MIRROR_Y         true

#define PIN_LCD_SCLK        38             // QSPI PCLK
#define PIN_LCD_D0          4
#define PIN_LCD_D1          5
#define PIN_LCD_D2          6
#define PIN_LCD_D3          7

// ---------- Shared I2C (touch + IMU + RTC + PMIC + audio codec) ----------
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         14

// ---------- ES8311 codec over I2S ----------
#define PIN_I2S_MCLK        42
#define PIN_I2S_BCLK        9
#define PIN_I2S_LRCLK       45             // a.k.a. WS
#define PIN_I2S_DOUT        8              // ESP32 -> codec (speaker)
#define PIN_I2S_DIN         10             // codec -> ESP32 (mics)
#define PIN_AUDIO_PA        46             // speaker amp enable
#define PIN_BOOT_BUTTON     0              // BOOT button (held on boot = captive portal)

// ---------- I2C addresses ----------
#define I2C_ADDR_TOUCH      0x5A           // CST9217 (corrected from vendor driver; was 0x15)
#define I2C_ADDR_IMU        0x6B
#define I2C_ADDR_RTC        0x51
#define I2C_ADDR_PMIC       0x34

// ---------- Peripherals present ----------
#define BOARD_HAS_IMU       1
#define BOARD_HAS_RTC       1
#define BOARD_HAS_PMIC      1
#define BOARD_HAS_SD        0     // no card slot on this board
#define BOARD_HAS_AUDIO     1
// WiFiManager references CONFIG_ESP32_PHY_MAX_WIFI_TX_POWER, which only exists on a
// chip with a native PHY. The P4 has none (the C6 is the radio), so it cannot build
// there and the captive portal needs a different mechanism.
#define BOARD_HAS_WIFIMANAGER 1

// Safety net: should never fire now that pins are filled in. Keeps future edits honest.
#if (PIN_LCD_SCLK < 0) || (PIN_I2C_SDA < 0)
#  error "board: QSPI/I2C pins are back to placeholders (-1). Restore the real values."
#endif
