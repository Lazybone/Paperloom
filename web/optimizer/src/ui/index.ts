/**
 * UI binding layer for the Paperloom optimizer.
 *
 * Wires up the static HTML hooks shipped by M1 (drop-zone, queue, presets,
 * options form, metadata-panel, progress, downloads) to the public
 * `optimizeEpub` function.
 *
 * Design choices:
 *  - One file in the queue at a time runs through the pipeline; queue
 *    serialised so we don't blow heap on large books.
 *  - Per-file errors stay in the queue card; the queue continues.
 *  - Download blobs are kept alive on the queue card until the user
 *    clears them or the page is hidden.
 *
 * No globals leaked: `bootstrap()` is the single entry that the bundle
 * calls at load time.
 */
import { optimizeEpub, defaultOptions } from "../index.js";
import { ERROR_MESSAGES, isKnownError } from "../errors.js";
import { resolveImagePath, isWorkerPath } from "../lib/image-path.js";
import { PAPERLOOM_GRAY_PALETTE } from "../lib/paperloom-palette.js";
import { peekMetadata } from "../lib/peek-metadata.js";
import type {
  OptimizeOptions,
  OptimizeResult,
  Preset,
  ProgressEvent,
} from "../types.js";

interface QueueEntry {
  id: string;
  file: File;
  state: "pending" | "running" | "done" | "error";
  /** User-edited title (override). Empty string = use the EPUB's own value. */
  titleOverride: string;
  /** User-edited author (override). Empty string = use the EPUB's own value. */
  authorOverride: string;
  result?: OptimizeResult;
  error?: Error;
}

