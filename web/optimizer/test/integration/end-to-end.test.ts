import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import {
  optimizeEpub,
  defaultOptions,
  DrmDetectedError,
  ZipSlipDetectedError,
  PIPELINE_STEPS,
} from "../../src/index.js";
import type { OptimizeOptions, ProgressEvent } from "../../src/types.js";

const here = dirname(fileURLToPath(import.meta.url));
const fixtures = join(here, "..", "fixtures");

function load(name: string): Uint8Array {
  return new Uint8Array(readFileSync(join(fixtures, name)));
}

function optsNoImages(): OptimizeOptions {
  const base = defaultOptions();
  // Image pipeline relies on Canvas / createImageBitmap which happy-dom
  // doesn't ship. The fixture has no images so we still cover the entire
  // non-image pipeline.
  return { ...base, features: { ...base.features, generateCover: false } };
}

describe("optimizeEpub - end to end on minimal fixture", () => {
  it("runs through every PIPELINE_STEP in order", async () => {
    const events: ProgressEvent[] = [];
    const result = await optimizeEpub(load("minimal.epub"), optsNoImages(), (e) => {
      events.push(e);
    });
    expect(result.blob.size).toBeGreaterThan(0);
    // Steps emitted in order, "start" before "done" per step.
    const seenStarts = events.filter((e) => e.phase === "start").map((e) => e.step);
    expect(seenStarts).toEqual([...PIPELINE_STEPS]);
  });

  it("preserves the cleaned title in the output filename", async () => {
    const result = await optimizeEpub(load("minimal.epub"), optsNoImages());
    expect(result.filename.toLowerCase()).toContain("hello");
    expect(result.filename.toLowerCase()).toContain("tester");
    expect(result.filename.endsWith(".epub")).toBe(true);
  });

  it("strips Calibre store-specific metadata", async () => {
    const result = await optimizeEpub(load("minimal.epub"), optsNoImages());
    const out = new Uint8Array(await result.blob.arrayBuffer());
    const ascii = new TextDecoder("utf-8", { fatal: false }).decode(out);
    expect(ascii).not.toContain("calibre:series");
  });
});

describe("optimizeEpub - fatal kinds", () => {
  it("rejects DRM-protected EPUBs", async () => {
    await expect(optimizeEpub(load("drm.epub"), optsNoImages())).rejects.toBeInstanceOf(
      DrmDetectedError,
    );
  });

  it("rejects zip-slip entries before structure parsing", async () => {
    await expect(optimizeEpub(load("zip-slip.epub"), optsNoImages())).rejects.toBeInstanceOf(
      ZipSlipDetectedError,
    );
  });
});

describe("optimizeEpub - ligature fixture", () => {
  it("replaces OCR ligatures inside Latin text", async () => {
    const result = await optimizeEpub(load("ligatures.epub"), optsNoImages());
    const out = new Uint8Array(await result.blob.arrayBuffer());
    // Result blob is a ZIP; unpack and look at the xhtml inside.
    const { unzipSync, strFromU8 } = await import("fflate");
    const entries = unzipSync(out);
    const chapter = strFromU8(entries["OEBPS/ch1.xhtml"]!);
    expect(chapter).toContain("fish");
    expect(chapter).toContain("flight");
  });
});
