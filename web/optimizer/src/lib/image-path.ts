/**
 * 2x2 fallback matrix resolver for the image pipeline.
 *
 *   ┌─────────────────┬──────────────┬─────────────────┐
 *   │ OffscreenCanvas │ WebCodecs    │ Path            │
 *   ├─────────────────┼──────────────┼─────────────────┤
 *   │ ✓               │ ✓            │ worker + IC     │
 *   │ ✓               │ ✗            │ worker + C2D    │
 *   │ ✗               │ ✓ or ✗       │ main + C2D      │
 *   └─────────────────┴──────────────┴─────────────────┘
 *
 * Per HIGH-4 from the brainstorm: a single resolver, called by both the
 * pipeline (pipeline/images.ts) and the worker dispatcher. No duplicated
 * feature detection logic.
 */

export type ImagePath = "worker+ic" | "worker+c2d" | "main+c2d";

export function resolveImagePath(global: typeof globalThis = globalThis): ImagePath {
  const hasOffscreen = typeof global.OffscreenCanvas !== "undefined";
  const hasWebCodecs =
    typeof (global as { ImageEncoder?: unknown }).ImageEncoder !== "undefined" &&
    typeof (global as { ImageDecoder?: unknown }).ImageDecoder !== "undefined";

  if (!hasOffscreen) return "main+c2d";
  if (hasWebCodecs) return "worker+ic";
  return "worker+c2d";
}

/** Whether the active path is worker-backed (used for warning UX). */
export function isWorkerPath(path: ImagePath): boolean {
  return path === "worker+ic" || path === "worker+c2d";
}
