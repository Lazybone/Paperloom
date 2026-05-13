#!/usr/bin/env node
/**
 * Bundle-size gate.
 *
 * Reads dist/optimizer.js + dist/image-worker.js, gzips in memory, and
 * exits non-zero if either exceeds its budget.
 *
 * Budgets:
 *   - dist/optimizer.js.gz      ≤ 195 KB (200 KB ceiling minus 5 KB safety)
 *   - dist/image-worker.js.gz   ≤ 25 KB
 *
 * Also checks docs/-side static pages (gzipped) when run from CI after
 * copy-to-docs runs:
 *   - docs/index.html             ≤ 30 KB
 *   - docs/flasher/index.html     ≤ 20 KB
 *   - docs/optimizer/index.html   ≤ 15 KB
 *   - docs/shared/design.css      ≤ 12 KB
 *   - docs/shared/fonts.css       ≤ 3 KB
 */
import { readFileSync, existsSync } from "node:fs";
import { gzipSync } from "node:zlib";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
const repoRoot = join(root, "..", "..");

const KB = 1024;

/** [path, budgetBytes] tuples; the path is checked for existence. */
const TARGETS = [
  [join(root, "dist/optimizer.js"), 195 * KB],
  [join(root, "dist/image-worker.js"), 25 * KB],
  [join(repoRoot, "docs/index.html"), 30 * KB],
  [join(repoRoot, "docs/flasher/index.html"), 20 * KB],
  [join(repoRoot, "docs/optimizer/index.html"), 15 * KB],
  [join(repoRoot, "docs/shared/design.css"), 12 * KB],
  [join(repoRoot, "docs/shared/fonts.css"), 3 * KB],
];

let failed = 0;
let checked = 0;

for (const [target, budget] of TARGETS) {
  if (!existsSync(target)) continue;
  checked += 1;
  const raw = readFileSync(target);
  const gz = gzipSync(raw, { level: 9 });
  const ok = gz.byteLength <= budget;
  const status = ok ? "ok " : "FAIL";
  const rel = target.replace(`${repoRoot}/`, "");
  process.stdout.write(`  ${status}  ${rel.padEnd(36)}  raw=${pad(raw.byteLength)}  gz=${pad(gz.byteLength)}  budget=${pad(budget)}\n`);
  if (!ok) failed += 1;
}

if (checked === 0) {
  console.error("size-check: no targets found; did the build run?");
  process.exit(2);
}

if (failed > 0) {
  console.error(`size-check: ${failed} target(s) exceed budget.`);
  process.exit(1);
}
console.warn(`size-check: ${checked} target(s) within budget.`);

function pad(n) {
  return n.toString().padStart(6, " ");
}
