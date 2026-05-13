/**
 * Zip-slip guard. Rejects any ZIP entry whose path tries to escape its root.
 *
 * Runs before structure parsing so a malicious EPUB can't smuggle relative
 * traversal entries into the EpubModel. See CRIT-2 from the brainstorm.
 *
 * Even though we never write to disk (everything stays in memory), the
 * resulting blob handed to the user could carry traversal references that
 * trip downstream readers. We refuse the input outright.
 */
import { ZipSlipDetectedError } from "../errors.js";

const UNSAFE_PATH = /(^|\/)\.\.(\/|$)|^[/\\]|^[a-zA-Z]:[/\\]|^~/;

export function guardZipEntries(entries: Record<string, Uint8Array>): void {
  for (const path of Object.keys(entries)) {
    if (UNSAFE_PATH.test(path) || path.includes("\\..\\")) {
      throw new ZipSlipDetectedError(`unsafe ZIP entry path: ${path}`);
    }
  }
}

/** Same check but on an iterable of paths, used by the re-zip step. */
export function assertSafeOutputPath(path: string): void {
  if (UNSAFE_PATH.test(path) || path.includes("\\..\\")) {
    throw new ZipSlipDetectedError(`unsafe output path: ${path}`);
  }
}
