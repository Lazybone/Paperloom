/**
 * Dedicated worker entry point for off-main-thread image processing.
 *
 * The main thread posts an EncodeRequest with the raw image bytes; the
 * worker decodes, resizes, autocontrasts, dithers, and JPEG-encodes, then
 * posts an EncodeResponse back. Transferable buffers in both directions
 * keep the heap clean.
 *
 * Worker scope has no DOM — only WorkerGlobalScope. We rely on
 * createImageBitmap + OffscreenCanvas, both of which are available on every
 * target browser (Chrome / Edge / Opera ≥ 119). The 2x2 fallback matrix
 * (lib/image-path.ts) ensures the worker is only used when OffscreenCanvas
 * is present, so we don't need a main-thread Canvas fallback inside the
 * worker itself.
 */
import { autocontrastGrayscale, contrastBoost } from "./lib/autocontrast.js";
import { encodePlaneToJpeg } from "./pipeline/images-encode.js";
import type { EncodeRequest, EncodeResponse, EncodeResponseErr, EncodeResponseOk } from "./lib/worker-protocol.js";
import type { PixelPlane } from "./types.js";

declare const self: DedicatedWorkerGlobalScope;

self.addEventListener("message", async (ev: MessageEvent<EncodeRequest>) => {
  const req = ev.data;
  try {
    const result = await handle(req);
    self.postMessage(result, { transfer: [result.bytes] });
  } catch (err) {
    const errResp: EncodeResponseErr = {
      id: req.id,
      ok: false,
      error: (err as Error).message,
      errorKind: "internal",
    };
    self.postMessage(errResp);
  }
});

async function handle(req: EncodeRequest): Promise<EncodeResponseOk> {
  const blob = new Blob([new Uint8Array(req.bytes)], { type: req.srcMime });
  let bitmap: ImageBitmap;
  try {
    bitmap = await createImageBitmap(blob);
  } catch (err) {
    throw new DecodeError(`createImageBitmap failed: ${(err as Error).message}`);
  }

  const [w, h] = fitIntoBox(bitmap.width, bitmap.height, req.maxWidth, req.maxHeight);
  const canvas = new OffscreenCanvas(w, h);
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new Error("worker OffscreenCanvas 2d context unavailable");
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(bitmap, 0, 0, w, h);
  const rgba = ctx.getImageData(0, 0, w, h).data;
  const gray = rgbaToGrayscale(rgba);

  const ac = autocontrastGrayscale(gray);
  const boosted = req.contrastBoost ? contrastBoost(ac.data, 1.5) : ac.data;
  const plane: PixelPlane = { data: boosted, width: w, height: h, channels: 1 };

  const bytes = await encodePlaneToJpeg({
    plane,
    palette: req.palette,
    jpegQuality: req.jpegQuality,
    dither: req.dither,
  });
  const buffer = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
  return {
    id: req.id,
    ok: true,
    bytes: buffer as ArrayBuffer,
    width: w,
    height: h,
    outMime: "image/jpeg",
  };
}

class DecodeError extends Error {
  override name = "DecodeError";
}

function rgbaToGrayscale(rgba: Uint8ClampedArray): Uint8ClampedArray {
  const out = new Uint8ClampedArray(rgba.length / 4);
  for (let i = 0, j = 0; i < rgba.length; i += 4, j += 1) {
    const r = rgba[i]!;
    const g = rgba[i + 1]!;
    const b = rgba[i + 2]!;
    const a = rgba[i + 3]! / 255;
    const inv = 1 - a;
    out[j] = Math.round(a * (0.2126 * r + 0.7152 * g + 0.0722 * b) + inv * 255);
  }
  return out;
}

function fitIntoBox(w: number, h: number, maxW: number, maxH: number): [number, number] {
  const r = Math.min(maxW / w, maxH / h, 1);
  return [Math.max(1, Math.round(w * r)), Math.max(1, Math.round(h * r))];
}

// Re-export for type assertion in TypeScript callers that import the module
// path (only happens in tests; production loads the worker via `new Worker(url)`).
export type { EncodeRequest, EncodeResponse };
