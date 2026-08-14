#pragma once
// MIPI-DSI panel for the ESP32-P4-WIFI6-Touch-LCD-4C. See display_p4.cpp.
#include "config.h"
#if defined(BOARD_WAVESHARE_P4_LCD_4C)
#include <lvgl.h>
bool display_p4_begin(void);                 // LDO -> DSI bus -> DBI init -> DPI panel
void display_p4_backlight(bool on);          // LEDC PWM on PIN_LCD_BL
// 0 = off, 255 = full. The panel is driven by a real duty cycle, so the idle dim and the
// brightness slider actually do something here rather than being on/off.
void display_p4_backlight_level(uint8_t level);
const uint16_t *display_p4_framebuffer(void);   // live DPI framebuffer, for /shot.bmp
void display_p4_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px);
#endif
