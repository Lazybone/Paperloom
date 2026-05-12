#!/usr/bin/env python3
# tools/fontconvert.py — Generate GFXfont C header from a TTF/OTF file.
#
# Adapted from epdiy/scripts/fontconvert.py.  The original emits epdiy's
# `EpdFont` / `EpdGlyph` types and includes "epdiy.h".  This variant emits
# the project-local `GFXfont` / `GFXglyph` / `UnicodeInterval` types defined
# in src/gfx_font.h, with a single `compressed_size` per glyph (uint16) and a
# `data_offset` (uint32).  Compressed glyph bitmaps are required (compressed
# flag is forced to 1) to match the runtime decoder.
#
# Usage:
#   python3 tools/fontconvert.py <FontName> <size_pt> <font.ttf> > out.h
#
# Default Unicode coverage: 0x20-0x7E (basic Latin) + 0xA0-0xFF (Latin-1
# Supplement).  Override with --intervals "0x20-0x7E,0xA0-0xFF,0x2010-0x205F".
#
# DPI is fixed at 150 to match the existing font headers in src/font_*.h.
#
# Output structure matches the GFXfont layout in src/gfx_font.h:
#   bitmap (uint8_t*) + glyph (GFXglyph*) + intervals (UnicodeInterval*)
#   interval_count (uint32_t) + compressed (bool) + advance_y (uint8_t)
#   ascender (int32_t) + descender (int32_t)
#
# Field order in GFXglyph: width, height, advance_x, left, top,
# compressed_size, data_offset.  This is verified at runtime by gfx_font.h.

import argparse
import math
import os
import sys
import zlib
from collections import namedtuple

try:
    import freetype
except ImportError:
    sys.exit("freetype-py required: pip install freetype-py")

DPI = 150  # matches existing src/font_*.h files

DEFAULT_INTERVALS = [
    (0x20, 0x7E),   # Basic Latin
    (0xA0, 0xFF),   # Latin-1 Supplement (incl. ä ö ü ß Ä Ö Ü and accents)
]

GlyphProps = namedtuple(
    "GlyphProps",
    ["width", "height", "advance_x", "left", "top",
     "compressed_size", "data_offset", "code_point"],
)


def parse_intervals(arg):
    out = []
    for chunk in arg.split(","):
        chunk = chunk.strip()
        if "-" in chunk:
            lo, hi = chunk.split("-", 1)
        else:
            lo = hi = chunk
        out.append((int(lo, 0), int(hi, 0)))
    out.sort()
    # Sanity: no overlap, ascending.
    for i in range(1, len(out)):
        if out[i][0] <= out[i - 1][1]:
            sys.exit(f"intervals must not overlap: {out[i - 1]} and {out[i]}")
        if out[i][0] > out[i][1]:
            sys.exit(f"invalid interval (lo>hi): {out[i]}")
    return out


def norm_floor(val):
    return int(math.floor(val / (1 << 6)))


def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))


def chunks(seq, n):
    for i in range(0, len(seq), n):
        yield seq[i:i + n]


def pack_bitmap_4bpp(bitmap):
    """Pack an 8-bit grayscale FreeType bitmap into 4bpp little-nibble first."""
    pixels = []
    px = 0
    for i, v in enumerate(bitmap.buffer):
        x = i % bitmap.width
        if x % 2 == 0:
            px = (v >> 4)
        else:
            px |= (v & 0xF0)
            pixels.append(px)
            px = 0
        if x == bitmap.width - 1 and bitmap.width % 2 > 0:
            pixels.append(px)
            px = 0
    return bytes(pixels)


