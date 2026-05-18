import { describe, it, expect } from "vitest";
import {
  containsRtlGlyphs,
  hasLatinNeighbours,
  isRtlAttribute,
} from "../../src/lib/rtl-detect.js";

describe("containsRtlGlyphs", () => {
  it("detects Arabic content", () => {
    expect(containsRtlGlyphs("مرحبا")).toBe(true);
  });
  it("detects Hebrew content", () => {
    expect(containsRtlGlyphs("שלום")).toBe(true);
  });
  it("returns false on pure Latin", () => {
    expect(containsRtlGlyphs("Hello world")).toBe(false);
  });
});

describe("hasLatinNeighbours", () => {
  it("returns true when the codepoint is inside a Latin word", () => {
    const text = "the fish flies";
    const idx = text.indexOf("ﬁ");
    // No literal ligature in this string — test the abstraction with a
    // plain index inside a Latin run instead.
    expect(hasLatinNeighbours(text, text.indexOf("f"))).toBe(true);
    expect(idx).toBe(-1);
  });

  it("returns false at run length < 3", () => {
    expect(hasLatinNeighbours("好 a 好", 2)).toBe(false);
  });
});

describe("isRtlAttribute", () => {
  it("walks up to find dir=rtl", () => {
    const doc = new DOMParser().parseFromString(
      `<div dir="rtl"><p><span id="t">x</span></p></div>`,
      "text/html",
    );
    const span = doc.getElementById("t");
    expect(isRtlAttribute(span)).toBe(true);
  });
  it("returns false when no ancestor sets dir=rtl", () => {
    const doc = new DOMParser().parseFromString(`<div><span id="t">x</span></div>`, "text/html");
    expect(isRtlAttribute(doc.getElementById("t"))).toBe(false);
  });
});
