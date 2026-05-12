// src/gfx_font.cpp
//
// get_glyph() — extracted from LilyGo-EPD47/src/font.c (MIT). The function
// performs a linear search over UnicodeInterval ranges, returning the matching
// GFXglyph pointer or NULL.
#include "gfx_font.h"

void get_glyph(const GFXfont *font, uint32_t code_point, GFXglyph **glyph) {
    UnicodeInterval *intervals = font->intervals;
    *glyph = nullptr;
    for (uint32_t i = 0; i < font->interval_count; i++) {
        UnicodeInterval *interval = &intervals[i];
        if (code_point >= interval->first && code_point <= interval->last) {
            *glyph = &font->glyph[interval->offset + (code_point - interval->first)];
            return;
        }
        if (code_point < interval->first) {
            return;
        }
    }
}
