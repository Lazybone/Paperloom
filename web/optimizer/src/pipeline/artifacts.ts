/**
 * Drop OS-specific noise files from the EPUB.
 * .DS_Store, Thumbs.db, desktop.ini, __MACOSX/* — anything a reader doesn't
 * need that bloats the file.
 */
import type { EpubModel } from "../lib/epub-model.js";

const ARTIFACT_PATTERNS = [
  /(^|\/)\.DS_Store$/i,
  /(^|\/)Thumbs\.db$/i,
  /(^|\/)desktop\.ini$/i,
  /^__MACOSX(\/|$)/,
];

export function purgeArtifacts(model: EpubModel): void {
  for (const path of Array.from(model.entries.keys())) {
    if (ARTIFACT_PATTERNS.some((re) => re.test(path))) {
      model.entries.delete(path);
    }
  }
}
