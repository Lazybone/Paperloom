/**
 * Public entry point of the Paperloom EPUB optimizer.
 *
 * The orchestrator unzips the input EPUB once via fflate, builds an EpubModel,
 * runs the 10 pipeline steps in PIPELINE_STEPS order, and emits throttled
 * progress events to the UI. Errors are funneled through pipeline/* modules
 * and surfaced via the central errors.ts catalogue.
 */

import { unzipSync, zipSync } from "fflate";
import type { EpubModel } from "./lib/epub-model.js";
import { PAPERLOOM_GRAY_PALETTE } from "./lib/paperloom-palette.js";
import { sanitizeFilename } from "./lib/filename-sanitize.js";
import {
  PIPELINE_STEPS,
  type OptimizeOptions,
  type OptimizeResult,
  type OptimizeWarning,
  type ProgressCallback,
} from "./types.js";
import {
  DrmDetectedError,
  ImageDecodeError,
  InvalidEpubError,
  MalformedZipError,
  OptimizerInternalError,
  ZipSlipDetectedError,
} from "./errors.js";
import { guardZipEntries } from "./pipeline/zip-guard.js";
import { detectDrm } from "./pipeline/drm.js";
import { parseStructure } from "./pipeline/structure.js";
import { applyMetadata } from "./pipeline/metadata.js";
import { processImages } from "./pipeline/images.js";
import { cleanHtml } from "./pipeline/html.js";
import { pruneCss } from "./pipeline/css.js";
import { stripFonts } from "./pipeline/fonts.js";
import { cleanText } from "./pipeline/text.js";
import { repairToc } from "./pipeline/toc.js";
import { purgeArtifacts } from "./pipeline/artifacts.js";
import { repackage } from "./pipeline/package.js";

/** Re-export the public surface so consumers only import from `index`. */
export type {
  OptimizeOptions,
  OptimizeResult,
  OptimizeWarning,
  ProgressEvent,
  ProgressCallback,
  PixelPlane,
  PipelineStep,
  Preset,
} from "./types.js";
export { PIPELINE_STEPS } from "./types.js";
export { PAPERLOOM_GRAY_PALETTE } from "./lib/paperloom-palette.js";
export {
  DrmDetectedError,
  InvalidEpubError,
  MalformedZipError,
  ZipSlipDetectedError,
  ImageDecodeError,
  OptimizerInternalError,
  ERROR_MESSAGES,
  isKnownError,
  userMessageFor,
} from "./errors.js";

/** Default option set, used by both preset resolution and tests. */
export function defaultOptions(): OptimizeOptions {
  return {
    preset: "full",
    target: {
      width: 960,
      height: 540,
      grayLevels: 16,
      palette: PAPERLOOM_GRAY_PALETTE,
    },
    features: {
      contrastBoost: true,
      removeFonts: true,
      cleanCss: true,
      generateCover: true,
      cleanMetadata: true,
      textCleanup: true,
      lightNovel: false,
    },
    jpegQuality: 78,
    dither: "floyd-steinberg",
  };
}

/**
 * Resolve preset-driven feature flags. Custom respects user input verbatim;
 * "quick" trims to images + text only; "full" enables everything.
 */
export function resolvePreset(opts: OptimizeOptions): OptimizeOptions {
  if (opts.preset === "custom") return opts;
  const features =
    opts.preset === "quick"
      ? {
          contrastBoost: true,
          removeFonts: false,
          cleanCss: false,
          generateCover: false,
          cleanMetadata: false,
          textCleanup: true,
          lightNovel: opts.features.lightNovel,
        }
      : {
          contrastBoost: true,
          removeFonts: true,
          cleanCss: true,
          generateCover: true,
          cleanMetadata: true,
          textCleanup: true,
          lightNovel: opts.features.lightNovel,
        };
  return { ...opts, features };
}

/**
 * Throttle progress callbacks to ~60Hz using requestAnimationFrame in DOM
 * contexts and a setTimeout fallback in worker / Node contexts.
 */
function makeThrottledEmitter(onProgress: ProgressCallback): ProgressCallback {
  let last = 0;
  return (event) => {
    const now = typeof performance !== "undefined" ? performance.now() : Date.now();
    if (event.phase === "start" || event.phase === "done" || event.phase === "error") {
      last = now;
      onProgress(event);
      return;
    }
    if (now - last < 16) return;
    last = now;
    onProgress(event);
  };
}

