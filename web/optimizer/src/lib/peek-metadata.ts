/**
 * Lightweight EPUB metadata + cover preview.
 *
 * Unzips the EPUB once, parses container.xml + OPF, extracts:
 *   - title (Dublin Core)
 *   - author (Dublin Core creator)
 *   - cover image bytes + mime type (EPUB 3 properties="cover-image" or
 *     EPUB 2 <meta name="cover">)
 *
 * Used by the UI binding layer to populate the file card before the user
 * clicks Optimize. Not part of the optimisation pipeline (the orchestrator
 * does its own parse via pipeline/structure.ts).
 *
 * Errors are caught and downgraded — if the preview fails for any reason,
 * the card falls back to the filename and a generic book glyph. We never
 * surface preview failures because they don't block optimisation.
 */
import { unzipSync } from "fflate";

export interface PeekResult {
  title: string;
  author: string;
  cover?: { bytes: Uint8Array; mimeType: string };
}

const TEXT_DECODER = new TextDecoder("utf-8");

export async function peekMetadata(file: Blob): Promise<PeekResult> {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const entries = unzipSync(bytes);
  const container = entries["META-INF/container.xml"];
  if (!container) return { title: "", author: "" };
  const opfPath = findOpfPath(TEXT_DECODER.decode(container));
  if (!opfPath || !entries[opfPath]) return { title: "", author: "" };
  const opf = new DOMParser().parseFromString(
    TEXT_DECODER.decode(entries[opfPath]),
    "application/xml",
  );
  if (opf.querySelector("parsererror")) return { title: "", author: "" };

  const title = dcText(opf, "title");
  const author = dcText(opf, "creator");
  const cover = readCover(opf, entries, opfPath);
  return { title, author, ...(cover ? { cover } : {}) };
}

function findOpfPath(containerXml: string): string | null {
  const doc = new DOMParser().parseFromString(containerXml, "application/xml");
  const rootfile = doc.querySelector("rootfile[full-path]");
  return rootfile?.getAttribute("full-path") ?? null;
}

function dcText(opf: Document, localName: string): string {
  const metadataEl = opf.getElementsByTagName("metadata")[0];
  if (!metadataEl) return "";
  const direct = metadataEl.getElementsByTagName(`dc:${localName}`)[0]?.textContent;
  if (direct && direct.trim().length > 0) return direct.trim();
  const bare = metadataEl.getElementsByTagName(localName)[0]?.textContent;
  return bare?.trim() ?? "";
}

function readCover(
  opf: Document,
  entries: Record<string, Uint8Array>,
  opfPath: string,
): { bytes: Uint8Array; mimeType: string } | undefined {
  const opfDir = opfPath.includes("/") ? opfPath.slice(0, opfPath.lastIndexOf("/")) : "";
  // Build a manifest map from id -> { href, mediaType }.
  const items = new Map<string, { href: string; mediaType: string; properties: string }>();
  for (const item of Array.from(opf.getElementsByTagName("item"))) {
    const id = item.getAttribute("id");
    const href = item.getAttribute("href");
    const mediaType = item.getAttribute("media-type") ?? "application/octet-stream";
    if (!id || !href) continue;
    items.set(id, {
      href: decodeURIComponent(href),
      mediaType,
      properties: item.getAttribute("properties") ?? "",
    });
  }

  // EPUB 3: properties contains "cover-image".
  for (const item of items.values()) {
    if (item.properties.split(/\s+/).includes("cover-image")) {
      return loadImage(entries, opfDir, item.href, item.mediaType);
    }
  }
  // EPUB 2: <meta name="cover" content="...">
  const coverMeta = opf.querySelector('meta[name="cover"]');
  const coverId = coverMeta?.getAttribute("content");
  if (coverId && items.has(coverId)) {
    const item = items.get(coverId)!;
    return loadImage(entries, opfDir, item.href, item.mediaType);
  }
  // Fallback: any item whose id contains "cover" and is an image.
  for (const [id, item] of items) {
    if (/cover/i.test(id) && /^image\//.test(item.mediaType)) {
      return loadImage(entries, opfDir, item.href, item.mediaType);
    }
  }
  return undefined;
}

function loadImage(
  entries: Record<string, Uint8Array>,
  opfDir: string,
  href: string,
  mediaType: string,
): { bytes: Uint8Array; mimeType: string } | undefined {
  const candidate = opfDir === "" ? href : `${opfDir}/${href}`;
  const bytes = entries[candidate];
  if (bytes) return { bytes, mimeType: mediaType };
  // Some EPUBs use root-relative hrefs; try the bare href too.
  if (entries[href]) return { bytes: entries[href]!, mimeType: mediaType };
  return undefined;
}
