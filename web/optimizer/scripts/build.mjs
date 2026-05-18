#!/usr/bin/env node
/**
 * esbuild driver. Builds two ESM bundles for browser consumption:
 *   - dist/optimizer.js       main pipeline + UI binding
 *   - dist/image-worker.js    dedicated worker script (no DOM)
 *
 * Production builds strip console + debugger and minify. `--dev` keeps
 * source maps + console statements + skips minification for quicker
 * iteration.
 */
import { build } from "esbuild";
import { mkdirSync, rmSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, "..");
const dist = join(root, "dist");
const dev = process.argv.includes("--dev");

rmSync(dist, { recursive: true, force: true });
mkdirSync(dist, { recursive: true });

/** Shared options for both bundles.
 *
 * Production builds drop source maps entirely. The optimizer page enforces
 * `connect-src 'none'` for the no-egress invariant, and Chrome DevTools
 * auto-fetches `<bundle>.map` on open — CSP correctly refuses that. Dev
 * builds keep external maps for debugging. */
const common = {
  bundle: true,
  format: "esm",
  target: ["chrome119", "edge119", "es2022"],
  platform: "browser",
  sourcemap: dev ? "external" : false,
  treeShaking: true,
  legalComments: "none",
  minify: !dev,
  drop: dev ? [] : ["console", "debugger"],
  define: {
    "process.env.NODE_ENV": JSON.stringify(dev ? "development" : "production"),
  },
  logLevel: "info",
};

await Promise.all([
  build({
    ...common,
    entryPoints: [join(root, "src/ui/index.ts")],
    outfile: join(dist, "optimizer.js"),
  }),
  build({
    ...common,
    entryPoints: [join(root, "src/image-worker.ts")],
    outfile: join(dist, "image-worker.js"),
    // Worker code only — no main-thread fallbacks for DOM.
    define: {
      ...common.define,
      "process.env.PAPERLOOM_WORKER": JSON.stringify("1"),
    },
  }),
]);

console.warn(`built ${dist}/optimizer.js + ${dist}/image-worker.js`);
