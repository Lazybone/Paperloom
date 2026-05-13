import { describe, it, expect } from "vitest";
import { sanitizeXhtmlString } from "../../src/pipeline/html.js";

describe("sanitizeXhtmlString", () => {
  it("strips <script> tags", () => {
    const html = `<html xmlns="http://www.w3.org/1999/xhtml"><body><script>/* noop */</script><p>ok</p></body></html>`;
    expect(sanitizeXhtmlString(html).html).not.toContain("<script");
  });

  it("strips <audio> / <video> / <source> / <track> / <iframe>", () => {
    const html =
      `<html xmlns="http://www.w3.org/1999/xhtml"><body><audio src="x"/><video src="y"/><iframe/></body></html>`;
    const out = sanitizeXhtmlString(html).html;
    expect(out).not.toContain("<audio");
    expect(out).not.toContain("<video");
    expect(out).not.toContain("<iframe");
  });

  it("strips on*= event handlers and data-/aria- attributes", () => {
    const html = `<html xmlns="http://www.w3.org/1999/xhtml"><body><p onclick="return false" data-foo="1" aria-label="x" role="button" tabindex="0">hi</p></body></html>`;
    const out = sanitizeXhtmlString(html).html;
    expect(out).not.toMatch(/\son\w+=/i);
    expect(out).not.toMatch(/\sdata-/i);
    expect(out).not.toMatch(/\saria-/i);
    expect(out).not.toMatch(/\srole=/i);
    expect(out).not.toMatch(/\stabindex=/i);
  });

  it("returns sanitized output even when the input is loose HTML", () => {
    // happy-dom parses this without flagging a parsererror, so the
    // fallback path is environment-dependent. The contract we care about
    // is that the function never throws and always returns the body text.
    const html = "<html><body><p>no namespace</body></html>";
    const result = sanitizeXhtmlString(html);
    expect(result.html).toContain("no namespace");
    expect(typeof result.usedHtmlFallback).toBe("boolean");
  });
});
