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
  result?: OptimizeResult;
  error?: Error;
}

export function bootstrap(): void {
  const dropZone = document.getElementById("drop-zone") as HTMLLabelElement | null;
  const fileInput = document.getElementById("file-input") as HTMLInputElement | null;
  const queueEl = document.getElementById("queue") as HTMLUListElement | null;
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
      };
      queue.push(entry);
      renderQueueItem(queueEl!, entry);
    }
    if (queue.length > 0) optimizeCta!.disabled = false;
  }

  async function drainQueue(): Promise<void> {
    if (running) return;
    running = true;
    optimizeCta!.disabled = true;
    if (progressEl) progressEl.hidden = false;
    try {
      for (const entry of queue) {
        if (entry.state !== "pending") continue;
        entry.state = "running";
        updateQueueState(entry);
        const opts = readOptions(optionsForm!);
        try {
          const result = await optimizeEpub(entry.file, opts, (ev) => {
            if (progressDetail) {
              progressDetail.textContent = formatProgress(entry.file.name, ev);
            }
            updateQueueProgress(entry, ev);
          });
          entry.state = "done";
          entry.result = result;
          appendDownload(downloadsEl, entry);
        } catch (err) {
          entry.state = "error";
          entry.error = err as Error;
        }
        updateQueueState(entry);
      }
    } finally {
      running = false;
      if (progressEl) progressEl.hidden = true;
      optimizeCta!.disabled = queue.every((e) => e.state !== "pending");
    }
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
  status.textContent = `${(entry.file.size / 1024 / 1024).toFixed(2)} MB · queued`;
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
  const status = li.querySelector(".qi-status") as HTMLElement | null;
  if (!status) return;
  status.dataset.state = entry.state;
  switch (entry.state) {
    case "running":
      status.textContent = "Optimizing…";
      break;
    case "done":
      status.textContent = entry.result
        ? `Done — ${(entry.result.stats.resultBytes / 1024 / 1024).toFixed(2)} MB · ${entry.result.stats.imagesProcessed} images`
        : "Done";
      break;
    case "error":
      status.textContent = entry.error ? userMessageForError(entry.error) : "Failed";
      break;
    default:
      status.textContent = `${(entry.file.size / 1024 / 1024).toFixed(2)} MB · queued`;
  }
}

function updateQueueProgress(entry: QueueEntry, ev: ProgressEvent): void {
  const li = document.getElementById(entry.id);
  if (!li) return;
  const fill = li.querySelector(".qi-bar > span") as HTMLElement | null;
  if (!fill) return;
  // 11 steps, distribute roughly evenly.
  const stepIndex = STEP_ORDER.indexOf(ev.step);
  if (stepIndex < 0) return;
  const stepPct = ev.phase === "done" ? 100 : ev.pct;
  const overall = ((stepIndex + stepPct / 100) / STEP_ORDER.length) * 100;
  fill.style.width = `${overall.toFixed(1)}%`;
}

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
