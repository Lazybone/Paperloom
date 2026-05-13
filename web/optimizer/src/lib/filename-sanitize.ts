/**
 * Sanitize a filename for the download blob.
 *
 * - Replace path separators and traversal sequences.
 * - Strip control characters and reserved characters across Win/macOS/Linux.
 * - Collapse whitespace.
 * - Cap at 240 chars (Mac+ext leaves us headroom under the 255-byte limit).
 * - Refuse the empty/punctuation-only result; fall back to "paperloom.epub".
 */
// Reserved across Windows / macOS / Linux conventions. Spaces and dashes
// are explicitly fine in filenames; control characters are handled in a
// separate pass to avoid regex character-class corner cases.
const RESERVED = /[<>:"/\\|?*]/g;
// eslint-disable-next-line no-control-regex -- ASCII control range, by design
const CONTROL = /[\x00-\x1f\x7f]/g;

export function sanitizeFilename(input: string): string {
  let out = input.normalize("NFC");
  out = out.replace(/\.\./g, "_");
  out = out.replace(RESERVED, "_");
  out = out.replace(CONTROL, "");
  out = out.replace(/\s+/g, " ").trim();
  if (out.length > 240) {
    const dot = out.lastIndexOf(".");
    if (dot > 0 && out.length - dot < 12) {
      out = out.slice(0, 240 - (out.length - dot)) + out.slice(dot);
    } else {
      out = out.slice(0, 240);
    }
  }
  // Fallback when the entire string was reduced to punctuation/junk
  // (no alphanumeric character survives).
  if (!out || !/[A-Za-z0-9]/.test(out)) return "paperloom.epub";
  return out;
}
