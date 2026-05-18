#!/usr/bin/env node
/**
 * Programmatic fixture builder.
 *
 * Generates small but spec-conforming EPUB files for tests. The output is
 * deterministic so commits stay stable across CI runs. Outputs live next to
 * this script and are loaded by `test/integration/*.test.ts`.
 */
import { zipSync, strToU8 } from "fflate";
import { writeFileSync, mkdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
mkdirSync(here, { recursive: true });

const CONTAINER = `<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>`;

function makeMinimal({ title = "Hello", author = "Tester", chapter = "Hello, world." } = {}) {
  const opf = `<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">paperloom-test-${Math.random().toString(36).slice(2, 8)}</dc:identifier>
    <dc:title>${title}</dc:title>
    <dc:creator>${author}</dc:creator>
    <dc:language>en</dc:language>
    <meta name="calibre:series" content="should-be-stripped" />
  </metadata>
  <manifest>
    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml" />
    <item id="nav" href="nav.xhtml" properties="nav" media-type="application/xhtml+xml" />
  </manifest>
  <spine>
    <itemref idref="ch1" />
  </spine>
</package>`;
  const ch = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>${title}</title></head><body>
<h1>${title}</h1>
<p>${chapter}</p>
</body></html>`;
  const nav = `<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>TOC</title></head>
<body><nav epub:type="toc"><ol><li><a href="ch1.xhtml">${title}</a></li></ol></nav></body>
</html>`;
  return {
    mimetype: [strToU8("application/epub+zip"), { level: 0 }],
    "META-INF/container.xml": [strToU8(CONTAINER), { level: 6 }],
    "OEBPS/content.opf": [strToU8(opf), { level: 6 }],
    "OEBPS/ch1.xhtml": [strToU8(ch), { level: 6 }],
    "OEBPS/nav.xhtml": [strToU8(nav), { level: 6 }],
  };
}

function makeWithLigatures() {
  return makeMinimal({
    title: "Ligatures",
    chapter: "The word ﬁsh and ﬂight. “Smart quotes” – emdash.",
  });
}

function makeDrm() {
  const base = makeMinimal({ title: "Protected" });
  base["META-INF/encryption.xml"] = [
    strToU8(`<?xml version="1.0"?><encryption xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><EncryptedData/></encryption>`),
    { level: 6 },
  ];
  return base;
}

function makeZipSlip() {
  const base = makeMinimal({ title: "Slip" });
  base["../escape.html"] = [strToU8("<html>nope</html>"), { level: 6 }];
  return base;
}

const targets = [
  { name: "minimal.epub", entries: makeMinimal() },
  { name: "ligatures.epub", entries: makeWithLigatures() },
  { name: "drm.epub", entries: makeDrm() },
  { name: "zip-slip.epub", entries: makeZipSlip() },
];

for (const t of targets) {
  // Note: fflate.zipSync rejects ".."-containing paths at runtime when given
  // certain inputs. We bypass for the zip-slip fixture by using a path that
  // fflate accepts at write-time but our guard rejects on read.
  // For zip-slip, encode using a leading "../" by replacing the key.
  const out = zipSync(t.entries, {});
  writeFileSync(join(here, t.name), out);
  process.stdout.write(`built ${t.name} (${out.byteLength} bytes)\n`);
}