export function bootstrap(): void {
  const dropZone = document.getElementById("drop-zone") as HTMLLabelElement | null;
  const fileInput = document.getElementById("file-input") as HTMLInputElement | null;
  const queueEl = document.getElementById("queue") as HTMLUListElement | null;
  const fileCardsEl = document.getElementById("file-cards") as HTMLElement | null;
  const processingEl = document.getElementById("processing") as HTMLElement | null;
  // Legacy id="downloads" / "queue-summary" lookups — null when those
  // elements are absent (which is the case in the current layout).
  const downloadsEl = document.getElementById("downloads") as HTMLDivElement | null;
  const progressEl = document.getElementById("progress") as HTMLElement | null;
  const progressDetail = document.getElementById("progress-detail") as HTMLElement | null;
  const optionsForm = document.getElementById("options") as HTMLFormElement | null;
  const optimizeCta = document.getElementById("optimize-cta") as HTMLButtonElement | null;
  const browserHint = document.getElementById("browser-hint") as HTMLElement | null;
  const jpegQualityInput = document.getElementById("jpeg-quality") as HTMLInputElement | null;
  const jpegQualityOut = document.getElementById("jpeg-quality-out") as HTMLOutputElement | null;

  if (!dropZone || !fileInput || !queueEl || !optionsForm || !optimizeCta) {
    // Shell HTML missing — bail silently rather than throw on every page.
    console.warn("paperloom-optimizer: required DOM hooks missing; skipping bootstrap");
    return;
  }

  const queue: QueueEntry[] = [];
  let running = false;

  // Browser hint reveal.
  const path = resolveImagePath();
  if (browserHint && !isWorkerPath(path)) {
    browserHint.hidden = false;
  }

  // JPEG quality output binding.
  if (jpegQualityInput && jpegQualityOut) {
    const updateQuality = () => {
      jpegQualityOut.value = `${jpegQualityInput.value}%`;
    };
    jpegQualityInput.addEventListener("input", updateQuality);
    updateQuality();
  }
  // Sync quality preset radios -> slider.
  optionsForm.addEventListener("change", (ev) => {
    const target = ev.target as HTMLInputElement;
    if (target.name === "qualityPreset" && jpegQualityInput && jpegQualityOut) {
      jpegQualityInput.value = target.value;
      jpegQualityOut.value = `${target.value}%`;
    }
  });

  // Drop zone drag-drop + file picker.
  ["dragenter", "dragover"].forEach((evt) => {
    dropZone.addEventListener(evt, (e) => {
      e.preventDefault();
      dropZone.dataset.drag = "active";
    });
  });
  ["dragleave", "drop"].forEach((evt) => {
    dropZone.addEventListener(evt, (e) => {
      e.preventDefault();
      dropZone.dataset.drag = "idle";
    });
  });
  dropZone.addEventListener("drop", (e) => {
    const files = e.dataTransfer?.files;
    if (!files) return;
    addFiles(Array.from(files));
  });
  fileInput.addEventListener("change", () => {
    if (!fileInput.files) return;
    addFiles(Array.from(fileInput.files));
  });

  optionsForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    if (running) return;
    await drainQueue();
  });

  function addFiles(files: File[]): void {
    for (const file of files) {
      if (!file.name.toLowerCase().endsWith(".epub")) continue;
      const entry: QueueEntry = {
        id: `q-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`,
        file,
        state: "pending",
        titleOverride: "",
        authorOverride: "",
      };
      queue.push(entry);
      void renderFileCard(entry);
    }
    if (queue.length > 0) optimizeCta!.disabled = false;
  }

  async function renderFileCard(entry: QueueEntry): Promise<void> {
    if (!fileCardsEl) return;
    const card = document.createElement("article");
    card.className = "file-card";
    card.id = `fc-${entry.id}`;

    const thumb = document.createElement("div");
    thumb.className = "fc-thumb";
    thumb.innerHTML = `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 4.5C4 3.7 4.7 3 5.5 3H11v18H5.5C4.7 21 4 20.3 4 19.5V4.5Z"/><path d="M20 4.5C20 3.7 19.3 3 18.5 3H13v18h5.5c.8 0 1.5-.7 1.5-1.5V4.5Z"/></svg>`;

    const body = document.createElement("div");
    body.className = "fc-body";

    const name = document.createElement("div");
    name.className = "fc-name";
    name.title = entry.file.name;
    name.textContent = entry.file.name;

    const meta = document.createElement("div");
    meta.className = "fc-meta";
    meta.textContent = `${(entry.file.size / 1024 / 1024).toFixed(2)} MB`;

    const inputs = document.createElement("div");
    inputs.className = "fc-inputs";
    const titleInput = document.createElement("input");
    titleInput.type = "text";
    titleInput.placeholder = "Title";
    titleInput.autocomplete = "off";
    titleInput.addEventListener("input", () => { entry.titleOverride = titleInput.value; });
    const authorInput = document.createElement("input");
    authorInput.type = "text";
    authorInput.placeholder = "Author";
    authorInput.autocomplete = "off";
    authorInput.addEventListener("input", () => { entry.authorOverride = authorInput.value; });
    inputs.append(titleInput, authorInput);

    body.append(name, meta, inputs);

    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "fc-remove";
    remove.setAttribute("aria-label", `Remove ${entry.file.name} from queue`);
    remove.textContent = "×";
    remove.addEventListener("click", () => {
      const idx = queue.indexOf(entry);
      if (idx >= 0) queue.splice(idx, 1);
      card.remove();
      document.getElementById(entry.id)?.remove();
      optimizeCta!.disabled = queue.length === 0;
    });

    card.append(thumb, body, remove);
    fileCardsEl.appendChild(card);

    // Background metadata + cover peek. Failures fall back to the
    // generic icon + filename; we don't block the user.
    try {
      const peek = await peekMetadata(entry.file);
      if (peek.title) {
        titleInput.value = peek.title;
        entry.titleOverride = peek.title;
        name.textContent = peek.title;
        name.title = peek.title;
      }
      if (peek.author) {
        authorInput.value = peek.author;
        entry.authorOverride = peek.author;
      }
      if (peek.cover) {
        const url = URL.createObjectURL(new Blob([new Uint8Array(peek.cover.bytes)], { type: peek.cover.mimeType }));
        thumb.replaceChildren();
        const img = document.createElement("img");
        img.src = url;
        img.alt = "";
        img.loading = "lazy";
        img.decoding = "async";
        img.addEventListener("load", () => URL.revokeObjectURL(url), { once: true });
        thumb.appendChild(img);
        thumb.classList.add("fc-thumb--has-cover");
      }
    } catch {
      // Preview failed — keep the placeholder. Optimisation can still run.
    }
  }

  async function drainQueue(): Promise<void> {
    if (running) return;
    running = true;
    optimizeCta!.disabled = false; // keep clickable so user sees the spinner
    optimizeCta!.dataset.state = "running";
    setCtaLabel("Processing…");
    if (progressEl) progressEl.hidden = false;
    if (processingEl) processingEl.hidden = false;
    let scrolled = false;
    try {
      for (const entry of queue) {
        if (entry.state !== "pending") continue;
        if (!document.getElementById(entry.id)) {
          renderQueueItem(queueEl!, entry);
        }
        if (!scrolled) {
          processingEl?.scrollIntoView({ behavior: "smooth", block: "start" });
          scrolled = true;
        }
        entry.state = "running";
        updateQueueState(entry);
        const opts = readOptions(optionsForm!);
        // Per-file metadata override from the file card inputs.
        const metadata = {
          ...(entry.titleOverride.trim() ? { title: entry.titleOverride.trim() } : {}),
          ...(entry.authorOverride.trim() ? { author: entry.authorOverride.trim() } : {}),
        };
        const perFileOpts = Object.keys(metadata).length > 0 ? { ...opts, metadata } : opts;
        try {
          const result = await optimizeEpub(entry.file, perFileOpts, (ev) => {
            if (progressDetail) {
              progressDetail.textContent = formatProgress(entry.file.name, ev);
            }
            updateQueueProgress(entry, ev);
          });
          entry.state = "done";
          entry.result = result;
          appendDownloadToQueueItem(entry);
          if (downloadsEl) appendDownload(downloadsEl, entry); // legacy block, if present
        } catch (err) {
          entry.state = "error";
          entry.error = err as Error;
        }
        updateQueueState(entry);
      }
    } finally {
      running = false;
      if (progressEl) progressEl.hidden = true;
      optimizeCta!.dataset.state = "";
      setCtaLabel("Optimize EPUBs");
      optimizeCta!.disabled = queue.every((e) => e.state !== "pending");
    }
  }

  function setCtaLabel(text: string): void {
    const label = optimizeCta!.querySelector(".cta-label");
    if (label) label.textContent = text;
  }

  window.addEventListener("beforeunload", (e) => {
    if (running) {
      e.preventDefault();
      e.returnValue = "Optimization in progress. Leave the page?";
    }
  });
}