def main():
    ap = argparse.ArgumentParser(description="Generate GFXfont header from TTF.")
    ap.add_argument("name", help="Font symbol name (PascalCase), e.g. LexendM")
    ap.add_argument("size", type=int, help="Font size in points (rendered at 150 DPI)")
    ap.add_argument("font", help="Path to TTF/OTF file")
    ap.add_argument(
        "--intervals",
        default=None,
        help='Override Unicode intervals, e.g. "0x20-0x7E,0xA0-0xFF". '
             'Default: Basic Latin + Latin-1 Supplement.',
    )
    args = ap.parse_args()

    intervals = parse_intervals(args.intervals) if args.intervals else list(DEFAULT_INTERVALS)
    face = freetype.Face(args.font)
    face.set_char_size(args.size << 6, args.size << 6, DPI, DPI)

    all_glyphs = []
    total_size = 0
    total_packed = 0
    total_chars = 0
    ascender_max = 0
    descender_min = 0
    height_max = 0
    missing = []

    # Walk requested intervals; emit only code points present in the font.
    for lo, hi in intervals:
        for cp in range(lo, hi + 1):
            if face.get_char_index(cp) == 0:
                missing.append(cp)
                continue
            face.load_glyph(face.get_char_index(cp), freetype.FT_LOAD_RENDER)
            if ascender_max < face.size.ascender:
                ascender_max = face.size.ascender
            if descender_min > face.size.descender:
                descender_min = face.size.descender
            if height_max < face.size.height:
                height_max = face.size.height
            total_chars += 1
            bmp = face.glyph.bitmap
            packed = pack_bitmap_4bpp(bmp)
            total_packed += len(packed)
            compressed = zlib.compress(packed)
            advance_x_px = norm_floor(face.glyph.advance.x)
            if advance_x_px < 0 or advance_x_px > 0xFF:
                sys.exit(
                    f"advance_x out of uint8_t range for codepoint {cp:#x}: "
                    f"{advance_x_px}px (size={args.size}pt)"
                )
            if len(compressed) > 0xFFFF:
                sys.exit(
                    f"compressed glyph too large for uint16_t: codepoint {cp:#x}, "
                    f"{len(compressed)} bytes"
                )
            # data_offset is stored in a uint32_t GFXglyph field.  Python ints
            # are unbounded so a runaway interval list could silently emit a
            # >4 GiB literal that the C compiler would then truncate.  Refuse
            # at this layer instead of relying on the toolchain to catch it.
            if total_size + len(compressed) > 0xFFFFFFFF:
                sys.exit(
                    f"bitmap blob exceeds uint32_t range: would reach "
                    f"{total_size + len(compressed)} bytes"
                )
            glyph = GlyphProps(
                width=bmp.width,
                height=bmp.rows,
                advance_x=advance_x_px,
                left=face.glyph.bitmap_left,
                top=face.glyph.bitmap_top,
                compressed_size=len(compressed),
                data_offset=total_size,
                code_point=cp,
            )
            total_size += len(compressed)
            all_glyphs.append((glyph, compressed))

    if not all_glyphs:
        sys.exit(f"no glyphs rendered for font {args.font}")

    # Rebuild interval list to drop codepoints the font did not provide,
    # so the on-device binary-search table is dense and accurate.
    rendered = sorted(g.code_point for g, _ in all_glyphs)
    runs = []
    run_lo = run_hi = rendered[0]
    for cp in rendered[1:]:
        if cp == run_hi + 1:
            run_hi = cp
        else:
            runs.append((run_lo, run_hi))
            run_lo = run_hi = cp
    runs.append((run_lo, run_hi))

    # Flatten bitmap blob.
    blob = bytearray()
    for _, comp in all_glyphs:
        blob.extend(comp)

    # Header emit.  Strip the source path down to a basename so the
    # generated file does not leak the build-machine's absolute filesystem
    # layout into committed source.
    print("// Auto-generated by tools/fontconvert.py — do not edit by hand.")
    print(f"// Source: {os.path.basename(args.font)}")
    print(f"// Size: {args.size}pt @ {DPI} DPI, {total_chars} glyphs, "
          f"{total_size} bytes (raw {total_packed}, "
          f"saved {(1 - total_size/total_packed)*100:.1f}%)")
    print("#pragma once")
    print('#include "gfx_font.h"')
    print()
    print(f"const uint8_t {args.name}Bitmaps[{len(blob)}] = {{")
    for row in chunks(list(blob), 16):
        print("    " + " ".join(f"0x{b:02X}," for b in row))
    print("};")
    print()
    print(f"const GFXglyph {args.name}Glyphs[] = {{")
    for g, _ in all_glyphs:
        ch = chr(g.code_point) if 0x20 <= g.code_point < 0x7F and g.code_point != ord('\\') else f"U+{g.code_point:04X}"
        print(f"    {{ {g.width}, {g.height}, {g.advance_x}, "
              f"{g.left}, {g.top}, {g.compressed_size}, {g.data_offset} }}, // '{ch}'")
    print("};")
    print()
    print(f"const UnicodeInterval {args.name}Intervals[] = {{")
    offset = 0
    for lo, hi in runs:
        print(f"    {{ 0x{lo:X}, 0x{hi:X}, 0x{offset:X} }},")
        offset += hi - lo + 1
    print("};")
    print()
    print(f"const GFXfont {args.name} = {{")
    print(f"    (uint8_t*){args.name}Bitmaps,")
    print(f"    (GFXglyph*){args.name}Glyphs,")
    print(f"    (UnicodeInterval*){args.name}Intervals,")
    print(f"    {len(runs)},              // interval_count")
    print(f"    1,                        // compressed (always zlib)")
    print(f"    {norm_ceil(height_max)},  // advance_y")
    print(f"    {norm_ceil(ascender_max)},  // ascender")
    print(f"    {norm_floor(descender_min)},  // descender")
    print("};")

    if missing:
        sys.stderr.write(
            f"warning: {len(missing)} requested codepoints not in font "
            f"(first 8: {', '.join(f'0x{cp:X}' for cp in missing[:8])})\n"
        )


if __name__ == "__main__":
    main()
