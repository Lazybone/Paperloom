/**
 * Re-package the EpubModel into a ZIP-ready entry map.
 *
 * EPUB spec requires:
 *  - "mimetype" is the FIRST entry in the archive.
 *  - "mimetype" is STORED (no compression).
 *  - Everything else uses deflate compression at moderate quality.
 *
 * fflate's `zipSync` honours an explicit `{ level }` per entry. We pass
 * `{ level: 0 }` on mimetype and `{ level: 6 }` on everything else.
 *
 * Defence-in-depth: re-validate every path before re-zip so we never emit
 * an EPUB that smuggles ../ entries even if the model was poisoned mid-run.
 */
import type { Zippable } from "fflate";
import type { EpubModel } from "../lib/epub-model.js";
import type { OptimizeOptions } from "../types.js";
import { assertSafeOutputPath } from "./zip-guard.js";

export interface PackagedZip {
  entries: Zippable;
  mtime: number;
}

const MIMETYPE = "application/epub+zip";
/** fflate's ZIP timestamp range is 1980-01-01 .. 2099-12-31; use a fixed
 * 1980 epoch for reproducible bundle hashes regardless of CI clock skew. */
const MTIME = Date.UTC(1980, 0, 1);

export function repackage(model: EpubModel, _opts: OptimizeOptions): PackagedZip {
  const entries: Zippable = {};

  // mimetype must come first and must NOT be compressed.
  entries["mimetype"] = [new TextEncoder().encode(MIMETYPE), { level: 0 }];

  for (const [path, data] of model.entries) {
    if (path === "mimetype") continue; // we already wrote it.
    assertSafeOutputPath(path);
    entries[path] = [data, { level: 6 }];
  }

  return { entries, mtime: MTIME };
}
