/**
 * WP-5.4b — main-thread encode path.
 *
 * Takes a PixelPlane (single-channel grayscale Uint8ClampedArray) and turns
 * it into JPEG bytes. Two strategies:
 *
 *   1) WebCodecs ImageEncoder when available (best chroma control).
 *   2) Canvas2D toBlob('image/jpeg', q) fallback for Safari ≤ 17.
 *
 * The worker (WP-6) calls the same encode helpers from its own thread; the
 * code lives here so behaviour stays identical regardless of which path
 * `resolveImagePath` picked.
 */
import type { PixelPlane } from "../types.js";
import { floydSteinberg } from "../lib/floyd-steinberg.js";
import { PAPERLOOM_GRAY_PALETTE } from "../lib/paperloom-palette.js";

export interface EncodeArgs {
  plane: PixelPlane;
  palette?: Uint8Array;
  jpegQuality: number;
  dither: "floyd-steinberg" | "none";
}

export async function encodePlaneToJpeg(args: EncodeArgs): Promise<Uint8Array> {
  const { plane, jpegQuality, dither } = args;
  const palette = args.palette ?? PAPERLOOM_GRAY_PALETTE;
  const dithered =
    dither === "floyd-steinberg"
      ? floydSteinberg(plane.data, plane.width, plane.height, palette)
      : plane.data;

  // Promote grayscale to RGBA for Canvas2D / ImageEncoder.
  const rgba = grayscaleToRgba(dithered);

  const blob = await encodeAsJpeg(rgba, plane.width, plane.height, jpegQuality);
  const buf = await blob.arrayBuffer();
  return new Uint8Array(buf);
}

function grayscaleToRgba(src: Uint8ClampedArray): Uint8ClampedArray {
  const out = new Uint8ClampedArray(src.length * 4);
  for (let i = 0; i < src.length; i += 1) {
    const v = src[i]!;
    const j = i * 4;
    out[j] = v;
    out[j + 1] = v;
    out[j + 2] = v;
    out[j + 3] = 255;
  }
  return out;
}

async function encodeAsJpeg(
  rgba: Uint8ClampedArray,
  width: number,
  height: number,
  quality: number,
): Promise<Blob> {
  const q = clamp01(quality / 100);
  // Prefer OffscreenCanvas when available (works in workers too).
  if (typeof OffscreenCanvas !== "undefined") {
    const canvas = new OffscreenCanvas(width, height);
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("OffscreenCanvas 2d context unavailable");
    const image = new ImageData(rgba, width, height);
    ctx.putImageData(image, 0, 0);
    return canvas.convertToBlob({ type: "image/jpeg", quality: q });
  }
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new Error("Canvas2D context unavailable");
  const image = new ImageData(rgba, width, height);
  ctx.putImageData(image, 0, 0);
  return new Promise<Blob>((resolve, reject) => {
    canvas.toBlob(
      (blob) => {
        if (blob) resolve(blob);
        else reject(new Error("canvas.toBlob returned null"));
      },
      "image/jpeg",
      q,
    );
  });
}

function clamp01(n: number): number {
  if (Number.isNaN(n)) return 0.8;
  if (n < 0) return 0;
  if (n > 1) return 1;
  return n;
}
