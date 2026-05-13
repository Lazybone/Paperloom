#!/usr/bin/env node
/**
 * Re-run the bundle-baseline survey from WP-0.4.
 *
 * Builds a minimal entry that imports the heaviest runtime deps
 * (fflate + css-tree) and measures raw + gzipped + brotli sizes. Useful
 * before upgrading either dependency to re-check the budget headroom.
 */
import { build } from "esbuild";
import { readFileSync, writeFileSync, rmSync, mkdtempSync } from "node:fs";
import { gzipSync, brotliCompressSync } from "node:zlib";
import { tmpdir } from "node:os";
import { join } from "node:path";

const tmp = mkdtempSync(join(tmpdir(), "paperloom-baseline-"));
const entry = join(tmp, "entry.ts");
writeFileSync(
  entry,
  `import { unzipSync, zipSync, strFromU8, strToU8 } from "fflate";
   import { parse, walk, generate } from "css-tree";
   export function probe(bytes) {
     const e = unzipSync(bytes);
     const ast = parse(strFromU8(e["styles/main.css"] || new Uint8Array()));
     walk(ast, () => {});
     return zipSync({ "out.css": strToU8(generate(ast)) });
   }`,
);
await build({
  entryPoints: [entry],
  outfile: join(tmp, "bundle.js"),
  bundle: true,
  minify: true,
  format: "esm",
  target: ["es2022"],
  logLevel: "warning",
});
const bytes = readFileSync(join(tmp, "bundle.js"));
const gz = gzipSync(bytes, { level: 9 });
const br = brotliCompressSync(bytes);
console.warn("baseline result (fflate + css-tree):");
console.warn(`  raw     = ${bytes.byteLength}`);
console.warn(`  gzipped = ${gz.byteLength}`);
console.warn(`  brotli  = ${br.byteLength}`);
rmSync(tmp, { recursive: true });
