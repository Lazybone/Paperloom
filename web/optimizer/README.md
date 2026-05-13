# @paperloom/optimizer

Client-side TypeScript port of [b1rdmania/epubkit](https://github.com/b1rdmania/epubkit), targeted at the **Paperloom 960×540 16-gray e-paper display** (LilyGo T5 E-Paper S3 Pro / Pro Lite, epdiy + TPS65185 + ED047TC1).

## What this is

A static web tool that reads an EPUB in the browser, runs a 10-stage pipeline (DRM probe → structure parse → metadata clean → image dither + JPEG re-encode → HTML sanitize → CSS prune → font strip → text cleanup → TOC repair → re-zip) and hands the user a smaller, sharper EPUB ready to drop on the device's microSD card.

Nothing leaves the browser. There is no upload, no telemetry, no analytics.

## Build outputs

`npm run build` writes to the sibling `docs/optimizer/` directory consumed by GitHub Pages:

- `docs/optimizer/optimizer.js` — main ESM bundle (≤ 200 KB gzipped, enforced at 195 KB by `scripts/size-check.mjs`)
- `docs/optimizer/image-worker.js` — dedicated worker for off-main-thread image processing (≤ 25 KB gzipped)
- `docs/optimizer/ui.css` — optimizer-only styling that's larger than fits the page-local file

## Layout

```
src/
  index.ts                 Public entry: optimizeEpub()
  types.ts                 PixelPlane, OptimizeOptions, ProgressEvent
  errors.ts                Error classes + ERROR_MESSAGES constants
  image-worker.ts          Dedicated worker entry
  lib/
    epub-model.ts          In-memory representation
    paperloom-palette.ts   16-gray LUT [0,17,34,...,255]
    rtl-detect.ts          Latin-run-length + Unicode block heuristic
    floyd-steinberg.ts     Sierra-style error diffusion
    autocontrast.ts        Pillow-parity histogram clip
    image-path.ts          2x2 OffscreenCanvas x WebCodecs resolver
    filename-sanitize.ts   Safe download filename
    worker-protocol.ts     Worker message shapes
  pipeline/
    drm.ts                 META-INF/encryption.xml probe
    zip-guard.ts           Zip-slip path validation
    structure.ts           OPF + manifest + spine + nav
    metadata.ts            Dublin Core / EPUB3 metadata
    text.ts                Whitespace, ligatures, smart quotes, NFC
    html.ts                XHTML sanitize + re-serialize
    css.ts                 css-tree prune + minify
    fonts.ts               Strip @font-face + font files
    toc.ts                 Validate/regenerate NCX/nav
    artifacts.ts           Drop .DS_Store, Thumbs.db, __MACOSX/
    package.ts             mimetype-first re-zip
  ui/
    index.ts               Wires HTML hooks
    drag-drop.ts           Drop zone + file input
    queue.ts               Per-file queue rendering
    progress.ts            Step progress board
    download.ts            URL.createObjectURL handling
test/
  pipeline/*.test.ts
  integration/*.test.ts
  security/*.test.ts
  fixtures/                Spec-generated EPUBs for testing
  ui/queue.test.ts
scripts/
  build.mjs                esbuild driver
  size-check.mjs           Gate at 195 KB gz (main) / 25 KB gz (worker)
  copy-to-docs.mjs         Drop artifacts into docs/optimizer/
  bundle-baseline.mjs      Re-run the size baseline survey
```

## Scripts

```
npm install            # install deps
npm run build          # compile + size-check + copy to docs/optimizer/
npm test               # vitest single run
npm run test:watch     # vitest watch mode
npm run lint           # eslint
npm run fixtures       # regenerate test EPUBs from JSON specs
npm run baseline       # re-run the bundle-size baseline measurement
```

## Hardware target

Tuned for the LilyGo T5 E-Paper S3 Pro display (960×540 e-paper, 16 grayscale levels via the epdiy 4-bit waveform). Other readers may benefit but are not in scope; see `.codewright/palette-lut.md` for the LUT derivation.

## License

MIT, same as Paperloom. Upstream epubkit attribution at `LICENSES/epubkit-LICENSE` and in the optimizer footer.