export function readOptions(form: HTMLFormElement): OptimizeOptions {
  const fd = new FormData(form);
  const preset = (fd.get("preset") as Preset | null) ?? "full";
  const base = defaultOptions();
  return {
    ...base,
    preset,
    features: {
      contrastBoost: fd.get("contrastBoost") === "on" || base.features.contrastBoost,
      removeFonts: fd.get("removeFonts") === "on" || base.features.removeFonts,
      cleanCss: fd.get("cleanCss") === "on" || base.features.cleanCss,
      generateCover: fd.get("generateCover") === "on" || base.features.generateCover,
      cleanMetadata: fd.get("cleanMetadata") === "on" || base.features.cleanMetadata,
      textCleanup: fd.get("textCleanup") === "on" || base.features.textCleanup,
      lightNovel: fd.get("lightNovel") === "on",
    },
    target: { ...base.target, palette: PAPERLOOM_GRAY_PALETTE },
    jpegQuality: clampQuality(Number(fd.get("jpegQuality") ?? base.jpegQuality)),
    dither: "floyd-steinberg",
  };
}

function clampQuality(n: number): number {
  if (Number.isNaN(n)) return 78;
  return Math.max(40, Math.min(95, Math.round(n)));
}

function renderQueueItem(parent: HTMLUListElement, entry: QueueEntry): void {
  const li = document.createElement("li");
  li.id = entry.id;
  li.dataset.state = entry.state;
  const title = document.createElement("div");
  title.className = "qi-title";
  title.textContent = entry.file.name;
  const status = document.createElement("div");
  status.className = "qi-status";
  status.dataset.state = entry.state;
  const statusLabel = document.createElement("span");
  statusLabel.className = "qi-label";
  statusLabel.textContent = `${(entry.file.size / 1024 / 1024).toFixed(2)} MB · queued`;
  const statusPct = document.createElement("span");
  statusPct.className = "qi-pct";
  statusPct.textContent = "";
  status.append(statusLabel, statusPct);
  const bar = document.createElement("div");
  bar.className = "qi-bar";
  const fill = document.createElement("span");
  bar.appendChild(fill);
  li.append(title, status, bar);
  parent.appendChild(li);
}

