/**
 * WP-5.4a — image pipeline orchestrator (decode + resize + autocontrast).
 *
 * For every raster image in the manifest:
 *   1. Decode (createImageBitmap; SVG-wrapped covers extract the inner raster).
 *   2. Resize to fit within the target dimensions, preserving aspect.
 *   3. Convert to single-channel grayscale.
 *   4. Apply autocontrast (Pillow-parity, lib/autocontrast).
 *   5. Apply 1.5x contrast boost when `opts.features.contrastBoost`.
 *   6. Hand the PixelPlane to images-encode (WP-5.4b) for dither + JPEG.
 *   7. (Light-novel mode) split landscape spreads via images-special (WP-5.4c).
 *
 * Per-image decode / encode errors are caught and downgraded to warnings;
 * the original bytes stay in place so the EPUB still re-zips successfully.
 */
import type { EpubModel } from "../lib/epub-model.js";
import { resolveHref } from "../lib/epub-model.js";
import type { OptimizeOptions, OptimizeWarning, PixelPlane } from "../types.js";
import { ImageDecodeError } from "../errors.js";
import { autocontrastGrayscale, contrastBoost } from "../lib/autocontrast.js";
import { encodePlaneToJpeg } from "./images-encode.js";
import { extractSvgImageHref, rotate90, splitLandscape, registerInsertedImage } from "./images-special.js";

const IMAGE_MIME_PATTERN = /^image\/(jpe?g|png|gif|webp|svg\+xml)$/i;
const TARGET_W = 960;
const TARGET_H = 540;
const HARD_CAP = 1024;
const LIGHT_NOVEL_RATIO = 1.8;

export interface ImageStats {
  processed: number;
  skipped: number;
}

export async function processImages(
  model: EpubModel,
  opts: OptimizeOptions,
  warnings: OptimizeWarning[],
  reportProgress?: (pct: number, detail: string) => void,
): Promise<ImageStats> {
  const imageItems = Array.from(model.manifest.values()).filter((item) =>
    isImage(item.mediaType, item.href),
  );
  let processed = 0;
  let skipped = 0;

  for (let i = 0; i < imageItems.length; i += 1) {
    const item = imageItems[i]!;
    const path = resolveHref(model, item.href);
    const bytes = model.entries.get(path);
    if (!bytes) {
      skipped += 1;
      continue;
    }
    reportProgress?.(
      Math.round(((i + 1) / imageItems.length) * 100),
      `image ${i + 1} / ${imageItems.length}`,
    );
    try {
      const resolvedBytes = await resolveSvgIfNeeded(model, item.mediaType, bytes);
      if (!resolvedBytes) {
        warnings.push({
          step: "images",
          code: "IMG_SVG_UNREADABLE",
          message: `Could not extract inner raster from ${item.href}; keeping original SVG.`,
        });
        skipped += 1;
        continue;
      }

      const decoded = await decode(resolvedBytes);
      const sized = await resize(decoded, TARGET_W, TARGET_H, HARD_CAP);
      const gray = toGrayscale(sized);
      const ac = autocontrastGrayscale(gray.data);
      const plane: PixelPlane = {
        data: opts.features.contrastBoost ? contrastBoost(ac.data, 1.5) : ac.data,
        width: gray.width,
        height: gray.height,
        channels: 1,
      };

      if (opts.features.lightNovel && plane.width / plane.height > LIGHT_NOVEL_RATIO) {
        const { left, right } = splitLandscape(plane);
        const leftBytes = await encodePlaneToJpeg({ plane: rotate90(left), jpegQuality: opts.jpegQuality, dither: opts.dither });
        const rightBytes = await encodePlaneToJpeg({ plane: rotate90(right), jpegQuality: opts.jpegQuality, dither: opts.dither });
        // Replace original with the left half; insert the right half as a sibling.
        model.entries.set(path, leftBytes);
        registerInsertedImage(
          model,
          {
            manifestItem: {
              id: `${item.id}-pt2`,
              href: appendSuffix(item.href, "-pt2"),
              mediaType: "image/jpeg",
            },
            bytes: rightBytes,
          },
          warnings,
        );
        processed += 2;
      } else {
        const encoded = await encodePlaneToJpeg({ plane, jpegQuality: opts.jpegQuality, dither: opts.dither });
        model.entries.set(path, encoded);
        // Update manifest media type since we always emit JPEG.
        if (item.mediaType.toLowerCase() !== "image/jpeg") {
          item.mediaType = "image/jpeg";
        }
        processed += 1;
      }
    } catch (err) {
      if (err instanceof ImageDecodeError) {
        warnings.push({ step: "images", code: "IMG_DECODE", message: err.message });
      } else {
        warnings.push({
          step: "images",
          code: "IMG_ENCODE",
          message: `Image ${item.href}: ${(err as Error).message}; original kept.`,
        });
      }
      skipped += 1;
    }
  }

  if (opts.features.generateCover && !model.metadata.coverHref) {
    // Cover synthesis is intentionally minimal in v0.3.0: a single off-white
    // canvas with the title + author in body type. Better than nothing.
    try {
      const coverBytes = await synthesizeCover(model.metadata.title, model.metadata.author, opts.jpegQuality);
      const href = "generated-cover.jpg";
      registerInsertedImage(
        model,
        {
          manifestItem: { id: "generated-cover", href, mediaType: "image/jpeg" },
          bytes: coverBytes,
        },
        warnings,
      );
      model.metadata.coverHref = href;
      warnings.push({ step: "images", code: "COVER_GENERATED", message: "No cover found; generated a minimal title-card." });
    } catch (err) {
      warnings.push({ step: "images", code: "COVER_GENERATE_FAILED", message: (err as Error).message });
    }
  }

  return { processed, skipped };
}

