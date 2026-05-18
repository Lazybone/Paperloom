/**
 * Pillow-parity autocontrast.
 *
 * Port of Pillow's `ImageOps.autocontrast(image, cutoff=0)` against a single
 * grayscale plane. Computes a histogram, clips `cutoff` percent off each tail,
 * then linearly stretches the remaining range to fill 0..255.
 *
 * Pillow's algorithm (simplified):
 *   - count pixels per bucket [0..255]
 *   - find low := smallest i where sum_{0..i} > cutoff * total
 *   - find high := largest i where sum_{i..255} > cutoff * total
 *   - if low >= high: identity
 *   - else: scale = 255 / (high - low); v' = clamp((v - low) * scale)
 *
 * After autocontrast we apply a 1.5x contrast multiplier around the
 * mid-gray (matches Pillow's `ImageEnhance.Contrast(1.5)`).
 */

export interface AutocontrastResult {
  data: Uint8ClampedArray;
  /** Diagnostics; useful in tests but optional. */
  lowBucket: number;
  highBucket: number;
  rangeBefore: number;
  rangeAfter: number;
}

export function autocontrastGrayscale(
  src: Uint8ClampedArray,
  cutoff = 0,
): AutocontrastResult {
  const hist = new Uint32Array(256);
  for (let i = 0; i < src.length; i += 1) {
    hist[src[i]!]! += 1;
  }
  const total = src.length;
  const cut = Math.max(0, Math.min(1, cutoff)) * total;

  let low = 0;
  let cumulative = 0;
  while (low < 255) {
    cumulative += hist[low]!;
    if (cumulative > cut) break;
    low += 1;
  }

  let high = 255;
  cumulative = 0;
  while (high > 0) {
    cumulative += hist[high]!;
    if (cumulative > cut) break;
    high -= 1;
  }

  const dst = new Uint8ClampedArray(src.length);
  if (low >= high) {
    dst.set(src);
    return {
      data: dst,
      lowBucket: low,
      highBucket: high,
      rangeBefore: high - low,
      rangeAfter: 255,
    };
  }

  const scale = 255 / (high - low);
  for (let i = 0; i < src.length; i += 1) {
    const v = src[i]!;
    dst[i] = (v - low) * scale;
  }

  return {
    data: dst,
    lowBucket: low,
    highBucket: high,
    rangeBefore: high - low,
    rangeAfter: 255,
  };
}

/** Pillow ImageEnhance.Contrast(factor) around mid-gray (128). */
export function contrastBoost(src: Uint8ClampedArray, factor: number): Uint8ClampedArray {
  if (factor === 1) return src;
  const dst = new Uint8ClampedArray(src.length);
  for (let i = 0; i < src.length; i += 1) {
    const v = src[i]!;
    dst[i] = (v - 128) * factor + 128;
  }
  return dst;
}
