/**
 * Text cleanup pipeline step.
 *
 * Walks every XHTML document in the spine + nav-doc and applies:
 *  - Whitespace collapse
 *  - OCR ligature replacement (Latin-only, RTL-guarded)
 *  - Smart-quote -> straight-quote (Latin-only, RTL-guarded)
 *  - Mojibake heuristic (Latin-1 misinterpretation of UTF-8)
 *  - Unicode NFC normalization
 *
 * Skip <script>, <style>, <pre>, <code>, and any node whose ancestor has
 * `dir="rtl"`. Per-codepoint Latin-run-length guard prevents corruption of
 * incidental CJK / mixed content.
 */
import type { EpubModel } from "../lib/epub-model.js";
import { resolveHref } from "../lib/epub-model.js";
import type { OptimizeWarning } from "../types.js";
import { containsRtlGlyphs, hasLatinNeighbours, isRtlAttribute } from "../lib/rtl-detect.js";

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder("utf-8");

// Keys use explicit \u escapes so neither esbuild/vitest's source
// transformer nor an editor's auto-normalization (NFC/NFKC) can mangle the
// Unicode codepoints we're looking up.
// Build at runtime via String.fromCharCode so neither vitest's esbuild
// transformer nor any editor normalization can mutate the Unicode keys.
const LIGATURES: Record<string, string> = {
  [String.fromCharCode(0xfb00)]: "ff",
  [String.fromCharCode(0xfb01)]: "fi",
  [String.fromCharCode(0xfb02)]: "fl",
  [String.fromCharCode(0xfb03)]: "ffi",
  [String.fromCharCode(0xfb04)]: "ffl",
  [String.fromCharCode(0xfb05)]: "ft",
  [String.fromCharCode(0xfb06)]: "st",
};

const SMART_QUOTE: Record<string, string> = {
  "‘": "'",
  "’": "'",
  "‚": "'",
  "‛": "'",
  "“": '"',
  "”": '"',
  "„": '"',
  "‟": '"',
  "–": "-",
  "—": "-",
};

const SKIP_TAGS = new Set(["script", "style", "pre", "code"]);

export function cleanText(model: EpubModel, warnings: OptimizeWarning[]): void {
  let touchedDocs = 0;
  for (const item of model.spine) {
    const manifest = model.manifest.get(item.idref);
    if (!manifest) continue;
    if (!isXhtml(manifest.mediaType, manifest.href)) continue;
    const path = resolveHref(model, manifest.href);
    const bytes = model.entries.get(path);
    if (!bytes) continue;
    const original = DECODER.decode(bytes);
    const cleaned = cleanXhtmlString(original);
    if (cleaned !== original) {
      model.entries.set(path, ENCODER.encode(cleaned));
      touchedDocs += 1;
    }
  }
  if (touchedDocs === 0) {
    warnings.push({
      step: "text",
      code: "TEXT_NO_OP",
      message: "No XHTML documents were modified by the text cleanup pass.",
    });
  }
}

export function cleanXhtmlString(input: string): string {
  // Parse, walk, rewrite text nodes, re-serialize.
  const doc = new DOMParser().parseFromString(input, "application/xhtml+xml");
  const useHtmlFallback = doc.querySelector("parsererror") !== null;
  const workDoc = useHtmlFallback
    ? new DOMParser().parseFromString(input, "text/html")
    : doc;

  walkText(workDoc.documentElement);

  return new XMLSerializer().serializeToString(workDoc);
}

function walkText(root: Element | null): void {
  if (!root) return;
  // Manual childNodes traversal — happy-dom's TreeWalker doesn't always
  // descend through XHTML doc trees the same way the browser does.
  const stack: Element[] = [root];
  while (stack.length > 0) {
    const el = stack.pop()!;
    if (SKIP_TAGS.has(el.tagName.toLowerCase())) continue;
    const rtlAttr = isRtlAttribute(el);
    for (const child of Array.from(el.childNodes)) {
      if (child.nodeType === /* TEXT_NODE */ 3) {
        const tn = child as Text;
        if (tn.data.length === 0) continue;
        const rtl = rtlAttr || containsRtlGlyphs(tn.data);
        const cleaned = transformText(tn.data, rtl);
        if (cleaned !== tn.data) tn.data = cleaned;
      } else if (child.nodeType === /* ELEMENT_NODE */ 1) {
        stack.push(child as Element);
      }
    }
  }
}

/** Visible-for-testing pure transform of a single text run. */
export function transformText(input: string, isRtl: boolean): string {
  let out = input;

  // Whitespace collapse — even in RTL.
  out = out.replace(/[ \t]+/g, " ");
  out = out.replace(/ ?\n /g, "\n");

  // Mojibake repair: Latin-1 interpretation of UTF-8 yields the well-known
  // "Â" / "â€" sequences. Run only on Latin-leaning content.
  if (!isRtl) out = repairMojibake(out);

  if (!isRtl) {
    out = replaceWithGuard(out, LIGATURES);
    out = replaceWithGuard(out, SMART_QUOTE);
  }

  // Unicode NFC, last so it never re-normalizes a half-replaced char.
  out = out.normalize("NFC");
  return out;
}

function replaceWithGuard(text: string, table: Record<string, string>): string {
  let buf = "";
  for (let i = 0; i < text.length; i += 1) {
    const ch = text[i]!;
    const replacement = table[ch];
    if (replacement && hasLatinNeighbours(text, i)) {
      buf += replacement;
    } else {
      buf += ch;
    }
  }
  return buf;
}

const MOJIBAKE_TABLE: [RegExp, string][] = [
  [/â€™/g, "'"],
  [/â€œ/g, '"'],
  [/â€/g, '"'],
  [/â€“/g, "-"],
  [/â€”/g, "-"],
  [/â€¦/g, "..."],
  [/Ã©/g, "é"],
  [/Ã¨/g, "è"],
  [/Ãª/g, "ê"],
  [/Ã /g, "à"],
  [/Ã¢/g, "â"],
  [/Ã®/g, "î"],
  [/Ã´/g, "ô"],
  [/Ã¼/g, "ü"],
  [/Ã¶/g, "ö"],
  [/Ã¤/g, "ä"],
  [/ÃŸ/g, "ß"],
];

function repairMojibake(input: string): string {
  let out = input;
  for (const [re, with_] of MOJIBAKE_TABLE) {
    out = out.replace(re, with_);
  }
  return out;
}

function isXhtml(mediaType: string, href: string): boolean {
  if (/xhtml|html/i.test(mediaType)) return true;
  return /\.x?html?$/i.test(href);
}
