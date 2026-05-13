/**
 * Font-stripping pipeline step.
 *
 * Deletes every .ttf/.otf/.woff/.woff2 entry from the manifest + ZIP map.
 * @font-face declarations are already removed by pipeline/css.ts; we just
 * make sure the underlying font files don't ride along.
 */
import type { EpubModel } from "../lib/epub-model.js";
import { resolveHref } from "../lib/epub-model.js";
import type { OptimizeWarning } from "../types.js";

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder("utf-8");
const FONT_RE = /\.(ttf|otf|woff2?|eot)$/i;

export function stripFonts(model: EpubModel, warnings: OptimizeWarning[]): void {
  const removedIds: string[] = [];

  for (const item of Array.from(model.manifest.values())) {
    if (FONT_RE.test(item.href) || /font/i.test(item.mediaType)) {
      const path = resolveHref(model, item.href);
      model.entries.delete(path);
      model.manifest.delete(item.id);
      removedIds.push(item.id);
    }
  }

  if (removedIds.length === 0) return;

  // Rewrite the OPF: drop the matching <item> entries.
  const opfBytes = model.entries.get(model.opfPath);
  if (!opfBytes) return;
  const opfDoc = new DOMParser().parseFromString(DECODER.decode(opfBytes), "application/xml");
  if (opfDoc.querySelector("parsererror")) {
    warnings.push({
      step: "fonts",
      code: "FONTS_OPF_UNPARSEABLE",
      message: `Could not re-write OPF after removing ${removedIds.length} font file(s).`,
    });
    return;
  }
  for (const id of removedIds) {
    const el = opfDoc.querySelector(`item[id="${cssEscape(id)}"]`);
    el?.parentNode?.removeChild(el);
  }
  const serialized = new XMLSerializer().serializeToString(opfDoc);
  model.entries.set(model.opfPath, ENCODER.encode(serialized));
}

function cssEscape(value: string): string {
  return value.replace(/(["\\\][^$.|?*+(){}^])/g, "\\$1");
}
