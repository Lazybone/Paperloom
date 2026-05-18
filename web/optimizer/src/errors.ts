/**
 * Centralized error classes + user-facing messages.
 *
 * Per HIGH-3 from the brainstorm: the UI binding layer imports
 * `ERROR_MESSAGES` directly and renders a translated message in the queue
 * item's error slot. Internal `error.message` strings are stable
 * (English, debug-oriented); `ERROR_MESSAGES` strings are the polished
 * user copy.
 */

export class DrmDetectedError extends Error {
  override name = "DrmDetectedError";
  readonly kind = "DRM" as const;
}

export class InvalidEpubError extends Error {
  override name = "InvalidEpubError";
  readonly kind = "INVALID_EPUB" as const;
}

export class MalformedZipError extends Error {
  override name = "MalformedZipError";
  readonly kind = "MALFORMED_ZIP" as const;
}

export class ZipSlipDetectedError extends Error {
  override name = "ZipSlipDetectedError";
  readonly kind = "ZIP_SLIP" as const;
}

export class ImageDecodeError extends Error {
  override name = "ImageDecodeError";
  readonly kind = "IMAGE_DECODE" as const;
}

export class OptimizerInternalError extends Error {
  override name = "OptimizerInternalError";
  readonly kind = "INTERNAL" as const;
}

export type KnownErrorKind =
  | "DRM"
  | "INVALID_EPUB"
  | "MALFORMED_ZIP"
  | "ZIP_SLIP"
  | "IMAGE_DECODE"
  | "INTERNAL";

/** User-facing messages keyed by error.kind. */
export const ERROR_MESSAGES = {
  DRM: "This EPUB is copy-protected. The optimizer can't read inside it. Try a DRM-free copy or use Calibre + the DeDRM plugin first.",
  INVALID_EPUB: "This file doesn't look like a valid EPUB. The manifest or container.xml is missing or malformed.",
  MALFORMED_ZIP: "The ZIP container couldn't be opened. The file is probably truncated or corrupt.",
  ZIP_SLIP: "The EPUB contains unsafe path entries (../ or absolute paths). It was rejected for safety.",
  IMAGE_DECODE: "One or more images could not be decoded. The optimizer kept the originals in place and continued.",
  INTERNAL: "Something went wrong inside the optimizer. The full trace is in your browser console.",
} as const satisfies Record<KnownErrorKind, string>;

/** Type guard that pulls .kind off our own error classes. */
export function isKnownError(
  err: unknown,
): err is { kind: KnownErrorKind; message: string } {
  return (
    err instanceof DrmDetectedError ||
    err instanceof InvalidEpubError ||
    err instanceof MalformedZipError ||
    err instanceof ZipSlipDetectedError ||
    err instanceof ImageDecodeError ||
    err instanceof OptimizerInternalError
  );
}

export function userMessageFor(err: unknown): string {
  if (isKnownError(err)) {
    return ERROR_MESSAGES[err.kind];
  }
  return ERROR_MESSAGES.INTERNAL;
}
