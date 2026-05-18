/**
 * CSS pipeline step.
 *
 * For every CSS asset referenced by the manifest:
 *  - Parse with css-tree.
 *  - Collect the universe of used class names, ids, and tag names from the
 *    spine XHTML documents.
 *  - Drop rules whose simple selectors never match.
 *  - Drop @font-face blocks (font removal happens in pipeline/fonts).
 *  - Re-serialize and write back.
 *
 * css-tree is pure JS, no eval/Function; verified via the dedicated security
 * test (test/security/css-strict-csp.test.ts).
 */
import * as cssTree from "css-tree";
import type { EpubModel } from "../lib/epub-model.js";
import { resolveHref } from "../lib/epub-model.js";
import type { OptimizeWarning } from "../types.js";

const ENCODER = new TextEncoder();
const DECODER = new TextDecoder("utf-8");

export function pruneCss(model: EpubModel, warnings: OptimizeWarning[]): void {
  const used = collectUsedSelectors(model);
  let droppedRules = 0;
  let droppedFontFaces = 0;
  let cssCount = 0;

  for (const item of model.manifest.values()) {
    if (!isCss(item.mediaType, item.href)) continue;
    const path = resolveHref(model, item.href);
    const bytes = model.entries.get(path);
    if (!bytes) continue;
    cssCount += 1;

    let ast: cssTree.CssNode;
    try {
      ast = cssTree.parse(DECODER.decode(bytes), { positions: false });
    } catch (err) {
      warnings.push({
        step: "css",
        code: "CSS_PARSE_ERROR",
        message: `css-tree failed on ${path}: ${(err as Error).message}; left untouched`,
      });
      continue;
    }

    cssTree.walk(ast, {
      visit: "Atrule",
      enter: (node, ruleListItem, ruleList) => {
        if (node.name.toLowerCase() === "font-face" && ruleList) {
          ruleList.remove(ruleListItem!);
          droppedFontFaces += 1;
        }
      },
    });

    cssTree.walk(ast, {
      visit: "Rule",
      enter: (node, ruleListItem, ruleList) => {
        if (!ruleListItem || !ruleList) return;
        const prelude = node.prelude;
        if (prelude.type !== "SelectorList") return;
        // Drop the rule iff every selector is unused.
        const anyUsed = prelude.children.some((selector) =>
          selectorMaybeMatches(selector, used),
        );
        if (!anyUsed) {
          ruleList.remove(ruleListItem);
          droppedRules += 1;
        }
      },
    });

    const generated = cssTree.generate(ast);
    model.entries.set(path, ENCODER.encode(generated));
  }

  if (cssCount === 0) {
    warnings.push({
      step: "css",
      code: "CSS_NONE",
      message: "No CSS files in the manifest; pruning skipped.",
    });
  } else {
    warnings.push({
      step: "css",
      code: "CSS_PRUNE_STATS",
      message: `Dropped ${droppedRules} rule(s) and ${droppedFontFaces} @font-face block(s) across ${cssCount} stylesheet(s).`,
    });
  }
}

interface UsedSelectors {
  classes: Set<string>;
  ids: Set<string>;
  tags: Set<string>;
}

function collectUsedSelectors(model: EpubModel): UsedSelectors {
  const out: UsedSelectors = {
    classes: new Set(),
    ids: new Set(),
    tags: new Set(),
  };
  for (const item of model.spine) {
    const manifest = model.manifest.get(item.idref);
    if (!manifest) continue;
    if (!/xhtml|html/i.test(manifest.mediaType) && !/\.x?html?$/i.test(manifest.href)) {
      continue;
    }
    const path = resolveHref(model, manifest.href);
    const bytes = model.entries.get(path);
    if (!bytes) continue;
    const doc = new DOMParser().parseFromString(DECODER.decode(bytes), "application/xhtml+xml");
    const root = doc.querySelector("parsererror")
      ? new DOMParser().parseFromString(DECODER.decode(bytes), "text/html").documentElement
      : doc.documentElement;
    walkUsage(root, out);
  }
  return out;
}

function walkUsage(root: Element | null, out: UsedSelectors): void {
  if (!root) return;
  const walker = root.ownerDocument!.createTreeWalker(root, /* SHOW_ELEMENT */ 1);
  let node = walker.nextNode() as Element | null;
  out.tags.add(root.tagName.toLowerCase());
  while (node) {
    out.tags.add(node.tagName.toLowerCase());
    const classes = node.getAttribute("class");
    if (classes) {
      for (const cls of classes.split(/\s+/)) {
        if (cls) out.classes.add(cls);
      }
    }
    const id = node.getAttribute("id");
    if (id) out.ids.add(id);
    node = walker.nextNode() as Element | null;
  }
}

function selectorMaybeMatches(selector: cssTree.CssNode, used: UsedSelectors): boolean {
  // Be conservative: if the selector contains a combinator we don't model,
  // assume it could match. We only drop rules we're sure are unused.
  if (selector.type !== "Selector") return true;
  let hasSimple = false;
  for (const child of selector.children) {
    switch (child.type) {
      case "ClassSelector":
        hasSimple = true;
        if (used.classes.has(child.name)) return true;
        break;
      case "IdSelector":
        hasSimple = true;
        if (used.ids.has(child.name)) return true;
        break;
      case "TypeSelector":
        hasSimple = true;
        if (used.tags.has(child.name.toLowerCase())) return true;
        break;
      case "PseudoElementSelector":
      case "PseudoClassSelector":
      case "AttributeSelector":
      case "Combinator":
        // Unknown signal — keep the rule.
        return true;
      default:
        break;
    }
  }
  // If the selector consisted entirely of pseudo classes / attribute matchers,
  // hasSimple is false and we'd be wrong to drop it.
  return !hasSimple;
}

function isCss(mediaType: string, href: string): boolean {
  if (/css/i.test(mediaType)) return true;
  return /\.css$/i.test(href);
}
