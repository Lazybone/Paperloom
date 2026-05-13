/**
 * Shared types for the Paperloom EPUB optimizer pipeline.
 *
 * Step ordering (string-typed `step` field of ProgressEvent) is the canonical
 * contract between the orchestrator and the UI binding layer (WP-7). Adding a
 * new step requires extending both `PipelineStep` here AND the pipeline
 * invocation order in `index.ts`. Tests assert the two stay in sync.
 */

/** Canonical pipeline step identifiers, in invocation order. */
export const PIPELINE_STEPS = [
  "drm",
  "structure",
  "metadata",
  "images",
  "html",
  "css",
  "fonts",
  "text",
  "toc",
  "artifacts",
  "package",
] as const;

export type PipelineStep = (typeof PIPELINE_STEPS)[number];

/** Optimization preset chosen in the UI. `custom` honours individual flags. */
export type Preset = "quick" | "full" | "custom";

/** Paperloom-tuned defaults. Mirrors what the firmware can actually display. */
export interface OptimizeOptions {
  preset: Preset;
  target: {
    width: 960;
    height: 540;
    grayLevels: 16;
    /** 16-element Uint8Array; see lib/paperloom-palette.ts. */
    palette: Uint8Array;
  };
  /** User overrides; only applied when defined. */
  metadata?: { title?: string; author?: string };
  /** Pipeline-step feature flags. `quick`/`full` overrides happen in index.ts. */
  features: {
    contrastBoost: boolean;
    removeFonts: boolean;
    cleanCss: boolean;
    generateCover: boolean;
    cleanMetadata: boolean;
    textCleanup: boolean;
    lightNovel: boolean;
  };
  jpegQuality: number; // 40..95
  dither: "floyd-steinberg" | "none";
}

/** An image plane shuttled between pipeline stages 5.4a → 5.4b → 5.4c. */
export interface PixelPlane {
  /** Grayscale (channels=1) or RGBA (channels=4) byte buffer, row-major. */
  data: Uint8ClampedArray;
  width: number;
  height: number;
  channels: 1 | 4;
}

/** Progress message emitted to the UI; throttled to ~60Hz by the orchestrator. */
export interface ProgressEvent {
  step: PipelineStep;
  phase: "start" | "progress" | "done" | "error";
  /** 0..100 within the current step. */
  pct: number;
  /** Optional human-readable detail, e.g. "image 12 / 45". */
  detail?: string;
  /** Set when phase === "error". */
  error?: Error;
}

/** Non-fatal observations surfaced to the UI alongside the result. */
export interface OptimizeWarning {
  step: PipelineStep;
  code: string;
  message: string;
}

/** Final result handed back to the UI. */
export interface OptimizeResult {
  blob: Blob;
  /** "Author - Title.epub" after filename sanitization. */
  filename: string;
  stats: {
    originalBytes: number;
    resultBytes: number;
    durationMs: number;
    imagesProcessed: number;
    imagesSkipped: number;
  };
  warnings: OptimizeWarning[];
}

export type ProgressCallback = (event: ProgressEvent) => void;
