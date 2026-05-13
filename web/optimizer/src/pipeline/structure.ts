/**
 * Parse META-INF/container.xml -> OPF -> manifest + spine + nav into an
 * EpubModel.
 *
 * We use the platform DOMParser (browser + happy-dom in tests). XHTML is
 * parsed as application/xhtml+xml; the OPF and container.xml are XML
 * documents. DOMParser does not resolve external entities (no XXE) in any
 * modern browser, so we don't need extra hardening there.
 */
import { InvalidEpubError } from "../errors.js";
import type { EpubModel, EpubMetadata, ManifestItem, SpineItem } from "../lib/epub-model.js";

const TEXT_DECODER = new TextDecoder("utf-8");
const XML_MIME = "application/xml" as const;

export function parseStructure(entries: Map<string, Uint8Array>): EpubModel {
  const containerBytes = entries.get("META-INF/container.xml");
  if (!containerBytes) {
    throw new InvalidEpubError("missing META-INF/container.xml");
  }
  const containerDoc = parseXml(TEXT_DECODER.decode(containerBytes));
  const rootfile = containerDoc.querySelector('rootfile[full-path]');
  const opfPath = rootfile?.getAttribute("full-path");
  if (!opfPath) throw new InvalidEpubError("container.xml has no rootfile/full-path");

  const opfBytes = entries.get(opfPath);
  if (!opfBytes) throw new InvalidEpubError(`OPF not found at ${opfPath}`);
  const opfDoc = parseXml(TEXT_DECODER.decode(opfBytes));

  const opfRoot = opfDoc.documentElement;
  const version = opfRoot.getAttribute("version") ?? "3.0";
  const epubVersion: 2 | 3 = version.startsWith("2") ? 2 : 3;

  const metadata = parseMetadata(opfDoc);
  const manifest = parseManifest(opfDoc);
  const spine = parseSpine(opfDoc);

  // Cover detection (EPUB 2 uses <meta name="cover">, EPUB 3 uses
  // properties="cover-image" on the manifest item).
  metadata.coverHref = findCoverHref(opfDoc, manifest);

  // Nav doc: EPUB 3 manifest item with properties contains "nav"; EPUB 2
  // spine has toc="ncx" idref to an NCX file in the manifest.
  const navPath = findNavPath(opfDoc, manifest, opfPath);

  const opfDir = opfPath.includes("/") ? opfPath.slice(0, opfPath.lastIndexOf("/")) : "";

  return {
    entries,
    opfPath,
    opfDir,
    epubVersion,
    metadata,
    manifest,
    spine,
    navPath,
  };
}

function parseXml(text: string): Document {
  const parser = new DOMParser();
  const doc = parser.parseFromString(text, XML_MIME);
  if (doc.querySelector("parsererror")) {
    // Recovery: re-parse as text/html. Useful for slightly malformed XHTML
    // but the OPF is supposed to be strict XML.
    const fallback = new DOMParser().parseFromString(text, "text/html");
    if (fallback.documentElement) return fallback;
    throw new InvalidEpubError("XML parse error in OPF / container");
  }
  return doc;
}

function parseMetadata(opfDoc: Document): EpubMetadata {
  const metadataEl = opfDoc.getElementsByTagName("metadata")[0];
  const dc = (name: string): string => {
    if (!metadataEl) return "";
    const direct = metadataEl.getElementsByTagName(`dc:${name}`)[0]?.textContent;
    if (direct && direct.trim().length > 0) return direct.trim();
    // Some publishers drop the dc: prefix.
    const bare = metadataEl.getElementsByTagName(name)[0]?.textContent;
    return bare?.trim() ?? "";
  };
  const title = dc("title") || "Untitled";
  const author = dc("creator") || "Unknown";
  const language = dc("language") || undefined;
  const identifier = dc("identifier") || undefined;
  return { title, author, language, identifier, extras: {} };
}

function parseManifest(opfDoc: Document): Map<string, ManifestItem> {
  const out = new Map<string, ManifestItem>();
  const items = opfDoc.getElementsByTagName("item");
  for (const item of Array.from(items)) {
    const id = item.getAttribute("id");
    const href = item.getAttribute("href");
    const mediaType = item.getAttribute("media-type");
    if (!id || !href) continue;
    out.set(id, {
      id,
      href: decodeURIComponent(href),
      mediaType: mediaType ?? "application/octet-stream",
    });
  }
  return out;
}

function parseSpine(opfDoc: Document): SpineItem[] {
  const out: SpineItem[] = [];
  const items = opfDoc.getElementsByTagName("itemref");
  for (const item of Array.from(items)) {
    const idref = item.getAttribute("idref");
    if (!idref) continue;
    const linear = item.getAttribute("linear") !== "no";
    out.push({ idref, linear });
  }
  return out;
}

function findCoverHref(
  opfDoc: Document,
  manifest: Map<string, ManifestItem>,
): string | undefined {
  // EPUB 3: <item properties="cover-image" ...>
  for (const item of manifest.values()) {
    const raw = opfDoc.querySelector(`item[id="${cssEscape(item.id)}"]`);
    const props = raw?.getAttribute("properties") ?? "";
    if (props.split(/\s+/).includes("cover-image")) return item.href;
  }
  // EPUB 2: <meta name="cover" content="..."/> -> manifest id
  const meta = opfDoc.querySelector('meta[name="cover"]');
  const coverId = meta?.getAttribute("content");
  if (coverId) {
    const item = manifest.get(coverId);
    if (item) return item.href;
  }
  return undefined;
}

function findNavPath(
  opfDoc: Document,
  manifest: Map<string, ManifestItem>,
  opfPath: string,
): string | undefined {
  const opfDir = opfPath.includes("/") ? opfPath.slice(0, opfPath.lastIndexOf("/")) : "";
  // EPUB 3 nav.
  for (const item of manifest.values()) {
    const raw = opfDoc.querySelector(`item[id="${cssEscape(item.id)}"]`);
    const props = raw?.getAttribute("properties") ?? "";
    if (props.split(/\s+/).includes("nav")) {
      return joinPath(opfDir, item.href);
    }
  }
  // EPUB 2 NCX.
  const spineEl = opfDoc.getElementsByTagName("spine")[0];
  const tocId = spineEl?.getAttribute("toc");
  if (tocId) {
    const item = manifest.get(tocId);
    if (item) return joinPath(opfDir, item.href);
  }
  return undefined;
}

function cssEscape(value: string): string {
  return value.replace(/(["\\\][^$.|?*+(){}^])/g, "\\$1");
}

function joinPath(dir: string, href: string): string {
  if (dir === "") return href;
  return `${dir}/${href}`;
}
