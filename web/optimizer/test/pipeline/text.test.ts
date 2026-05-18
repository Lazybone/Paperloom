import { describe, it, expect } from "vitest";
import { transformText, cleanXhtmlString } from "../../src/pipeline/text.js";

describe("transformText (Latin-mode)", () => {
  it("replaces OCR ligatures inside Latin runs", () => {
    expect(transformText("the ﬁsh and ﬂight", false)).toContain("fi");
    expect(transformText("the ﬁsh and ﬂight", false)).toContain("fl");
  });

  it("rewrites smart quotes", () => {
    expect(transformText("said “hello”", false)).toBe("said \"hello\"");
    expect(transformText("don’t", false)).toBe("don't");
  });

  it("collapses internal whitespace", () => {
    expect(transformText("multi    spaces", false)).toBe("multi spaces");
  });

  it("repairs common UTF-8/Latin-1 mojibake", () => {
    expect(transformText("clichÃ©", false)).toContain("é");
  });

  it("NFC-normalizes the result", () => {
    // Two ways to spell "é": a + combining acute (decomposed) vs precomposed.
    const decomposed = "café";
    expect(transformText(decomposed, false)).toBe(decomposed.normalize("NFC"));
  });
});

describe("transformText (RTL-mode)", () => {
  it("skips ligature replacement when isRtl=true", () => {
    expect(transformText("ﬁ", true)).toBe("ﬁ".normalize("NFC"));
  });
  it("skips smart-quote replacement when isRtl=true", () => {
    expect(transformText("“hello”", true)).toContain("“");
  });
});

describe("cleanXhtmlString", () => {
  it("rewrites text inside the body but leaves structure intact", () => {
    const input =
      `<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><body><p>the ﬁsh</p></body></html>`;
    const output = cleanXhtmlString(input);
    expect(output).toContain("fi");
    expect(output).toContain("<html");
    expect(output).toContain("<body");
  });
});
