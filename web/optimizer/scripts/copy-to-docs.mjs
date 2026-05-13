#!/usr/bin/env node
/**
 * Copies the freshly-built bundles into ../../docs/optimizer/ so GitHub
 * Pages serves them next to the static HTML shell. We DO commit the
 * outputs — see WP-10 / plan note on pre-built artifacts.
 */
import { copyFileSync, mkdirSync, existsSync, unlinkSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
const repoRoot = join(root, "..", "..");

const FILES = [
  ["dist/optimizer.js", "docs/optimizer/optimizer.js"],
  ["dist/optimizer.js.map", "docs/optimizer/optimizer.js.map"],
  ["dist/image-worker.js", "docs/optimizer/image-worker.js"],
  ["dist/image-worker.js.map", "docs/optimizer/image-worker.js.map"],
];

mkdirSync(join(repoRoot, "docs/optimizer"), { recursive: true });

let copied = 0;
for (const [src, dst] of FILES) {
  const from = join(root, src);
  const to = join(repoRoot, dst);
  if (!existsSync(from)) {
    // Source missing — typical for production builds where source maps
    // are dropped entirely. Remove any stale copy at the destination so
    // browsers don't keep trying to fetch a map that violates CSP.
    if (existsSync(to)) {
      unlinkSync(to);
      console.warn(`copy-to-docs: removed stale ${dst}`);
    }
    continue;
  }
  copyFileSync(from, to);
  copied += 1;
  console.warn(`copied ${src} -> ${dst}`);
}
console.warn(`copy-to-docs: ${copied} file(s) copied`);
