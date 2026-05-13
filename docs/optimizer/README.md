# Paperloom EPUB Optimizer

Drop an EPUB. Get back a smaller, sharper EPUB tuned for the Paperloom display (960×540, 16 grays). Everything happens inside this browser tab — no upload, no analytics, no server.

## What it does

A 10-stage pipeline runs on every file:

1. **DRM probe** — rejects encrypted EPUBs (no bypass)
2. **Structure parse** — reads OPF + manifest + spine + nav
3. **Metadata clean** — strips Calibre / Amazon / Kobo / iBooks tags
4. **Image optimise** — quantises to 16 grays via Floyd–Steinberg, autocontrast, baseline JPEG
5. **HTML sanitize** — drops scripts, audio/video, on*= handlers
6. **CSS prune** — removes unused selectors, minifies
7. **Font strip** — deletes embedded font files
8. **Text cleanup** — OCR-ligatures, smart quotes, mojibake, NFC
9. **TOC fix** — generates from headings when missing
10. **Repackage** — mimetype-first, stored not deflated

## Supported browsers

| Browser           | Path | Notes |
|-------------------|------|-------|
| Chrome 119+       | worker + WebCodecs | full speed |
| Edge 119+         | worker + WebCodecs | full speed |
| Opera 105+        | worker + WebCodecs | full speed |
| Safari ≤ 17       | main thread + Canvas2D | fallback; large books may stutter |
| Firefox (current) | main thread + Canvas2D | fallback; large books may stutter |

The page shows a banner when it falls back so you know what you're getting.

## Limits & known issues

- The optimiser is tuned for **Paperloom** (LilyGo T5 S3 Pro / Pro Lite). Other 960×540 e-readers should benefit; other resolutions get clamped down. Other grayscale depths get re-quantised to 16 levels.
- DRM-protected EPUBs are rejected. Remove DRM externally (Calibre + DeDRM) first.
- Light Novel mode rotates landscape spreads and splits double-page art; preview the result on a few books before bulk-running.
- All processing happens on a single browser tab. Multi-tab parallelism is not implemented.

## Source

Port of [b1rdmania/epubkit](https://github.com/b1rdmania/epubkit). The MIT licence text is at `../LICENSES/MIT-epubkit.txt`.

Source code: [`web/optimizer/`](https://github.com/Lazybone/Paperloom/tree/main/web/optimizer).

## Rebuilding

```bash
cd web/optimizer
npm install
npm run build       # esbuild + size-gate + copy to docs/optimizer/
npm test            # vitest
```

The CI workflow at `.github/workflows/optimizer-build.yml` does the same thing automatically.
