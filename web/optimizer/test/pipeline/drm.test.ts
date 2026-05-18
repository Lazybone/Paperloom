import { describe, it, expect } from "vitest";
import { DrmDetectedError } from "../../src/errors.js";
import { detectDrm } from "../../src/pipeline/drm.js";

describe("detectDrm", () => {
  it("passes when neither encryption.xml nor ADEPT rights present", () => {
    expect(() => detectDrm(new Map())).not.toThrow();
  });

  it("rejects any encryption.xml content", () => {
    const m = new Map<string, Uint8Array>([
      ["META-INF/encryption.xml", new TextEncoder().encode("<encryption/>")],
    ]);
    expect(() => detectDrm(m)).toThrow(DrmDetectedError);
  });

  it("rejects an Adobe ADEPT rights.xml", () => {
    const m = new Map<string, Uint8Array>([
      [
        "META-INF/rights.xml",
        new TextEncoder().encode("<root><adept:rights/></root>"),
      ],
    ]);
    expect(() => detectDrm(m)).toThrow(DrmDetectedError);
  });

  it("ignores a benign rights.xml without ADEPT markers", () => {
    const m = new Map<string, Uint8Array>([
      ["META-INF/rights.xml", new TextEncoder().encode("<root/>")],
    ]);
    expect(() => detectDrm(m)).not.toThrow();
  });
});
