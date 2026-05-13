/**
 * Security: css-tree round-trip must not call eval/Function.
 *
 * Production CSP for the optimizer page is `script-src 'self'` with no
 * `'unsafe-eval'`. If css-tree ever introduced an eval/Function call,
 * the CSS step would crash at runtime under that CSP. This test pins the
 * invariant so a future upgrade of css-tree can't silently regress it.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import * as cssTree from "css-tree";

describe("[security] css-tree round-trip under strict CSP", () => {
  let evalSpy: ReturnType<typeof vi.spyOn>;
  let functionCtorSpy: ReturnType<typeof vi.spyOn>;
  const originalEval = globalThis.eval;
  const originalFunction = globalThis.Function;

  beforeEach(() => {
    evalSpy = vi.spyOn(globalThis, "eval");
    // Spy on the Function constructor's invocation by wrapping it.
    functionCtorSpy = vi.fn(originalFunction) as unknown as ReturnType<typeof vi.spyOn>;
    (globalThis as unknown as { Function: typeof Function }).Function =
      functionCtorSpy as unknown as typeof Function;
  });

  afterEach(() => {
    evalSpy.mockRestore();
    (globalThis as unknown as { eval: typeof eval }).eval = originalEval;
    (globalThis as unknown as { Function: typeof Function }).Function = originalFunction;
  });

  it("does not call eval or Function() during parse/walk/generate", () => {
    const css = `
      body { color: rgb(255, 0, 0); }
      .x { display: flex; gap: 1rem; }
      @font-face { font-family: x; src: url(x.woff2); }
      @media (prefers-color-scheme: dark) { body { color: #fff; } }
    `;
    const ast = cssTree.parse(css, { positions: false });
    cssTree.walk(ast, {
      visit: "Rule",
      enter: () => {},
    });
    const out = cssTree.generate(ast);
    expect(out.length).toBeGreaterThan(0);
    expect(evalSpy).not.toHaveBeenCalled();
    expect(functionCtorSpy).not.toHaveBeenCalled();
  });
});
