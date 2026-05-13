/**
 * WP-5.4c — special-case image handling.
 *
 *  - SVG cover unwrap: when the cover is an `<image>`-wrapped SVG, extract
 *    the inner raster so we can run it through the normal pipeline.
 *  - Light-novel split: when `lightNovel === true` and an image is a
 *    landscape spread (aspect > 1.8), rotate / split it into two portrait
 *    halves and insert them as new manifest entries in the spine.
 *
 * Today the SVG unwrap is best-effort (handles the common cases); the
 * light-novel split rotates the image but defers the spine-and-manifest
 * surgery to a single helper so WP-5.5 (toc.ts) sees the new spine order.
 */
import type { EpubModel, ManifestItem } from "../lib/epub-model.js";
import type { OptimizeWarning, PixelPlane } from "../types.js";

const DECODER = new TextDecoder("utf-8");

/** Best-effort: pull the first <image> href out of an SVG envelope. */
export function extractSvgImageHref(svgBytes: Uint8Array): string | null {
  const text = DECODER.decode(svgBytes);
  const match = text.match(/<image\b[^>]*?(?:xlink:)?href\s*=\s*['"]([^'"]+)['"]/i);
  return match?.[1] ?? null;
}

export interface SplitResult {
  left: PixelPlane;
  right: PixelPlane;
}

/**
 * Split a landscape grayscale plane (aspect > 1.8) into two portrait halves.
 * Returns left + right ordered as they should appear in the spine.
 */
export function splitLandscape(plane: PixelPlane): SplitResult {
  if (plane.channels !== 1) {
    throw new Error(`splitLandscape requires a single-channel plane (got channels=${plane.channels})`);
  }
  const half = Math.floor(plane.width / 2);
  const left = new Uint8ClampedArray(half * plane.height);
  const right = new Uint8ClampedArray((plane.width - half) * plane.height);
  for (let y = 0; y < plane.height; y += 1) {
    for (let x = 0; x < half; x += 1) {
      left[y * half + x] = plane.data[y * plane.width + x]!;
    }
    for (let x = 0; x < plane.width - half; x += 1) {
      right[y * (plane.width - half) + x] = plane.data[y * plane.width + (x + half)]!;
    }
  }
  return {
    left: { data: left, width: half, height: plane.height, channels: 1 },
    right: { data: right, width: plane.width - half, height: plane.height, channels: 1 },
  };
}

/** Rotate a single-channel landscape plane 90° clockwise into a portrait. */
export function rotate90(plane: PixelPlane): PixelPlane {
  if (plane.channels !== 1) {
    throw new Error(`rotate90 requires a single-channel plane (got channels=${plane.channels})`);
  }
  const out = new Uint8ClampedArray(plane.data.length);
  for (let y = 0; y < plane.height; y += 1) {
    for (let x = 0; x < plane.width; x += 1) {
      out[x * plane.height + (plane.height - 1 - y)] = plane.data[y * plane.width + x]!;
    }
  }
  return { data: out, width: plane.height, height: plane.width, channels: 1 };
}

export interface InsertedImage {
  manifestItem: ManifestItem;
  bytes: Uint8Array;
}

/** Register a new image entry (manifest + entries map). */
export function registerInsertedImage(
  model: EpubModel,
  inserted: InsertedImage,
  warnings: OptimizeWarning[],
): void {
  if (model.manifest.has(inserted.manifestItem.id)) {
    warnings.push({
      step: "images",
      code: "IMG_ID_COLLISION",
      message: `Manifest id collision when inserting ${inserted.manifestItem.id}; skipped.`,
    });
    return;
  }
  model.manifest.set(inserted.manifestItem.id, inserted.manifestItem);
  const path = model.opfDir === ""
    ? inserted.manifestItem.href
    : `${model.opfDir}/${inserted.manifestItem.href}`;
  model.entries.set(path, inserted.bytes);
}
