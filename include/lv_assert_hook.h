#pragma once
// Pulled in by LV_ASSERT_HANDLER_INCLUDE (see lv_conf.h). Kept as its own header because
// LVGL allows exactly one include there, and the handler needs both a flush and a reset.
//
// This is included from C as well as C++ -- the generated font tables are .c files -- so
// nothing here may be C++-only. That rules out Serial/ESP.restart(); the ESP-IDF calls
// below do the same job and compile in both languages.
#include <stdio.h>
#include <esp_system.h>
#include <esp_rom_sys.h>

// esp_rom_printf goes straight at the UART with no buffering and no heap, so it
// survives whatever state LVGL is in when it gives up. Plain printf did not: the
// board was resetting with nothing on the wire at all, which made an assert look
// identical to a spontaneous reboot.
