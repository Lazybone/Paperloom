/**
 * DRM probe. Any presence of META-INF/encryption.xml or Adobe ADEPT markers
 * rejects the entire EPUB. We don't try to be smart about partial DRM —
 * that risks accidental bypass and creates a compliance gray zone.
 */
import { DrmDetectedError } from "../errors.js";

const TEXT_DECODER = new TextDecoder("utf-8");

export function detectDrm(entries: Map<string, Uint8Array>): void {
  const encryption = entries.get("META-INF/encryption.xml");
  if (encryption && encryption.byteLength > 0) {
    throw new DrmDetectedError("META-INF/encryption.xml present");
  }
  // Adobe ADEPT rights.xml: only present on Adobe-protected files.
  const rights = entries.get("META-INF/rights.xml");
  if (rights && rights.byteLength > 0) {
    const text = TEXT_DECODER.decode(rights);
    if (/<adept:rights\b/i.test(text) || /adept-rights\b/i.test(text)) {
      throw new DrmDetectedError("Adobe ADEPT rights.xml present");
    }
  }
}
