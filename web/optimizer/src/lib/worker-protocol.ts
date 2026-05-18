/**
 * Wire format for the image worker.
 *
 * Both directions use transferable ArrayBuffers; the main thread posts
 * EncodeRequest with the raw image bytes and gets EncodeResponse back with
 * the dithered + encoded JPEG bytes. One outstanding request per id.
 */

export interface EncodeRequest {
  id: number;
  op: "encode";
  bytes: ArrayBuffer;
  srcMime: string;
  /** Target dimensions; image is resized to fit while preserving aspect. */
  maxWidth: number;
  maxHeight: number;
  palette: Uint8Array;
  jpegQuality: number; // 0..100
  dither: "floyd-steinberg" | "none";
  contrastBoost: boolean;
}

export interface EncodeResponseOk {
  id: number;
  ok: true;
  bytes: ArrayBuffer;
  width: number;
  height: number;
  /** Image format actually written. Always "image/jpeg" for now. */
  outMime: "image/jpeg";
}

export interface EncodeResponseErr {
  id: number;
  ok: false;
  error: string;
  errorKind: "decode" | "encode" | "internal";
}

export type EncodeResponse = EncodeResponseOk | EncodeResponseErr;
