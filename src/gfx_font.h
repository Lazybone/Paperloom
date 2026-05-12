// src/gfx_font.h
//
// GFXfont/GFXglyph types and get_glyph() — extracted from LilyGo-EPD47/src/font.c
// (originally MIT-licensed, see LICENSE in upstream repo). Vendored locally so that
// font_*.h headers no longer depend on the EPD library.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  width;
    uint8_t  height;
    uint8_t  advance_x;
    int16_t  left;
    int16_t  top;
    uint16_t compressed_size;
    uint32_t data_offset;
} GFXglyph;

typedef struct {
    uint32_t first;
    uint32_t last;
    uint32_t offset;
} UnicodeInterval;

typedef struct {
    uint8_t          *bitmap;
    GFXglyph         *glyph;
    UnicodeInterval  *intervals;
    uint32_t          interval_count;
    bool              compressed;
    uint8_t           advance_y;
    int32_t           ascender;
    int32_t           descender;
} GFXfont;

void get_glyph(const GFXfont *font, uint32_t code_point, GFXglyph **glyph);

#ifdef __cplusplus
}
#endif
