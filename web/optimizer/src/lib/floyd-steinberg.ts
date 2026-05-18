/**
 * Floyd-Steinberg dithering against a small palette.
 *
 * Standard 4-neighbour error-diffusion kernel:
 *
 *           current   7/16
 *   3/16     5/16     1/16
 *
 * Quantizes a grayscale plane to the supplied palette (default = 16-level
 * Paperloom palette). Returns a new Uint8ClampedArray with palette values.
 */
import { PAPERLOOM_GRAY_PALETTE, snapToPalette } from "./paperloom-palette.js";

export function floydSteinberg(
  src: Uint8ClampedArray,
  width: number,
  height: number,
  palette: Uint8Array = PAPERLOOM_GRAY_PALETTE,
): Uint8ClampedArray {
  // Work in Float32 so accumulated error doesn't truncate prematurely.
  const buf = new Float32Array(src.length);
  for (let i = 0; i < src.length; i += 1) buf[i] = src[i]!;

  const out = new Uint8ClampedArray(src.length);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const idx = y * width + x;
      const old = buf[idx]!;
      const snapped = snapToPalette(clamp(old), palette);
      out[idx] = snapped;
      const err = old - snapped;
      diffuse(buf, x + 1, y, width, height, err, 7 / 16);
      diffuse(buf, x - 1, y + 1, width, height, err, 3 / 16);
      diffuse(buf, x, y + 1, width, height, err, 5 / 16);
      diffuse(buf, x + 1, y + 1, width, height, err, 1 / 16);
    }
  }
  return out;
}

function clamp(v: number): number {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return v;
}

function diffuse(
  buf: Float32Array,
  x: number,
  y: number,
  width: number,
  height: number,
  err: number,
  weight: number,
): void {
  if (x < 0 || x >= width || y < 0 || y >= height) return;
  buf[y * width + x] += err * weight;
}