/**
 * Main entry point.
 *
 * The orchestrator does not catch step errors — fatal kinds (DRM, malformed
 * zip, zip-slip, invalid EPUB) propagate so the UI can render the right
 * message. Per-image decode errors are caught inside `processImages` and
 * downgraded to warnings.
 */
export async function optimizeEpub(
  input: Blob | ArrayBuffer | Uint8Array,
  options: OptimizeOptions = defaultOptions(),
  onProgress: ProgressCallback = () => {},
): Promise<OptimizeResult> {
  const startedAt = performance.now();
  const opts = resolvePreset(options);
  const emit = makeThrottledEmitter(onProgress);
  const warnings: OptimizeWarning[] = [];

  const inputBytes = await asUint8(input);
  const originalBytes = inputBytes.byteLength;

  let entries: Record<string, Uint8Array>;
  try {
    entries = unzipSync(inputBytes);
  } catch (err) {
    throw new MalformedZipError(`fflate.unzipSync failed: ${(err as Error).message}`);
  }

  // Step 0 (off-budget): zip-slip guard. Has its own pipeline step name so
  // tests can assert it runs before structure parsing.
  guardZipEntries(entries);

  // Convert to a Map for pipeline modules.
  const entryMap = new Map<string, Uint8Array>(Object.entries(entries));
  let model: EpubModel | undefined;
  let imagesProcessed = 0;
  let imagesSkipped = 0;

  for (const step of PIPELINE_STEPS) {
    emit({ step, phase: "start", pct: 0 });
    try {
      switch (step) {
        case "drm":
          detectDrm(entryMap);
          break;
        case "structure":
          model = parseStructure(entryMap);
          break;
        case "metadata":
          must(model);
          if (opts.features.cleanMetadata || opts.metadata) {
            applyMetadata(model, opts, warnings);
          }
          break;
        case "images": {
          must(model);
          const result = await processImages(model, opts, warnings, (pct, detail) => {
            emit({ step, phase: "progress", pct, detail });
          });
          imagesProcessed = result.processed;
          imagesSkipped = result.skipped;
          break;
        }
        case "html":
          must(model);
          cleanHtml(model, warnings);
          break;
        case "css":
          must(model);
          if (opts.features.cleanCss) {
            pruneCss(model, warnings);
          }
          break;
        case "fonts":
          must(model);
          if (opts.features.removeFonts) {
            stripFonts(model, warnings);
          }
          break;
        case "text":
          must(model);
          if (opts.features.textCleanup) {
            cleanText(model, warnings);
          }
          break;
        case "toc":
          must(model);
          repairToc(model, warnings);
          break;
        case "artifacts":
          must(model);
          purgeArtifacts(model);
          break;
        case "package": {
          must(model);
          break;
        }
      }
      emit({ step, phase: "done", pct: 100 });
    } catch (err) {
      // Fatal kinds: re-throw so the UI can render the right message.
      if (
        err instanceof DrmDetectedError ||
        err instanceof InvalidEpubError ||
        err instanceof MalformedZipError ||
        err instanceof ZipSlipDetectedError
      ) {
        emit({ step, phase: "error", pct: 0, error: err });
        throw err;
      }
      if (err instanceof ImageDecodeError) {
        // Already captured as warning inside processImages; should not reach here.
        warnings.push({ step, code: "IMAGE_DECODE", message: err.message });
        emit({ step, phase: "error", pct: 0, error: err });
        continue;
      }
      emit({ step, phase: "error", pct: 0, error: err as Error });
      throw new OptimizerInternalError(`step ${step}: ${(err as Error).message}`);
    }
  }

  must(model);

  // pipeline/package re-zips from the model + returns the final blob.
  const zipped = repackage(model, opts);
  const finalBytes = zipSync(zipped.entries, { mtime: new Date(zipped.mtime) });

  const resultBlob = new Blob([new Uint8Array(finalBytes)], { type: "application/epub+zip" });
  return {
    blob: resultBlob,
    filename: sanitizeFilename(`${model.metadata.author} - ${model.metadata.title}.epub`),
    stats: {
      originalBytes,
      resultBytes: resultBlob.size,
      durationMs: performance.now() - startedAt,
      imagesProcessed,
      imagesSkipped,
    },
    warnings,
  };
}

function must<T>(value: T | undefined): asserts value is T {
  if (value === undefined) {
    throw new OptimizerInternalError("pipeline reached a step before EpubModel was built");
  }
}

async function asUint8(input: Blob | ArrayBuffer | Uint8Array): Promise<Uint8Array> {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  return new Uint8Array(await input.arrayBuffer());
}