interface RasterDecoded {
  width: number;
  height: number;
  bitmap: ImageBitmap | HTMLImageElement;
}

interface RasterSized {
  width: number;
  height: number;
  rgba: Uint8ClampedArray;
}

async function decode(bytes: Uint8Array): Promise<RasterDecoded> {
  try {
    const blob = new Blob([new Uint8Array(bytes)]);
    if (typeof createImageBitmap !== "undefined") {
      const bitmap = await createImageBitmap(blob);
      return { width: bitmap.width, height: bitmap.height, bitmap };
    }
    // Browser fallback.
    return await new Promise((resolve, reject) => {
      const url = URL.createObjectURL(blob);
      const img = new Image();
      img.onload = () => {
        URL.revokeObjectURL(url);
        resolve({ width: img.naturalWidth, height: img.naturalHeight, bitmap: img });
      };
      img.onerror = () => {
        URL.revokeObjectURL(url);
        reject(new ImageDecodeError("image element failed to load"));
      };
      img.src = url;
    });
  } catch (err) {
    throw new ImageDecodeError(`decode failed: ${(err as Error).message}`);
  }
}

async function resize(
  src: RasterDecoded,
  maxW: number,
  maxH: number,
  hardCap: number,
): Promise<RasterSized> {
  const [w, h] = fitIntoBox(src.width, src.height, maxW, maxH, hardCap);
  if (typeof OffscreenCanvas !== "undefined") {
    const canvas = new OffscreenCanvas(w, h);
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("OffscreenCanvas 2d context unavailable");
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(src.bitmap as CanvasImageSource, 0, 0, w, h);
    const data = ctx.getImageData(0, 0, w, h);
    return { width: w, height: h, rgba: data.data };
  }
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new Error("Canvas2D context unavailable");
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(src.bitmap as CanvasImageSource, 0, 0, w, h);
  const data = ctx.getImageData(0, 0, w, h);
  return { width: w, height: h, rgba: data.data };
}

function toGrayscale(sized: RasterSized): { width: number; height: number; data: Uint8ClampedArray } {
  const out = new Uint8ClampedArray(sized.width * sized.height);
  for (let i = 0, j = 0; i < sized.rgba.length; i += 4, j += 1) {
    // Rec. 709 luminance weighting.
    const r = sized.rgba[i]!;
    const g = sized.rgba[i + 1]!;
    const b = sized.rgba[i + 2]!;
    const a = sized.rgba[i + 3]! / 255;
    // Composite onto white when alpha < 1 so transparent regions don't go black.
    const inv = 1 - a;
    out[j] = Math.round(a * (0.2126 * r + 0.7152 * g + 0.0722 * b) + inv * 255);
  }
  return { width: sized.width, height: sized.height, data: out };
}

