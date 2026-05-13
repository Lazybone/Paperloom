/**
 * Conservative RTL detection.
 *
 * Used by pipeline/text.ts to short-circuit Latin-only transformations
 * (OCR-ligature substitutions, smart-quote replacement). Coverage:
 * Arabic, Hebrew, Persian, Urdu. Syriac / Thaana / N'Ko / Mandaic are not
 * detected — documented as a v0.4 follow-up.
 *
 * Two layers:
 *  1. Ancestor `dir=rtl` attribute (authoritative when present).
 *  2. Unicode block heuristic: a codepoint qualifies for Latin-only
 *     transformation when EITHER side has at least `minRun` consecutive
 *     Latin characters. Default minRun is 2.
 */

// Explicit \u escapes — some Hebrew/Arabic glyphs are multi-codepoint
// sequences when written as literals, which JavaScript misparses inside a
// character class. Coverage:
//   U+0590..U+08FF  Hebrew + Arabic + Syriac (+ filler blocks)
//   U+FB1D..U+FDFF  Hebrew presentation forms + Arabic presentation A
//   U+FE70..U+FEFC  Arabic presentation forms B
const RTL_RANGE = /[֐-ࣿיִ-﷿ﹰ-ﻼ]/;
const LATIN = /[A-Za-z]/;

export function isRtlAttribute(node: Element | null): boolean {
  let cur: Element | null = node;
  while (cur) {
    const dir = cur.getAttribute("dir");
    if (dir && dir.toLowerCase() === "rtl") return true;
    cur = cur.parentElement;
  }
  return false;
}

export function containsRtlGlyphs(text: string): boolean {
  return RTL_RANGE.test(text);
}

/**
 * @returns true when the codepoint at `index` sits inside a Latin word —
 *   defined as having `minRun` consecutive Latin neighbours on EITHER side.
 *   Default minimum run length is 2.
 */
export function hasLatinNeighbours(
  text: string,
  index: number,
  minRun = 2,
): boolean {
  let leftRun = 0;
  for (let i = index - 1; i >= 0; i -= 1) {
    if (!LATIN.test(text[i] ?? "")) break;
    leftRun += 1;
    if (leftRun >= minRun) return true;
  }
  let rightRun = 0;
  for (let i = index + 1; i < text.length; i += 1) {
    if (!LATIN.test(text[i] ?? "")) break;
    rightRun += 1;
    if (rightRun >= minRun) return true;
  }
  return false;
}
