/**
 * Metadata pipeline step.
 *
 *  - Strip store-specific Calibre / Amazon / Kobo / iBooks / Google Play
 *    meta tags from the OPF.
 *  - Apply user-provided title/author overrides when present.
 *  - Persist the cleaned OPF back into the EpubModel's entries map.
 */
import type { EpubModel } from "../lib/epub-model.js";
import type { OptimizeOptions, OptimizeWarning } from "../types.js";

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder("utf-8");

const STORE_META = [
  /^calibre[:.]/i,
  /^amazon[:.]/i,
  /^kindle[:.]/i,
  /^kobo[:.]/i,
  /^ibooks[:.]/i,
  /^google_play[:.]/i,
  /^store[:.]/i,
];

export function applyMetadata(
  model: EpubModel,
  opts: OptimizeOptions,
  warnings: OptimizeWarning[],
): void {
  const opfBytes = model.entries.get(model.opfPath);
  if (!opfBytes) {
    warnings.push({ step: "metadata", code: "OPF_MISSING", message: "OPF disappeared after structure parse; metadata skipped" });
    return;
  }
  const opfDoc = new DOMParser().parseFromString(DECODER.decode(opfBytes), "application/xml");
  if (opfDoc.querySelector("parsererror")) {
    warnings.push({ step: "metadata", code: "OPF_UNPARSEABLE", message: "OPF failed to parse; metadata skipped" });
    return;
  }

  const metadataEl = opfDoc.getElementsByTagName("metadata")[0];
  if (!metadataEl) {
    warnings.push({ step: "metadata", code: "OPF_NO_METADATA", message: "OPF has no <metadata> element" });
    return;
  }

  if (opts.features.cleanMetadata) {
    stripStoreMeta(metadataEl);
  }

  if (opts.metadata?.title && opts.metadata.title.trim().length > 0) {
    setDcText(metadataEl, "title", opts.metadata.title.trim());
    model.metadata.title = opts.metadata.title.trim();
  }
  if (opts.metadata?.author && opts.metadata.author.trim().length > 0) {
    setDcText(metadataEl, "creator", opts.metadata.author.trim());
    model.metadata.author = opts.metadata.author.trim();
  }

  const serialized = new XMLSerializer().serializeToString(opfDoc);
  model.entries.set(model.opfPath, ENCODER.encode(serialized));
}

function stripStoreMeta(metadataEl: Element): void {
  // <meta name="...."> for EPUB 2-style metadata + <meta property="...">
  // for EPUB 3. Remove any whose name/property matches our store list.
  const metas = Array.from(metadataEl.getElementsByTagName("meta"));
  for (const meta of metas) {
    const name = meta.getAttribute("name") ?? meta.getAttribute("property") ?? "";
    if (STORE_META.some((re) => re.test(name))) {
      meta.parentNode?.removeChild(meta);
    }
  }
}

function setDcText(metadataEl: Element, localName: string, value: string): void {
  const existing =
    metadataEl.getElementsByTagName(`dc:${localName}`)[0] ??
    metadataEl.getElementsByTagName(localName)[0];
  if (existing) {
    existing.textContent = value;
    return;
  }
  const doc = metadataEl.ownerDocument;
  // Match the DC namespace already on the document if possible.
  const dcNs = "http://purl.org/dc/elements/1.1/";
  const el = doc.createElementNS(dcNs, `dc:${localName}`);
  el.textContent = value;
  metadataEl.appendChild(el);
}
