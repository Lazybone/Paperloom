/**
 * In-memory representation of a decoded EPUB.
 *
 * The orchestrator unzips the input once via fflate.unzipSync, populates this
 * model in pipeline/structure, and then every subsequent step mutates the
 * model in place. The final pipeline/package step re-zips from the model's
 * `entries` map.
 *
 * Coupling note (ARCH iter-1): keeping all step modules behind a single
 * EpubModel keeps inter-step coupling shallow — modules don't reach into
 * each other's state, they only read/write fields here.
 */

export interface ManifestItem {
  id: string;
  href: string;
  mediaType: string;
}

export interface SpineItem {
  idref: string;
  linear: boolean;
}

export interface EpubMetadata {
  title: string;
  author: string;
  language?: string;
  identifier?: string;
  coverHref?: string;
  /** Anything we don't recognize gets preserved verbatim in the OPF. */
  extras: Record<string, string>;
}

export interface EpubModel {
  /** Raw ZIP entries, keyed by full path inside the archive. */
  entries: Map<string, Uint8Array>;
  /** Path of the OPF document inside `entries`, e.g. "OEBPS/content.opf". */
  opfPath: string;
  /** Directory the OPF lives in, used to resolve relative manifest hrefs. */
  opfDir: string;
  /** Detected EPUB version (2 or 3). */
  epubVersion: 2 | 3;
  /** Parsed OPF metadata block. */
  metadata: EpubMetadata;
  /** Manifest items keyed by id. */
  manifest: Map<string, ManifestItem>;
  /** Spine, in reading order. */
  spine: SpineItem[];
  /** Path of the NCX (EPUB 2) or nav-doc (EPUB 3), if present. */
  navPath?: string;
}

/** Resolve a manifest href to an absolute path inside `entries`. */
export function resolveHref(model: EpubModel, hrefFromOpf: string): string {
  if (hrefFromOpf.startsWith("/")) return hrefFromOpf.replace(/^\//, "");
  if (model.opfDir === "") return hrefFromOpf;
  return `${model.opfDir}/${hrefFromOpf}`;
}

/** Inverse of `resolveHref`: produce an OPF-relative href from an entry path. */
export function unresolveHref(model: EpubModel, entryPath: string): string {
  if (model.opfDir === "") return entryPath;
  const prefix = `${model.opfDir}/`;
  return entryPath.startsWith(prefix) ? entryPath.slice(prefix.length) : entryPath;
}

/** Look up an entry by either the manifest id or the resolved entry path. */
export function findEntry(model: EpubModel, idOrPath: string): Uint8Array | undefined {
  const direct = model.entries.get(idOrPath);
  if (direct) return direct;
  const item = model.manifest.get(idOrPath);
  if (!item) return undefined;
  return model.entries.get(resolveHref(model, item.href));
}
