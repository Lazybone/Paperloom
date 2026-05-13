# Optimizer Smoke Checklist (WP-14)

Manual test pass before tagging a release. The vitest suite covers the textual pipeline; this checklist covers the bits vitest can't reach: real image pipeline, real browsers, and the Paperloom display itself.

## Setup

- [ ] Build green locally: `cd web/optimizer && npm run build`
- [ ] All 54+ unit + integration tests pass: `npm test`
- [ ] CI workflow green for the same commit
- [ ] Local server up: `python3 -m http.server 8765 --directory docs`

## 1. Page rendering

For each of `http://localhost:8765/`, `/flasher/`, `/optimizer/`:
- [ ] Loads without console errors or CSP violations
- [ ] Both light and dark themes look intentional (toggle OS pref)
- [ ] Masthead, hero, footer all render with shared design tokens
- [ ] Back-link from `/flasher/` and `/optimizer/` returns to hub
- [ ] DevTools Network panel: every request lands on the expected origin

## 2. Optimizer happy path (Chrome / Edge / Opera ≥ 119)

With a known-good public-domain EPUB (e.g. Gutenberg's `Pride and Prejudice`):
- [ ] Drag onto drop zone → file appears in the queue with title + size
- [ ] Click "Full Paperloom" preset → tile fills with accent
- [ ] Click "Optimize EPUBs" → button disables, progress board appears
- [ ] Each pipeline step fires "start" then "done" in order
- [ ] Result download appears as a pill button with the optimised filename
- [ ] Downloaded EPUB opens in Calibre without errors
- [ ] Downloaded EPUB renders on the Paperloom hardware with sharp text

## 3. Quick preset

- [ ] Pick "Quick" → only images + text get touched (font/CSS toggles greyed visually if Custom is used)
- [ ] Result file size is smaller than the input but larger than Full

## 4. Custom preset

- [ ] Pick "Custom"
- [ ] Toggle off "Generate Cover" → result has no `generated-cover.jpg`
- [ ] Toggle off "Remove Fonts" → result still contains the original font files

## 5. Light Novel mode

With an EPUB containing landscape spreads:
- [ ] Enable Light Novel mode
- [ ] Result has the spread split into two portrait pages in the spine

## 6. Failure modes

- [ ] Drop a DRM-protected EPUB → queue item shows the friendly DRM error message and does not crash the rest of the queue
- [ ] Drop a zip-slip fixture (`web/optimizer/test/fixtures/zip-slip.epub`) → friendly "unsafe path" error, no file leaks
- [ ] Drop a 200 MB EPUB on Chrome → optimisation completes (may be slow), no OOM
- [ ] Drop the same on iOS Safari → either completes or shows the slower-path banner before degrading gracefully

## 7. Fallback browsers (Safari / Firefox)

- [ ] Open `/optimizer/` → "Slower path detected" banner is visible
- [ ] Optimisation still works on a small EPUB; result downloads correctly
- [ ] No crash, no fatal console error

## 8. CSP enforcement

- [ ] DevTools Console: zero CSP violations on any of the three pages during a full optimisation run
- [ ] `connect-src` policy never blocks a legitimate fetch (firmware bin on the flasher page, none on the optimizer page)

## 9. Network egress invariant

In the optimizer:
- [ ] DevTools → Network → record full session → drop EPUB → optimise → download. Confirm zero outbound requests for user content. Only the initial page + bundle + Google Fonts loads count.

## 10. Bundle / size check

- [ ] `dist/optimizer.js.gz` ≤ 195 KB
- [ ] `dist/image-worker.js.gz` ≤ 25 KB
- [ ] `docs/index.html.gz` ≤ 30 KB
- [ ] `site/flasher/index.html.gz` ≤ 20 KB
- [ ] `site/optimizer/index.html.gz` ≤ 15 KB
- [ ] `site/shared/design.css.gz` ≤ 12 KB

## Sign-off

```
Tester:
Date:
Browser(s):
Device(s):
Notes:
```