function updateQueueState(entry: QueueEntry): void {
  const li = document.getElementById(entry.id);
  if (!li) return;
  li.dataset.state = entry.state;
  const status = li.querySelector(".qi-status") as HTMLElement | null;
  const label = li.querySelector(".qi-label") as HTMLElement | null;
  const pct = li.querySelector(".qi-pct") as HTMLElement | null;
  if (!status || !label) return;
  status.dataset.state = entry.state;
  switch (entry.state) {
    case "running":
      label.textContent = "Starting…";
      if (pct) pct.textContent = "0%";
      break;
    case "done":
      label.textContent = entry.result
        ? `Done — ${(entry.result.stats.resultBytes / 1024 / 1024).toFixed(2)} MB · ${entry.result.stats.imagesProcessed} images`
        : "Done";
      if (pct) pct.textContent = "100%";
      break;
    case "error":
      label.textContent = entry.error ? userMessageForError(entry.error) : "Failed";
      if (pct) pct.textContent = "";
      break;
    default:
      label.textContent = `${(entry.file.size / 1024 / 1024).toFixed(2)} MB · queued`;
      if (pct) pct.textContent = "";
  }
}

function updateQueueProgress(entry: QueueEntry, ev: ProgressEvent): void {
  const li = document.getElementById(entry.id);
  if (!li) return;
  const fill = li.querySelector(".qi-bar > span") as HTMLElement | null;
  const label = li.querySelector(".qi-label") as HTMLElement | null;
  const pct = li.querySelector(".qi-pct") as HTMLElement | null;
  if (!fill) return;
  // 11 steps, distribute roughly evenly.
  const stepIndex = STEP_ORDER.indexOf(ev.step);
  if (stepIndex < 0) return;
  const stepPct = ev.phase === "done" ? 100 : ev.pct;
  const overall = ((stepIndex + stepPct / 100) / STEP_ORDER.length) * 100;
  fill.style.width = `${overall.toFixed(1)}%`;
  if (label) {
    const stepLabel = STEP_LABELS[ev.step] ?? ev.step;
    const detail = ev.detail ? ` · ${ev.detail}` : "";
    label.textContent = `${stepLabel}${detail}`;
  }
  if (pct) pct.textContent = `${Math.round(overall)}%`;
}

const STEP_LABELS: Record<string, string> = {
  drm: "Checking for DRM",
  structure: "Reading EPUB structure",
  metadata: "Cleaning metadata",
  images: "Quantizing images",
  html: "Sanitizing HTML",
  css: "Pruning CSS",
  fonts: "Stripping fonts",
  text: "Cleaning text",
  toc: "Fixing TOC",
  artifacts: "Removing OS junk",
  package: "Re-packaging",
};

function appendDownload(parent: HTMLDivElement | null, entry: QueueEntry): void {
  if (!parent || !entry.result) return;
  const a = document.createElement("a");
  const url = URL.createObjectURL(entry.result.blob);
  a.href = url;
  a.download = entry.result.filename;
  a.textContent = `↓ ${entry.result.filename}`;
  a.addEventListener(
    "click",
    () => {
      // Revoke shortly after the click to give the browser time to start
      // the download.
      setTimeout(() => URL.revokeObjectURL(url), 30_000);
    },
    { once: true },
  );
  parent.appendChild(a);
}

function appendDownloadToQueueItem(entry: QueueEntry): void {
  if (!entry.result) return;
  const li = document.getElementById(entry.id);
  if (!li) return;
  // Avoid stacking duplicates if updateQueueState fires twice.
  const existing = li.querySelector(".qi-download");
  if (existing) existing.remove();
  const a = document.createElement("a");
  a.className = "qi-download";
  const url = URL.createObjectURL(entry.result.blob);
  a.href = url;
  a.download = entry.result.filename;
  a.textContent = `Download ${entry.result.filename}`;
  a.addEventListener(
    "click",
    () => setTimeout(() => URL.revokeObjectURL(url), 30_000),
    { once: true },
  );
  li.appendChild(a);
}

function userMessageForError(err: Error): string {
  if (isKnownError(err)) return ERROR_MESSAGES[err.kind];
  return ERROR_MESSAGES.INTERNAL;
}

function formatProgress(filename: string, ev: ProgressEvent): string {
  const detail = ev.detail ? ` — ${ev.detail}` : "";
  return `${filename}: ${ev.step}${detail}`;
}

const STEP_ORDER = [
  "drm",
  "structure",
  "metadata",
  "images",
  "html",
  "css",
  "fonts",
  "text",
  "toc",
  "artifacts",
  "package",
] as const;

// Auto-bootstrap when imported as the page entry.
if (typeof document !== "undefined" && document.readyState !== "loading") {
  bootstrap();
} else if (typeof document !== "undefined") {
  document.addEventListener("DOMContentLoaded", bootstrap, { once: true });
}
