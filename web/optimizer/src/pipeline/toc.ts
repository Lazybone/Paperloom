/**
 * TOC repair pipeline step.
 *
 * EPUB 3: validate the nav-doc (a XHTML file with <nav epub:type="toc">).
 * If absent or empty, generate one from the first <h1>/<h2> of every spine
 * document.
 *
 * EPUB 2: validate the NCX. If absent or empty, generate a minimal NCX
 * from the spine + spine-document headings.
 *
 * Either way: if a TOC exists, leave it as-is. We don't try to merge or
 * re-order entries — that's user-visible content.
 */
import type { EpubModel } from "../lib/epub-model.js";
import { resolveHref } from "../lib/epub-model.js";
import type { OptimizeWarning } from "../types.js";

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder("utf-8");

export function repairToc(model: EpubModel, warnings: OptimizeWarning[]): void {
  if (model.navPath) {
    const bytes = model.entries.get(model.navPath);
    if (bytes && bytes.byteLength > 0) {
      // Existing TOC; leave alone.
      return;
    }
  }
  // Build entries from spine headings.
  const entries: { href: string; label: string }[] = [];
  for (const spineItem of model.spine) {
    const manifest = model.manifest.get(spineItem.idref);
    if (!manifest) continue;
    if (!/xhtml|html/i.test(manifest.mediaType) && !/\.x?html?$/i.test(manifest.href)) continue;
    const path = resolveHref(model, manifest.href);
    const bytes = model.entries.get(path);
    if (!bytes) continue;
    const doc = new DOMParser().parseFromString(DECODER.decode(bytes), "application/xhtml+xml");
    const label = firstHeading(doc) ?? manifest.href;
    entries.push({ href: manifest.href, label });
  }

  if (entries.length === 0) {
    warnings.push({
      step: "toc",
      code: "TOC_NO_SPINE_HEADINGS",
      message: "Could not generate a TOC: no spine documents with headings.",
    });
    return;
  }

  if (model.epubVersion === 3) {
    writeNav(model, entries);
  } else {
    writeNcx(model, entries);
  }
  warnings.push({
    step: "toc",
    code: "TOC_GENERATED",
    message: `Generated a ${entries.length}-entry TOC from spine headings.`,
  });
}

function firstHeading(doc: Document): string | null {
  for (const tag of ["h1", "h2"]) {
    const el = doc.getElementsByTagName(tag)[0];
    if (el && el.textContent && el.textContent.trim().length > 0) {
      return el.textContent.trim();
    }
  }
  return null;
}

function writeNav(model: EpubModel, entries: { href: string; label: string }[]): void {
  const navHref = "nav.xhtml";
  const navPath = model.opfDir === "" ? navHref : `${model.opfDir}/${navHref}`;
  const body = entries
    .map((e) => `<li><a href="${escapeAttr(e.href)}">${escapeText(e.label)}</a></li>`)
    .join("\n");
  const xhtml = `<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Table of Contents</title></head>
<body>
<nav epub:type="toc" id="toc">
<h1>Contents</h1>
<ol>
${body}
</ol>
</nav>
</body>
</html>
`;
  model.entries.set(navPath, ENCODER.encode(xhtml));
  model.navPath = navPath;
  // Register in OPF manifest with properties="nav".
  registerManifestItem(model, "nav", navHref, "application/xhtml+xml", "nav");
}

function writeNcx(model: EpubModel, entries: { href: string; label: string }[]): void {
  const ncxHref = "toc.ncx";
  const ncxPath = model.opfDir === "" ? ncxHref : `${model.opfDir}/${ncxHref}`;
  const navPoints = entries
    .map(
      (e, i) =>
        `<navPoint id="nav-${i + 1}" playOrder="${i + 1}"><navLabel><text>${escapeText(e.label)}</text></navLabel><content src="${escapeAttr(e.href)}" /></navPoint>`,
    )
    .join("\n");
  const ncx = `<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head>
  <meta name="dtb:uid" content="${escapeAttr(model.metadata.identifier ?? "paperloom")}" />
  <meta name="dtb:depth" content="1" />
  <meta name="dtb:totalPageCount" content="0" />
  <meta name="dtb:maxPageNumber" content="0" />
</head>
<docTitle><text>${escapeText(model.metadata.title)}</text></docTitle>
<navMap>
${navPoints}
</navMap>
</ncx>
`;
  model.entries.set(ncxPath, ENCODER.encode(ncx));
  model.navPath = ncxPath;
  registerManifestItem(model, "ncx", ncxHref, "application/x-dtbncx+xml");
  // Wire spine toc="ncx".
  rewriteSpineToc(model, "ncx");
}

function registerManifestItem(
  model: EpubModel,
  id: string,
  href: string,
  mediaType: string,
  properties?: string,
): void {
  if (!model.manifest.has(id)) {
    model.manifest.set(id, { id, href, mediaType });
  }
  const opfBytes = model.entries.get(model.opfPath);
  if (!opfBytes) return;
  const doc = new DOMParser().parseFromString(DECODER.decode(opfBytes), "application/xml");
  if (doc.querySelector("parsererror")) return;
  const manifest = doc.getElementsByTagName("manifest")[0];
  if (!manifest) return;
  const existing = doc.querySelector(`item[id="${id}"]`);
  const opfNs = doc.documentElement.namespaceURI;
  const item = existing ?? doc.createElementNS(opfNs, "item");
  item.setAttribute("id", id);
  item.setAttribute("href", href);
  item.setAttribute("media-type", mediaType);
  if (properties) item.setAttribute("properties", properties);
  if (!existing) manifest.appendChild(item);
  model.entries.set(model.opfPath, ENCODER.encode(new XMLSerializer().serializeToString(doc)));
}

function rewriteSpineToc(model: EpubModel, idref: string): void {
  const opfBytes = model.entries.get(model.opfPath);
  if (!opfBytes) return;
  const doc = new DOMParser().parseFromString(DECODER.decode(opfBytes), "application/xml");
  if (doc.querySelector("parsererror")) return;
  const spine = doc.getElementsByTagName("spine")[0];
  if (!spine) return;
  spine.setAttribute("toc", idref);
  model.entries.set(model.opfPath, ENCODER.encode(new XMLSerializer().serializeToString(doc)));
}

function escapeText(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
function escapeAttr(s: string): string {
  return escapeText(s).replace(/"/g, "&quot;");
}
