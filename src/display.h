#pragma once

#include <Arduino.h>
#include "config.h"  // FONT_FAMILY_COUNT, FONT_SIZE_LEVEL_COUNT

void display_init();
void display_clear();
void display_fill_screen(uint8_t gray4);
void display_draw_text(int x, int y, const char* text, uint8_t fg_color = 0);
void display_draw_pixel(int x, int y, uint8_t gray4);
void display_draw_filled_rect(int x, int y, int w, int h, uint8_t gray4);
void display_draw_hline(int x, int y, int w, uint8_t gray4);
void display_draw_vline(int x, int y, int h, uint8_t gray4);
void display_draw_rect(int x, int y, int w, int h, uint8_t gray4);
void display_update();               // full refresh (heavy clear + draw, ~3s, 6 cycles)
void display_update_sleep();         // full refresh for sleep image; preserves panel hold state
void display_update_medium();        // medium refresh for chapter jumps (~1s, 2 cycles)
void display_update_fast();          // lighter full-screen refresh for page turns (1 cycle)
void display_update_reader_body(int x, int y, int w, int h, bool strongCleanup = false);
void display_update_partial();        // partial update (no clear, no flash)
void display_update_mode(bool fullRefresh);  // select mode
int  display_text_width(const char* text);
int  display_font_height();
int  display_font_ascender();
int  display_width();
int  display_height();
// Font family identifiers — keep in sync with settings.h's fontFamily and
// the static font lookup tables in display.cpp.  Stored as uint8_t in
// settings so the on-disk schema stays compact.  FONT_FAMILY_COUNT lives
// in include/config.h so non-display code (settings.cpp) can clamp it
// without pulling in this header.
enum FontFamily : uint8_t {
    FONT_FAMILY_SANS  = 0,  // Lexend Deca — research-backed sans
    FONT_FAMILY_SERIF = 1,  // Literata — Google's screen-optimized serif
    FONT_FAMILY_SLAB  = 2,  // Bitter — slab serif tuned for e-ink
    FONT_FAMILY_INTER = 3,  // Inter — also used for UI chrome; available
                            // here so readers can pick a single-font device.
    FONT_FAMILY_CHAREINK = 4,  // ChareInk7SP — SIL Charis derivative tuned
                               // for e-ink legibility (high contrast strokes).
};

// Display names for each family.  Defined in display.cpp; declared here so
// UI code can label dropdowns without duplicating the string table.
extern const char* const kFontFamilyNames[FONT_FAMILY_COUNT];

void display_set_font_size(int sizeLevel);             // 0-4: XS,S,M,ML,L (defaults to Sans family)
void display_set_font(int sizeLevel, uint8_t family);  // sizeLevel + FontFamily index
void display_power_off();             // ensure EPD power rail is off
