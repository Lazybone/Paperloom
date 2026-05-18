import { describe, it, expect } from "vitest";
import { autocontrastGrayscale, contrastBoost } from "../../src/lib/autocontrast.js";

describe("autocontrastGrayscale", () => {
  it("identity-stretches an already full-range plane", () => {
    const src = new Uint8ClampedArray([0, 127, 255]);
    const { data, lowBucket, highBucket } = autocontrastGrayscale(src);
    expect(Array.from(data)).toEqual([0, 127, 255]);
    expect(lowBucket).toBe(0);
    expect(highBucket).toBe(255);
  });

  it("stretches a compressed range to full 0..255", () => {
    const src = new Uint8ClampedArray([100, 130, 160]);
    const { data } = autocontrastGrayscale(src);
    // Min was 100, max was 160; the lowest pixel must drop to 0 and the
    // highest must reach (or hit) 255.
    expect(data[0]).toBe(0);
    expect(data[2]).toBe(255);
  });

  it("falls back to identity when the histogram is a single bucket", () => {
    const src = new Uint8ClampedArray([50, 50, 50, 50]);
    const { data } = autocontrastGrayscale(src);
    expect(Array.from(data)).toEqual([50, 50, 50, 50]);
  });

  it("respects cutoff by ignoring tail outliers", () => {
    const src = new Uint8ClampedArray(100);
    src.fill(120);
    src[0] = 0;
    src[1] = 255;
    const { lowBucket, highBucket } = autocontrastGrayscale(src, 0.02);
    expect(lowBucket).toBeGreaterThan(0);
    expect(highBucket).toBeLessThan(255);
  });
});

describe("contrastBoost", () => {
  it("noops when factor === 1", () => {
    const src = new Uint8ClampedArray([10, 100, 200]);
    const out = contrastBoost(src, 1);
    expect(out).toBe(src);
  });

  it("pushes mid-gray neighbours toward black/white at 1.5x", () => {
    const src = new Uint8ClampedArray([64, 128, 192]);
    const out = contrastBoost(src, 1.5);
    expect(out[0]).toBeLessThan(64);
    expect(out[1]).toBe(128);
    expect(out[2]).toBeGreaterThan(192);
  });

  it("clamps results into 0..255", () => {
    const src = new Uint8ClampedArray([10, 245]);
    const out = contrastBoost(src, 3);
    expect(out[0]).toBe(0);
    expect(out[1]).toBe(255);
  });
});
