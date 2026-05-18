import { describe, it, expect } from "vitest";
import { ZipSlipDetectedError } from "../../src/errors.js";
import { assertSafeOutputPath, guardZipEntries } from "../../src/pipeline/zip-guard.js";

describe("guardZipEntries", () => {
  it("accepts safe relative paths", () => {
    expect(() => guardZipEntries({ "OEBPS/ch1.xhtml": new Uint8Array() })).not.toThrow();
  });
  it("rejects ../ traversal", () => {
    expect(() => guardZipEntries({ "../escape.html": new Uint8Array() })).toThrow(
      ZipSlipDetectedError,
    );
  });
  it("rejects absolute paths", () => {
    expect(() => guardZipEntries({ "/etc/passwd": new Uint8Array() })).toThrow(
      ZipSlipDetectedError,
    );
  });
  it("rejects ~ home paths", () => {
    expect(() => guardZipEntries({ "~/file": new Uint8Array() })).toThrow(
      ZipSlipDetectedError,
    );
  });
  it("rejects nested traversal", () => {
    expect(() => guardZipEntries({ "OEBPS/../etc": new Uint8Array() })).toThrow(
      ZipSlipDetectedError,
    );
  });
});

describe("assertSafeOutputPath", () => {
  it("is a noop on safe paths", () => {
    expect(() => assertSafeOutputPath("META-INF/container.xml")).not.toThrow();
  });
  it("throws on traversal", () => {
    expect(() => assertSafeOutputPath("../boom")).toThrow(ZipSlipDetectedError);
  });
});
