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

// Sentinel for `mode` / `temp_c` parameters of epd_be_draw_grayscale_image:
// passing this value means "use the backend default" (MODE_GC16 / 25 °C
// respectively). Defined here so the public header does not need to pull
// in epdiy-internal headers (EpdDrawMode enum lives in epdiy.h).
#define EPD_BE_DEFAULT_MODE   (-1)
#define EPD_BE_DEFAULT_TEMP_C (-1000)

void    epd_be_init();
void    epd_be_poweron();
void    epd_be_poweroff();          // soft: rails off, panel latch retained
void    epd_be_poweroff_all();      // hard: full PMIC sleep before deep sleep
void    epd_be_clear();
void    epd_be_clear_area_cycles(EpdRect area, int32_t cycles, int32_t cycle_time);

// Draws a grayscale image into the panel's high-level framebuffer and pushes
// it out via the epdiy waveform driver.
//
//   mode    -- EpdDrawMode value (e.g. MODE_GC16, MODE_GL16, MODE_DU, ...).
//              Pass EPD_BE_DEFAULT_MODE to use MODE_GC16 (full 16-grey GC).
//              Mode is forwarded byte-for-byte to epd_hl_update_screen /
//              epd_hl_update_area; callers should pick the right one for
//              the surface (partial pages → MODE_GL16/MODE_DU; full frame
//              redraw → MODE_GC16).
//   temp_c  -- Ambient panel temperature in Celsius. Pass
//              EPD_BE_DEFAULT_TEMP_C to fall back to a hardcoded 25 °C.
//              Most callers should pass epd_be_get_ambient_temp_cached()
//              so waveforms compensate for real panel temperature.
//
// Returns 0 on success or a non-zero error code mirroring the underlying
// epdiy EpdDrawError bitfield (e.g. FAILED_ALLOC=0x10, MODE_NOT_FOUND=0x80,
// NO_PHASES_AVAILABLE=0x100). The header exposes the value as a raw `int`
// so callers do not need to include epdiy-internal headers; existing void-
// context callers stay source-compatible because the int return is
// discardable. Non-zero results are also logged to Serial inside the
// backend for diagnosis.
//
// The C++ default arguments below keep legacy two-argument call sites
// working until they migrate to explicit mode + temp (WP-0.2 in
// docs/partial_updates_plan.md). Default arguments are a C++-only feature,
// so the declaration with defaults lives outside the extern "C" block.
int     epd_be_draw_grayscale_image(EpdRect area, uint8_t *data,
                                    int mode, int temp_c);
EpdRect epd_be_full_screen();

// Returns ambient panel temperature in Celsius, cached for 2 s to amortize
// the underlying I2C read on the TPS65185. Falls back to the last good
// value on sensor failure; on cold boot before the first successful read,
// returns 25 °C.
int     epd_be_get_ambient_temp_cached();

#ifdef __cplusplus
}   // extern "C"

// C++-only overload of epd_be_draw_grayscale_image that supplies default
// arguments. extern "C" forbids C++ default arguments at the language
// level on some compilers, so the defaults are exposed here as a thin
// inline wrapper instead. ABI is unchanged — calls resolve to the same
// extern-C symbol above.
inline int epd_be_draw_grayscale_image(EpdRect area, uint8_t *data) {
    return epd_be_draw_grayscale_image(area, data,
                                       EPD_BE_DEFAULT_MODE,
                                       EPD_BE_DEFAULT_TEMP_C);
}
#endif
