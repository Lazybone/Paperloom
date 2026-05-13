import { describe, it, expect, vi, afterEach, beforeEach } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { optimizeEpub, defaultOptions, ZipSlipDetectedError, DrmDetectedError } from "../../src/index.js";
import { sanitizeXhtmlString } from "../../src/pipeline/html.js";

const here = dirname(fileURLToPath(import.meta.url));
const fixtures = join(here, "..", "fixtures");
function load(name: string): Uint8Array {
  return new Uint8Array(readFileSync(join(fixtures, name)));
}

describe("[security] zip-slip rejection", () => {
  it("refuses to parse an EPUB with a `../` entry", async () => {
    await expect(optimizeEpub(load("zip-slip.epub"))).rejects.toBeInstanceOf(ZipSlipDetectedError);
  });
});

describe("[security] DRM rejection", () => {
  it("refuses to process when META-INF/encryption.xml exists", async () => {
    await expect(optimizeEpub(load("drm.epub"))).rejects.toBeInstanceOf(DrmDetectedError);
  });
});

describe("[security] HTML sanitization post-conditions", () => {
  it("strips scripts, media, and on*=/data-/aria- attributes", () => {
    const html = `<html xmlns="http://www.w3.org/1999/xhtml"><body>
      <script>/* noop */</script>
      <p onclick="return false" data-id="1" aria-label="a" role="x" tabindex="2" style="color:red">hi</p>
      <audio/><video/><iframe/>
    </body></html>`;
    const out = sanitizeXhtmlString(html).html;
    expect(out).not.toContain("<script");
    expect(out).not.toContain("<audio");
    expect(out).not.toContain("<video");
    expect(out).not.toContain("<iframe");
    expect(out).not.toMatch(/\son\w+=/);
    expect(out).not.toMatch(/\sdata-/);
    expect(out).not.toMatch(/\saria-/);
    expect(out).not.toMatch(/\srole=/);
    expect(out).not.toMatch(/\stabindex=/);
    expect(out).not.toMatch(/\sstyle=/);
  });
});

describe("[security] no network egress during optimizeEpub", () => {
  let originalFetch: typeof globalThis.fetch | undefined;

  beforeEach(() => {
    originalFetch = globalThis.fetch;
    globalThis.fetch = vi.fn(() => {
      throw new Error("fetch must not be called during optimizeEpub");
    });
  });
  afterEach(() => {
    globalThis.fetch = originalFetch!;
  });

  it("never calls global fetch on a clean run", async () => {
    const base = defaultOptions();
    const opts = { ...base, features: { ...base.features, generateCover: false } };
    await optimizeEpub(load("minimal.epub"), opts);
    expect(globalThis.fetch).not.toHaveBeenCalled();
  });
});
