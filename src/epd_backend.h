// src/epd_backend.h
//
// Thin abstraction over the EPD driver. display.cpp depends only on this header,
// allowing the underlying library to be swapped (LilyGo-EPD47 → vroland/epdiy)
// without touching the rendering / rotation / font code in display.cpp.
//
// The `epd_be_*` prefix avoids symbol-name collisions with both LilyGo-EPD47
// and upstream-epdiy, both of which export `epd_*` symbols at link time.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} EpdRect;

void    epd_be_init();
void    epd_be_poweron();
void    epd_be_poweroff();          // soft: rails off, panel latch retained
void    epd_be_poweroff_all();      // hard: full PMIC sleep before deep sleep
void    epd_be_clear();
void    epd_be_clear_area_cycles(EpdRect area, int32_t cycles, int32_t cycle_time);
void    epd_be_draw_grayscale_image(EpdRect area, uint8_t *data);
EpdRect epd_be_full_screen();

#ifdef __cplusplus
}
#endif
