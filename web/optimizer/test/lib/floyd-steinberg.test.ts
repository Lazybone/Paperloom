import { describe, it, expect } from "vitest";
import { floydSteinberg } from "../../src/lib/floyd-steinberg.js";
import { PAPERLOOM_GRAY_PALETTE, snapToPalette } from "../../src/lib/paperloom-palette.js";

describe("floydSteinberg", () => {
  it("quantizes every output pixel to a palette value", () => {
    const w = 32;
    const h = 32;
    const src = new Uint8ClampedArray(w * h);
    for (let i = 0; i < src.length; i += 1) src[i] = i % 256;
    const out = floydSteinberg(src, w, h);
    const allowed = new Set(PAPERLOOM_GRAY_PALETTE);
    for (let i = 0; i < out.length; i += 1) {
      expect(allowed.has(out[i]!)).toBe(true);
    }
  });

  it("preserves overall brightness within a few percent", () => {
    const w = 64;
    const h = 64;
    const src = new Uint8ClampedArray(w * h);
    src.fill(128);
    const out = floydSteinberg(src, w, h);
    const mean = out.reduce((acc, v) => acc + v, 0) / out.length;
    expect(Math.abs(mean - 128)).toBeLessThan(5);
  });

  it("snapToPalette is consistent with the palette table", () => {
    expect(snapToPalette(0)).toBe(0);
    expect(snapToPalette(255)).toBe(255);
    expect(snapToPalette(17)).toBe(17);
    expect(snapToPalette(8)).toBe(0);
  });
});
