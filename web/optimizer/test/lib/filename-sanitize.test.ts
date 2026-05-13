import { describe, it, expect } from "vitest";
import { sanitizeFilename } from "../../src/lib/filename-sanitize.js";

describe("sanitizeFilename", () => {
  it("replaces path separators and traversal sequences", () => {
    expect(sanitizeFilename("../etc/passwd")).not.toContain("..");
    expect(sanitizeFilename("../etc/passwd")).not.toContain("/");
    expect(sanitizeFilename("a\\b")).not.toContain("\\");
  });

  it("falls back when the result would be empty or trivial", () => {
    expect(sanitizeFilename("...")).toBe("paperloom.epub");
    expect(sanitizeFilename("///")).toBe("paperloom.epub");
    expect(sanitizeFilename("")).toBe("paperloom.epub");
  });

  it("preserves a normal Author - Title.epub", () => {
    expect(sanitizeFilename("Anne Carson - Antigonick.epub")).toBe(
      "Anne Carson - Antigonick.epub",
    );
  });

  it("caps at 240 characters", () => {
    const out = sanitizeFilename(`${"a".repeat(300)}.epub`);
    expect(out.length).toBeLessThanOrEqual(240);
  });
});
