/**
 * HTML pipeline step.
 *
 * For every XHTML document in the spine + the nav-doc:
 *  - Parse via DOMParser as application/xhtml+xml. On <parsererror>,
 *    re-parse as text/html and re-emit (lossy but recovers most cases).
 *  - Strip <script>, <audio>, <video>, <source>, <track>, <iframe>, <embed>
 *    and inline event handlers (on*=...).
 *  - Strip attribute noise: data-*, aria-*, role, tabindex, style.
 *  - Re-serialize.
 *
 * Post-conditions (asserted by web/optimizer/test/security/security.test.ts):
 *   - no <script>, <audio>, <video>, <source>, <track>, <iframe>, <embed>
 *   - no attribute starting with `on`, `data-`, `aria-`
 *   - no `role`, `tabindex`, or `style` attribute
 *   - no parser-error left in the output (we always re-parse on failure)
 */
import type { EpubModel } from "../lib/epub-model.js";
import { resolveHref } from "../lib/epub-model.js";
import type { OptimizeWarning } from "../types.js";

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder("utf-8");

const BANNED_TAGS = new Set([
  "script",
  "audio",
  "video",
  "source",
  "track",
  "iframe",
  "embed",
  "object",
]);

const ATTR_BLOCKLIST_PREFIX = ["on", "data-", "aria-"];
const ATTR_BLOCKLIST_EXACT = new Set(["role", "tabindex", "style"]);

export function cleanHtml(model: EpubModel, warnings: OptimizeWarning[]): void {
  let recoveries = 0;
  for (const item of model.spine) {
    const manifest = model.manifest.get(item.idref);
    if (!manifest) continue;
    if (!isXhtml(manifest.mediaType, manifest.href)) continue;
    const path = resolveHref(model, manifest.href);
    const bytes = model.entries.get(path);
    if (!bytes) continue;
    const original = DECODER.decode(bytes);
    const result = sanitizeXhtmlString(original);
    if (result.usedHtmlFallback) recoveries += 1;
    if (result.html !== original) {
      model.entries.set(path, ENCODER.encode(result.html));
    }
  }
  if (recoveries > 0) {
    warnings.push({
      step: "html",
      code: "HTML_RECOVERY",
      message: `Re-parsed ${recoveries} XHTML document(s) as text/html after the strict parse failed.`,
    });
  }
}

export interface SanitizeResult {
  html: string;
  usedHtmlFallback: boolean;
}

export function sanitizeXhtmlString(input: string): SanitizeResult {
  let doc = new DOMParser().parseFromString(input, "application/xhtml+xml");
  let usedHtmlFallback = false;
  if (doc.querySelector("parsererror")) {
    doc = new DOMParser().parseFromString(input, "text/html");
    usedHtmlFallback = true;
  }
  stripBannedTags(doc);
  stripAttributes(doc);
  return {
    html: new XMLSerializer().serializeToString(doc),
    usedHtmlFallback,
  };
}

function stripBannedTags(doc: Document): void {
  for (const tag of BANNED_TAGS) {
    for (const el of Array.from(doc.getElementsByTagName(tag))) {
      el.parentNode?.removeChild(el);
    }
  }
}

function stripAttributes(doc: Document): void {
  const walker = doc.createTreeWalker(doc.documentElement, /* SHOW_ELEMENT */ 1);
  let node = walker.nextNode() as Element | null;
  while (node) {
    const attrs = Array.from(node.attributes);
    for (const attr of attrs) {
      const name = attr.name.toLowerCase();
      if (ATTR_BLOCKLIST_EXACT.has(name)) {
        node.removeAttribute(attr.name);
        continue;
      }
      if (ATTR_BLOCKLIST_PREFIX.some((p) => name.startsWith(p))) {
        node.removeAttribute(attr.name);
      }
    }
    node = walker.nextNode() as Element | null;
  }
}

function isXhtml(mediaType: string, href: string): boolean {
  if (/xhtml|html/i.test(mediaType)) return true;
  return /\.x?html?$/i.test(href);
}
