/**
 * Plane Radar 2.0 — LVGL v8.x configuration.
 * Reached via -DLV_CONF_INCLUDE_SIMPLE (LVGL does #include "lv_conf.h").
 * Only the settings we care about are listed; everything else falls back to the
 * library defaults in lv_conf_internal.h. Tuned for the CO5300 466x466 AMOLED.
 */
#if 1 /* Enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
/* 0: native RGB565 order — paired with gfx->draw16bitRGBBitmap() in the flush_cb.
   If colors look byte-swapped on hardware, set this to 1 and use draw16bitBeRGBBitmap(). */
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* Object/style pool (internal RAM). Big bitmap draw buffers are allocated
   separately in PSRAM in display.cpp. Bump this if the radar UI grows. */
#define LV_MEM_CUSTOM 0
/* LVGL allocates every widget, style and mask from this fixed pool, and running out is
 * not graceful -- the allocator asserts and spins, freezing whatever core LVGL runs on.
 * The 720x720 panel carries the same widget tree at larger sizes, so it gets more room. */
#if defined(BOARD_WAVESHARE_P4_LCD_4C)
#define LV_MEM_SIZE (112U * 1024U)
#elif !defined(ARDUINO)
/* Desktop simulator. The device budgets do not transfer: the host is 64-bit, so every
 * pointer inside lv_obj_t, the style lists and the timer/event chains doubles, and the
 * same widget tree overruns 64 KB before the first frame. Colour depth and draw buffers
 * are identical to the S3, so nothing pixel-visible changes -- only the object pool. */
#define LV_MEM_SIZE (1024U * 1024U)
#else
#define LV_MEM_SIZE (64U * 1024U)
#endif
#define LV_MEM_ADR 0
#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD 16   /* ms; ~60 Hz cap (SPI bandwidth is the real limit) */
#define LV_INDEV_DEF_READ_PERIOD 20  /* ms */

/* On the device, drive LVGL's tick from Arduino's millis() — no separate ticker.
   On the native SDL simulator there is no Arduino.h, so fall back to lv_tick_inc()
   (called from the sim main loop). */
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#  define LV_TICK_CUSTOM 1
#  define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#  define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#else
#  define LV_TICK_CUSTOM 0
#endif

#define LV_DPI_DEF 130

/*=======================
   FEATURE / DRAW CONFIG
 *=======================*/
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/*==================
   LOG (serial)
 *==================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/*==================
   ASSERTS / DEBUG
 *==================*/
/* LVGL's own default for LV_ASSERT_HANDLER is `while(1);` -- a silent hang. That is
   survivable on a board you can power-cycle, but in the CI simulator it is a job that
   spins until the runner times out with no output at all. Off-device, print the failing
   assert and abort so the log names it. Device builds keep LVGL's default. */
#if defined(ARDUINO)
/* On device LVGL's default handler is `while(1);`: the UI core stops dead, the display
   freezes on its last frame and the web server stops answering, with nothing on the wire
   to say why. That is exactly how the P4 lockup presented, and it is why that lockup is
   still unexplained. LVGL logs the failing assertion (LV_LOG_PRINTF) immediately before
   calling this, so flush that line out and reset: a board that restarts with a reason
   printed is diagnosable, a frozen one is not. Note it takes a reboot either way -- a
   frozen board needs the plug pulled. */
#define LV_ASSERT_HANDLER_INCLUDE "lv_assert_hook.h"
#define LV_ASSERT_HANDLER   do { esp_rom_printf("\n*** LVGL ASSERT -> restarting ***\n"); fflush(stdout); esp_restart(); } while (0);
#else
#define LV_ASSERT_HANDLER_INCLUDE <stdio.h>
/* LVGL logs the failing assert (LV_LOG_PRINTF) just before calling this, so the flush is
   what actually gets it out of the pipe buffer; __builtin_trap needs no further header. */
#define LV_ASSERT_HANDLER   do { fflush(stdout); __builtin_trap(); } while (0);
#endif
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0

/*==================
   FONTS
 *==================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1   /* large-text mode: 14 -> 18 */
#define LV_FONT_MONTSERRAT_20 1   /* large-text mode: 16 -> 20 */
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1   /* clock screen digits */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*==================
   WIDGETS
 *==================*/
/* Core widgets default to enabled in v8. Spinner lives in "extra" — enable it
   explicitly for the M0 hello screen. */
#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_SPINNER 1
#define LV_USE_LIST 1
#define LV_USE_TILEVIEW 1

#endif /* LV_CONF_H */
#endif /* Enable content */
