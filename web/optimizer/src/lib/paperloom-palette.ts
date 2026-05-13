/**
 * 16-level grayscale palette for Paperloom (LilyGo T5 S3 Pro / Pro Lite).
 *
 * The firmware quantizes 8-bit gray to 4-bit via integer shift (gray >> 4),
 * see src/cover_renderer.cpp:95. The reverse mapping for display is
 * gray4 * 17, which lands on these exact 16 values. Use this as the target
 * palette for Floyd-Steinberg dithering so optimizer output survives the
 * firmware's on-device requantization losslessly.
 *
 * See .codewright/palette-lut.md for the full derivation.
 */
export const PAPERLOOM_GRAY_PALETTE: Uint8Array = new Uint8Array([
  0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187, 204, 221, 238, 255,
]);

/** Snap a single 8-bit gray value to the nearest palette level. */
export function snapToPalette(gray8: number, palette: Uint8Array = PAPERLOOM_GRAY_PALETTE): number {
  // Palette is linear with step 17 by construction; clamp + nearest-step.
  if (gray8 <= 0) return palette[0]!;
  if (gray8 >= 255) return palette[palette.length - 1]!;
  const idx = Math.min(palette.length - 1, Math.round((gray8 * (palette.length - 1)) / 255));
  return palette[idx]!;
}