function fitIntoBox(w: number, h: number, maxW: number, maxH: number, hardCap: number): [number, number] {
  const r = Math.min(maxW / w, maxH / h, hardCap / Math.max(w, h), 1);
  return [Math.max(1, Math.round(w * r)), Math.max(1, Math.round(h * r))];
}

async function resolveSvgIfNeeded(
  model: EpubModel,
  mediaType: string,
  bytes: Uint8Array,
): Promise<Uint8Array | null> {
  if (!/svg/i.test(mediaType)) return bytes;
  const innerHref = extractSvgImageHref(bytes);
  if (!innerHref) return null;
  // Look up by basename — the SVG's xlink:href is typically relative.
  const wanted = innerHref.replace(/^[./]+/, "");
  for (const [path, data] of model.entries) {
    if (path.endsWith(wanted)) return data;
  }
  return null;
}

function appendSuffix(href: string, suffix: string): string {
  const dot = href.lastIndexOf(".");
  if (dot <= 0) return `${href}${suffix}`;
  return `${href.slice(0, dot)}${suffix}.jpg`;
}

async function synthesizeCover(title: string, author: string, jpegQuality: number): Promise<Uint8Array> {
  const width = 600;
  const height = 900;
  const canvas =
    typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(width, height) : document.createElement("canvas");
  if ("width" in canvas) {
    (canvas as HTMLCanvasElement).width = width;
    (canvas as HTMLCanvasElement).height = height;
  }
  const ctx = (canvas as OffscreenCanvas).getContext
    ? ((canvas as OffscreenCanvas).getContext("2d") as OffscreenCanvasRenderingContext2D | null)
    : ((canvas as HTMLCanvasElement).getContext("2d") as CanvasRenderingContext2D | null);
  if (!ctx) throw new Error("cover canvas 2d context unavailable");
  ctx.fillStyle = "#f4ede0";
  ctx.fillRect(0, 0, width, height);
  ctx.fillStyle = "#1a1714";
  ctx.font = "bold 56px serif";
  ctx.textBaseline = "top";
  wrapText(ctx as CanvasRenderingContext2D, title || "Untitled", 48, 120, width - 96, 64);
  ctx.font = "italic 30px serif";
  wrapText(ctx as CanvasRenderingContext2D, author || "Unknown", 48, 760, width - 96, 36);

  let blob: Blob;
  if ("convertToBlob" in canvas) {
    blob = await (canvas as OffscreenCanvas).convertToBlob({ type: "image/jpeg", quality: jpegQuality / 100 });
  } else {
    blob = await new Promise<Blob>((resolve, reject) => {
      (canvas as HTMLCanvasElement).toBlob(
        (b) => (b ? resolve(b) : reject(new Error("toBlob null"))),
        "image/jpeg",
        jpegQuality / 100,
      );
    });
  }
  return new Uint8Array(await blob.arrayBuffer());
}

function wrapText(
  ctx: CanvasRenderingContext2D,
  text: string,
  x: number,
  y: number,
  maxWidth: number,
  lineHeight: number,
): void {
  const words = text.split(/\s+/);
  let line = "";
  let cy = y;
  for (const w of words) {
    const tentative = line ? `${line} ${w}` : w;
    if (ctx.measureText(tentative).width > maxWidth && line) {
      ctx.fillText(line, x, cy);
      line = w;
      cy += lineHeight;
    } else {
      line = tentative;
    }
  }
  if (line) ctx.fillText(line, x, cy);
}

function isImage(mediaType: string, href: string): boolean {
  if (IMAGE_MIME_PATTERN.test(mediaType)) return true;
  return /\.(jpe?g|png|gif|webp|svg)$/i.test(href);
}
