#include "wifi_upload.h"
#include "config.h"
#include "settings.h"
#include "display.h"        // FONT_FAMILY_COUNT for /api/settings validation
#include "frontlight.h"
#include "button_action.h"
#include "reader.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <ArduinoJson.h>

static WebServer _server(80);
static bool _running = false;
static File _uploadFile;
static BookReader* _reader = nullptr;

// Connection state tracking
static bool _connecting = false;
static bool _error = false;
static String _errorMsg;
static unsigned long _connectStart = 0;
static const unsigned long CONNECT_TIMEOUT_MS = 15000;

void wifi_upload_set_reader(BookReader* reader) { _reader = reader; }

// ─── HTML / JS payload (PROGMEM) ──────────────────────────────────
static const char UPLOAD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Paperloom</title>
<style>
  :root {
    --bg:           #f5f3ee;     /* warm paper */
    --bg-deep:      #ece8df;
    --card:         #ffffff;
    --text:         #1c1917;     /* near-black ink */
    --muted:        #78716c;
    --accent:       #1c1917;
    --accent-soft:  #292524;
    --brand:        #b45309;     /* warm amber, e-reader vibe */
    --brand-soft:   #fef3c7;
    --danger:       #b91c1c;
    --danger-hover: #dc2626;
    --border:       #e7e5e0;
    --hover:        #fafaf7;
    --shadow-sm:    0 1px 2px rgba(28,25,23,0.04);
    --shadow-md:    0 4px 16px rgba(28,25,23,0.06);
    --radius:       10px;
  }
  * { box-sizing: border-box; }
  html, body { height: 100%; }
  body {
    font-family: ui-serif, "Charter", "Iowan Old Style", "Cambria", Georgia, serif;
    margin: 0; background: var(--bg); color: var(--text);
    -webkit-font-smoothing: antialiased;
  }
  .ui {
    font-family: -apple-system, BlinkMacSystemFont, "Inter", "Segoe UI", system-ui, sans-serif;
  }

  /* ─── Header ─────────────────────────────────────────── */
  header {
    background: var(--accent); color: #fff;
    padding: 14px 28px;
    display: flex; align-items: center; gap: 14px;
    box-shadow: var(--shadow-sm);
  }
  header .logo {
    width: 30px; height: 30px; border-radius: 6px;
    background: var(--brand);
    display: grid; place-items: center;
    color: #fff; font-weight: 700; font-family: ui-serif, Georgia, serif; font-size: 17px;
    box-shadow: inset 0 0 0 1px rgba(255,255,255,0.08);
  }
  header h1 { margin: 0; font-size: 18px; font-weight: 600; letter-spacing: 0.2px; font-family: ui-serif, "Charter", Georgia, serif; }
  header .sub { margin-left: auto; font-size: 12px; color: rgba(255,255,255,0.55); font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; letter-spacing: 0.5px; text-transform: uppercase; }

  /* ─── Tabs ───────────────────────────────────────────── */
  nav {
    background: var(--card);
    border-bottom: 1px solid var(--border);
    display: flex; padding: 0 20px;
  }
  nav button {
    background: transparent; border: 0; color: var(--muted);
    padding: 14px 22px; font-size: 14px; cursor: pointer;
    font-family: inherit; letter-spacing: 0.3px;
    border-bottom: 2px solid transparent; margin-bottom: -1px;
    transition: color 0.15s;
  }
  nav button:hover { color: var(--text); }
  nav button.active { color: var(--text); border-bottom-color: var(--brand); font-weight: 600; }

  main { max-width: 920px; margin: 0 auto; padding: 28px 20px 80px; }

  /* ─── Card ───────────────────────────────────────────── */
  .card {
    background: var(--card); border-radius: var(--radius);
    border: 1px solid var(--border);
    margin-bottom: 18px; box-shadow: var(--shadow-sm);
    overflow: hidden;
  }
  .card-head {
    padding: 16px 20px; border-bottom: 1px solid var(--border);
    display: flex; align-items: center; gap: 12px; flex-wrap: wrap;
    background: linear-gradient(180deg, #fff, #fcfaf6);
  }
  .card-body { padding: 20px; }
  h2 {
    margin: 0; font-size: 13px; font-weight: 600; color: var(--muted);
    text-transform: uppercase; letter-spacing: 1.2px;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
  }
  .card-section { padding: 18px 20px; border-bottom: 1px solid var(--border); }
  .card-section:last-child { border-bottom: 0; }
  .card-section h2 { margin-bottom: 14px; }

  /* ─── Breadcrumbs ────────────────────────────────────── */
  .crumbs {
    flex: 1; display: flex; flex-wrap: wrap; gap: 6px;
    align-items: center; font-size: 14px; min-width: 0;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
  }
  .crumbs a {
    color: var(--text); text-decoration: none; cursor: pointer;
    padding: 4px 8px; border-radius: 5px;
  }
  .crumbs a:hover { background: var(--hover); color: var(--brand); }
  .crumbs a.root { display: inline-flex; align-items: center; gap: 6px; font-weight: 600; }
  .crumbs .sep { color: var(--border); user-select: none; }
  .badge {
    margin-left: 8px; padding: 2px 8px; border-radius: 999px;
    background: var(--bg-deep); color: var(--muted);
    font-size: 11px; font-weight: 600; letter-spacing: 0.4px;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
  }

  /* ─── Buttons ────────────────────────────────────────── */
  button.btn, label.btn {
    background: var(--accent); color: #fff; border: 0;
    padding: 8px 14px; border-radius: 6px; cursor: pointer;
    font-size: 13px; font-weight: 500;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    display: inline-flex; align-items: center; gap: 6px;
    transition: background 0.15s, transform 0.05s;
  }
  button.btn:hover, label.btn:hover { background: var(--accent-soft); }
  button.btn:active { transform: translateY(1px); }
  button.btn.sm { padding: 5px 10px; font-size: 12px; }
  button.btn.brand { background: var(--brand); }
  button.btn.brand:hover { background: #92400e; }
  button.btn.danger { background: transparent; color: var(--danger); border: 1px solid transparent; }
  button.btn.danger:hover { background: #fef2f2; border-color: #fecaca; }
  button.btn.ghost { background: transparent; color: var(--text); border: 1px solid var(--border); }
  button.btn.ghost:hover { background: var(--hover); border-color: #d6d3d1; }

  /* ─── Rows ───────────────────────────────────────────── */
  .row {
    display: flex; align-items: center; gap: 14px;
    padding: 12px 20px; border-bottom: 1px solid var(--border);
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    font-size: 14px;
    transition: background 0.1s;
  }
  .row:last-child { border-bottom: 0; }
  .row:hover { background: var(--hover); }
  .row .icon { width: 20px; height: 20px; flex: 0 0 20px; color: var(--muted); display: grid; place-items: center; }
  .row.is-folder .icon { color: var(--brand); }
  .row .name { flex: 1; min-width: 0; word-break: break-all; color: var(--text); }
  .row .name a { color: inherit; text-decoration: none; cursor: pointer; }
  .row .name a:hover { color: var(--brand); }
  .row .size { color: var(--muted); font-size: 12px; min-width: 80px; text-align: right; font-variant-numeric: tabular-nums; }
  .row .actions { display: flex; gap: 4px; }
  .row.parent .icon { color: var(--muted); }
  .row.parent .name { color: var(--muted); cursor: pointer; }

  /* ─── Forms ──────────────────────────────────────────── */
  input[type="text"], input[type="password"], input[type="number"] {
    width: 100%; padding: 10px 12px; border: 1px solid var(--border); border-radius: 6px;
    font-size: 14px; background: #fff; color: var(--text);
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    transition: border-color 0.15s, box-shadow 0.15s;
  }
  input:focus { outline: 0; border-color: var(--brand); box-shadow: 0 0 0 3px rgba(180,83,9,0.15); }
  label {
    display: block; margin: 14px 0 6px; font-weight: 600; font-size: 12px;
    color: var(--muted); text-transform: uppercase; letter-spacing: 0.6px;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
  }
  .switch {
    display: flex; align-items: center; gap: 10px; padding: 10px 0;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; font-size: 14px;
  }
  .switch input { width: 18px; height: 18px; accent-color: var(--brand); }
  .grid2 { display: grid; grid-template-columns: 1fr 1fr; gap: 14px 18px; }
  @media (max-width: 600px) { .grid2, .grid3 { grid-template-columns: 1fr; } header { padding: 12px 18px; } main { padding: 16px 12px 60px; } }

  /* ─── Settings: section head, fields, grids ──────────── */
  .sec-head { display: flex; align-items: center; gap: 10px; margin: 0 0 16px; }
  .sec-head .sec-icon { width: 18px; height: 18px; color: var(--brand); flex: 0 0 18px; }
  .sec-head h2 { margin: 0; }
  .field { margin: 0; }
  .field + .field, .sec-body > .field + .field, .card-section > .grid2 + .field,
  .card-section > .field + .grid2, .card-section > .grid2 + .toggle,
  .card-section > .field + .toggle, .card-section > .toggle + .toggle,
  .card-section > .sec-head + .toggle, .card-section > .toggle + .sec-body { margin-top: 14px; }
  .field label { display: block; margin: 0 0 6px; font-weight: 600; font-size: 11px;
    color: var(--muted); text-transform: uppercase; letter-spacing: 0.7px;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; }
  .hint { margin: 6px 0 0; font-size: 12px; color: var(--muted); line-height: 1.45;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; }
  .grid3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px 16px; }

  /* ─── Select ─────────────────────────────────────────── */
  select {
    width: 100%; padding: 10px 36px 10px 12px;
    border: 1px solid var(--border); border-radius: 6px;
    font-size: 14px; color: var(--text); cursor: pointer;
    background: #fff url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='12' height='12' viewBox='0 0 24 24' fill='none' stroke='%2378716c' stroke-width='2.5' stroke-linecap='round'><path d='M6 9l6 6 6-6'/></svg>") right 12px center no-repeat;
    appearance: none; -webkit-appearance: none;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    transition: border-color 0.15s, box-shadow 0.15s;
  }
  select:focus { outline: 0; border-color: var(--brand); box-shadow: 0 0 0 3px rgba(180,83,9,0.15); }

  /* ─── Toggle switch ──────────────────────────────────── */
  .toggle { display: flex; align-items: center; gap: 12px; padding: 6px 0; cursor: pointer;
    font-size: 14px; user-select: none;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
  }
  .toggle input { position: absolute; opacity: 0; pointer-events: none; width: 0; height: 0; }
  .toggle .track { width: 38px; height: 22px; background: var(--bg-deep);
    border: 1px solid var(--border); border-radius: 999px; position: relative;
    transition: background 0.2s, border-color 0.2s; flex: 0 0 38px;
  }
  .toggle .thumb { position: absolute; top: 2px; left: 2px; width: 16px; height: 16px;
    background: #fff; border-radius: 50%; box-shadow: var(--shadow-sm);
    transition: transform 0.2s;
  }
  .toggle input:checked + .track { background: var(--brand); border-color: var(--brand); }
  .toggle input:checked + .track .thumb { transform: translateX(16px); }
  .toggle input:focus-visible + .track { box-shadow: 0 0 0 3px rgba(180,83,9,0.18); }
  .toggle-label { flex: 1; color: var(--text); }

  /* ─── Segmented control ──────────────────────────────── */
  .seg { display: flex; background: var(--bg-deep); border: 1px solid var(--border);
    border-radius: 8px; padding: 3px; gap: 2px; width: 100%;
  }
  .seg button { flex: 1; background: transparent; border: 0;
    padding: 9px 10px; border-radius: 6px; cursor: pointer;
    font-size: 13px; color: var(--muted); font-weight: 500;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
    transition: background 0.15s, color 0.15s, box-shadow 0.15s;
  }
  .seg button:hover { color: var(--text); }
  .seg button.active { background: var(--card); color: var(--text); font-weight: 600;
    box-shadow: var(--shadow-sm);
  }
  .seg button:focus-visible { outline: 0; box-shadow: 0 0 0 2px var(--brand); }

  /* ─── Range slider ───────────────────────────────────── */
  .range { display: flex; align-items: center; gap: 14px; padding: 6px 0; }
  .range input[type="range"] { flex: 1; -webkit-appearance: none; appearance: none;
    height: 4px; background: var(--bg-deep); border-radius: 2px; outline: none;
    border: 1px solid var(--border);
  }
  .range input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none; appearance: none; width: 20px; height: 20px;
    background: var(--brand); border-radius: 50%; cursor: pointer;
    box-shadow: var(--shadow-sm); border: 2px solid var(--card);
    transition: transform 0.1s;
  }
  .range input[type="range"]::-webkit-slider-thumb:hover { transform: scale(1.1); }
  .range input[type="range"]::-moz-range-thumb {
    width: 20px; height: 20px; background: var(--brand); border-radius: 50%;
    cursor: pointer; border: 2px solid var(--card);
  }
  .range input[type="range"]:focus-visible { box-shadow: 0 0 0 3px rgba(180,83,9,0.18); }
  .range-value { font-variant-numeric: tabular-nums; font-weight: 600; font-size: 13px;
    color: var(--text); min-width: 52px; text-align: right;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
  }

  /* ─── Password reveal ────────────────────────────────── */
  .input-wrap { position: relative; }
  .input-wrap input { padding-right: 40px; }
  .input-wrap .reveal { position: absolute; right: 6px; top: 50%;
    transform: translateY(-50%); background: transparent; border: 0;
    padding: 6px; cursor: pointer; color: var(--muted);
    display: grid; place-items: center; border-radius: 4px;
  }
  .input-wrap .reveal:hover { color: var(--text); background: var(--hover); }
  .input-wrap .reveal:focus-visible { outline: 0; box-shadow: 0 0 0 2px var(--brand); }

  /* ─── Disabled section body ──────────────────────────── */
  .sec-body { transition: opacity 0.2s; }
  .sec-body.is-disabled { opacity: 0.45; pointer-events: none; }

  /* ─── Sticky save bar ────────────────────────────────── */
  .save-bar {
    position: sticky; bottom: 12px; display: flex; gap: 10px;
    align-items: center; padding: 12px 18px; margin-top: 18px;
    background: var(--card); border: 1px solid var(--border);
    border-radius: var(--radius); box-shadow: var(--shadow-md);
    z-index: 10;
  }
  .save-bar .dirty { flex: 1; display: flex; align-items: center; gap: 8px;
    font-size: 12px; color: var(--muted);
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif;
  }
  .save-bar .dirty::before { content: ""; width: 8px; height: 8px;
    border-radius: 50%; background: var(--border);
    transition: background 0.2s;
  }
  .save-bar.is-dirty .dirty { color: var(--text); font-weight: 500; }
  .save-bar.is-dirty .dirty::before { background: var(--brand); animation: pulse 1.6s ease-in-out infinite; }
  .save-bar button.btn:disabled { opacity: 0.4; cursor: not-allowed; pointer-events: none; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.45; } }

  /* ─── States ─────────────────────────────────────────── */
  .empty {
    color: var(--muted); padding: 48px 24px; text-align: center;
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; font-size: 14px;
  }
  .empty .big { font-size: 26px; margin-bottom: 6px; opacity: 0.4; }
  .toast {
    position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%) translateY(8px);
    background: var(--accent); color: #fff; padding: 10px 18px; border-radius: 8px;
    opacity: 0; transition: opacity 0.2s, transform 0.2s; pointer-events: none; z-index: 100;
    box-shadow: var(--shadow-md);
    font-family: -apple-system, BlinkMacSystemFont, system-ui, sans-serif; font-size: 13px;
  }
  .toast.show { opacity: 1; transform: translateX(-50%) translateY(0); }
  .toast.error { background: var(--danger); }
  .progress {
    display: none; margin: 0 20px 14px; height: 4px;
    background: var(--bg-deep); border-radius: 2px; overflow: hidden;
  }
  .progress > div { height: 100%; background: var(--brand); width: 0; transition: width 0.1s; }
  .footer-actions { display: flex; gap: 10px; justify-content: flex-end; padding: 16px 20px; background: #fafaf7; border-top: 1px solid var(--border); }

  /* ─── Dark mode (matches reader's "warm e-ink at night" feel) ──── */
  @media (prefers-color-scheme: dark) {
    :root {
      --bg:           #1c1917;
      --bg-deep:      #0f0e0c;
      --card:         #292524;
      --text:         #f5f3ee;
      --muted:        #a8a29e;
      --accent:       #f5f3ee;
      --accent-soft:  #d6d3d1;
      --brand:        #f59e0b;
      --brand-soft:   #44403c;
      --danger:       #f87171;
      --border:       #44403c;
      --hover:        #1c1917;
      --shadow-sm:    0 1px 2px rgba(0,0,0,0.5);
      --shadow-md:    0 4px 16px rgba(0,0,0,0.55);
    }
    header { background: #0f0e0c; }
    .card-head { background: linear-gradient(180deg, var(--card), #1c1917); }
    nav { background: #0f0e0c; }
    .footer-actions { background: #1c1917; }
    input[type="text"], input[type="password"], input[type="number"] {
      background: #1c1917; color: var(--text);
    }
    select { background-color: #1c1917; color: var(--text); }
    .seg button.active { background: #1c1917; color: var(--text); }
    .toggle .thumb { background: #f5f3ee; }
    .save-bar { box-shadow: 0 6px 24px rgba(0,0,0,0.5); }
    button.btn { background: var(--brand); color: #1c1917; }
    button.btn:hover { background: #fbbf24; }
    button.btn.brand { background: var(--brand); color: #1c1917; }
    button.btn.brand:hover { background: #fbbf24; }
    button.btn.ghost { color: var(--text); }
    button.btn.danger { color: var(--danger); }
  }
</style>
</head>
<body>
<header>
  <div class="logo">P</div>
  <h1>Paperloom</h1>
  <span class="sub">E-Reader</span>
</header>
<nav role="tablist" aria-label="Sections">
  <button id="tabFiles" class="active" role="tab" aria-selected="true" aria-controls="filesView" onclick="showTab('files')">Files</button>
  <button id="tabSettings" role="tab" aria-selected="false" aria-controls="settingsView" onclick="showTab('settings')">Settings</button>
</nav>

<main>
  <section id="filesView" role="tabpanel" aria-labelledby="tabFiles" tabindex="0">
    <div class="card">
      <div class="card-head">
        <div class="crumbs" id="crumbs"></div>
        <span class="badge" id="itemCount">0 items</span>
        <div style="flex-basis:100%;height:0"></div>
        <button class="btn ghost" onclick="promptMkdir()">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>
          New Folder
        </button>
        <label class="btn brand" for="fileInput">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M17 8l-5-5-5 5M12 3v12"/></svg>
          Upload File
          <input type="file" id="fileInput" accept=".epub,image/jpeg,image/png" hidden onchange="doUpload()">
        </label>
      </div>
      <div class="progress" id="upProgress"><div></div></div>
      <div id="fileList"></div>
    </div>
  </section>

  <section id="settingsView" role="tabpanel" aria-labelledby="tabSettings" tabindex="0" hidden>
    <div class="card">
      <div class="card-section">
        <div class="sec-head">
          <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>
          <h2>WiFi</h2>
        </div>
        <div class="field">
          <label for="set_wifiSSID">Network name</label>
          <input type="text" id="set_wifiSSID" maxlength="32" autocomplete="off" spellcheck="false">
        </div>
        <div class="field">
          <label for="set_wifiPass">Password</label>
          <div class="input-wrap">
            <input type="password" id="set_wifiPass" maxlength="64" placeholder="••••••••" autocomplete="new-password">
            <button type="button" class="reveal" id="passReveal" aria-label="Show password" onclick="togglePass()">
              <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/><circle cx="12" cy="12" r="3"/></svg>
            </button>
          </div>
          <p class="hint">Leave blank to keep the current password.</p>
        </div>
      </div>

      <div class="card-section">
        <div class="sec-head">
          <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M2 3h6a4 4 0 0 1 4 4v14a3 3 0 0 0-3-3H2z"/><path d="M22 3h-6a4 4 0 0 0-4 4v14a3 3 0 0 1 3-3h7z"/></svg>
          <h2>Reading</h2>
        </div>
        <div class="field">
          <label>Font size</label>
          <div class="seg" role="group" aria-label="Font size" data-target="set_fontSizeLevel">
            <button type="button" data-v="0" aria-pressed="false">XS</button>
            <button type="button" data-v="1" aria-pressed="false">S</button>
            <button type="button" data-v="2" aria-pressed="false">M</button>
            <button type="button" data-v="3" aria-pressed="false">L</button>
            <button type="button" data-v="4" aria-pressed="false">XL</button>
          </div>
          <input type="hidden" id="set_fontSizeLevel">
        </div>
        <div class="field">
          <label>Line spacing</label>
          <div class="seg" role="group" aria-label="Line spacing" data-target="set_lineSpacingLevel">
            <button type="button" data-v="0" aria-pressed="false">Tight</button>
            <button type="button" data-v="1" aria-pressed="false">Snug</button>
            <button type="button" data-v="2" aria-pressed="false">Normal</button>
            <button type="button" data-v="3" aria-pressed="false">Roomy</button>
            <button type="button" data-v="4" aria-pressed="false">Loose</button>
          </div>
          <input type="hidden" id="set_lineSpacingLevel">
        </div>
        <div class="field">
          <label for="set_fontFamily">Font family</label>
          <select id="set_fontFamily">
            <option value="0">Sans · Lexend Deca</option>
            <option value="1">Serif · Literata</option>
            <option value="2">Slab · Bitter</option>
            <option value="3">Inter</option>
          </select>
        </div>
        <div class="grid2">
          <div class="field">
            <label for="set_sleepTimeoutMin">Sleep after</label>
            <input type="number" id="set_sleepTimeoutMin" min="1" max="240">
            <p class="hint">Minutes of inactivity before sleep.</p>
          </div>
          <div class="field">
            <label for="set_refreshEveryPages">Full refresh every</label>
            <input type="number" id="set_refreshEveryPages" min="1" max="20">
            <p class="hint">Pages between full e-ink refreshes.</p>
          </div>
        </div>
        <label class="toggle">
          <input type="checkbox" id="set_showPageNumbers">
          <span class="track"><span class="thumb"></span></span>
          <span class="toggle-label">Show page numbers</span>
        </label>
        <label class="toggle">
          <input type="checkbox" id="set_showBattery">
          <span class="track"><span class="thumb"></span></span>
          <span class="toggle-label">Show battery indicator</span>
        </label>
      </div>

      <div class="card-section">
        <div class="sec-head">
          <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="5"/><line x1="12" y1="1" x2="12" y2="3"/><line x1="12" y1="21" x2="12" y2="23"/><line x1="4.22" y1="4.22" x2="5.64" y2="5.64"/><line x1="18.36" y1="18.36" x2="19.78" y2="19.78"/><line x1="1" y1="12" x2="3" y2="12"/><line x1="21" y1="12" x2="23" y2="12"/><line x1="4.22" y1="19.78" x2="5.64" y2="18.36"/><line x1="18.36" y1="5.64" x2="19.78" y2="4.22"/></svg>
          <h2>Frontlight</h2>
        </div>
        <label class="toggle">
          <input type="checkbox" id="set_frontlightEnabled">
          <span class="track"><span class="thumb"></span></span>
          <span class="toggle-label">Frontlight enabled</span>
        </label>
        <div class="sec-body" id="frontlightBody">
          <div class="field">
            <label for="set_frontlightBrightness">Brightness</label>
            <div class="range">
              <input type="range" id="set_frontlightBrightness" min="0" max="100" step="1" aria-valuemin="0" aria-valuemax="100">
              <span class="range-value" id="val_brightness" aria-live="polite">0%</span>
            </div>
          </div>
        </div>
      </div>

      <div class="card-section">
        <div class="sec-head">
          <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3"/></svg>
          <h2>IO48 button</h2>
        </div>
        <label class="toggle">
          <input type="checkbox" id="set_userButtonEnabled">
          <span class="track"><span class="thumb"></span></span>
          <span class="toggle-label">Enable button</span>
        </label>
        <div class="sec-body" id="userBtnBody">
          <div class="grid3">
            <div class="field">
              <label for="set_userButtonTapAction">Tap</label>
              <select id="set_userButtonTapAction" class="btnAction"></select>
            </div>
            <div class="field">
              <label for="set_userButtonDoubleAction">Double tap</label>
              <select id="set_userButtonDoubleAction" class="btnAction"></select>
            </div>
            <div class="field">
              <label for="set_userButtonLongAction">Hold</label>
              <select id="set_userButtonLongAction" class="btnAction"></select>
            </div>
          </div>
        </div>
      </div>

      <div class="card-section">
        <div class="sec-head">
          <svg class="sec-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="3" width="18" height="18" rx="3"/><circle cx="12" cy="12" r="3"/></svg>
          <h2>Boot button</h2>
        </div>
        <label class="toggle">
          <input type="checkbox" id="set_bootButtonEnabled">
          <span class="track"><span class="thumb"></span></span>
          <span class="toggle-label">Enable button</span>
        </label>
        <div class="sec-body" id="bootBtnBody">
          <div class="grid3">
            <div class="field">
              <label for="set_bootButtonTapAction">Tap</label>
              <select id="set_bootButtonTapAction" class="btnAction"></select>
            </div>
            <div class="field">
              <label for="set_bootButtonDoubleAction">Double tap</label>
              <select id="set_bootButtonDoubleAction" class="btnAction"></select>
            </div>
            <div class="field">
              <label for="set_bootButtonLongAction">Hold</label>
              <select id="set_bootButtonLongAction" class="btnAction"></select>
            </div>
          </div>
        </div>
      </div>
    </div>

    <div class="save-bar" id="saveBar">
      <span class="dirty" id="dirtyLabel">All changes saved</span>
      <button class="btn ghost" id="discardBtn" onclick="loadSettings()" disabled>Discard</button>
      <button class="btn brand" id="saveBtn" onclick="saveSettings()" disabled>Save changes</button>
    </div>
  </section>
</main>

<div id="toast" class="toast"></div>

<script>
let currentPath = '/';

function showTab(name) {
  const filesActive = name === 'files';
  const settingsActive = name === 'settings';
  const tFiles = document.getElementById('tabFiles');
  const tSettings = document.getElementById('tabSettings');
  tFiles.classList.toggle('active', filesActive);
  tSettings.classList.toggle('active', settingsActive);
  tFiles.setAttribute('aria-selected', String(filesActive));
  tSettings.setAttribute('aria-selected', String(settingsActive));
  document.getElementById('filesView').hidden = !filesActive;
  document.getElementById('settingsView').hidden = !settingsActive;
  if (settingsActive) loadSettings();
  if (filesActive) refreshList();
}

function toast(msg, isErr) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show' + (isErr ? ' error' : '');
  setTimeout(() => t.className = 'toast' + (isErr ? ' error' : ''), 2200);
}

function fmtSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
}

function joinPath(base, name) {
  if (base === '/') return '/' + name;
  return base + '/' + name;
}

function parentPath(p) {
  if (p === '/' || p === '') return '/';
  const idx = p.lastIndexOf('/');
  return idx <= 0 ? '/' : p.substring(0, idx);
}

// SVG icon snippets used inline in row markup.
const ICON_FOLDER  = '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg>';
const ICON_FILE    = '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z"/><path d="M14 3v6h6"/></svg>';
const ICON_PARENT  = '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M9 14L4 9l5-5"/><path d="M4 9h11a5 5 0 0 1 5 5v6"/></svg>';
const ICON_HOME    = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 11l9-8 9 8M5 10v10a1 1 0 0 0 1 1h4v-6h4v6h4a1 1 0 0 0 1-1V10"/></svg>';
const ICON_DL      = '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M7 10l5 5 5-5M12 15V3"/></svg>';
const ICON_TRASH   = '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2m3 0v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/></svg>';

function renderCrumbs() {
  const el = document.getElementById('crumbs');
  el.innerHTML = '';
  const parts = currentPath.split('/').filter(p => p.length);
  const root = document.createElement('a');
  root.className = 'root';
  root.innerHTML = ICON_HOME + ' SD';
  root.onclick = () => { currentPath = '/'; refreshList(); };
  el.appendChild(root);
  let acc = '';
  for (const p of parts) {
    acc += '/' + p;
    const sep = document.createElement('span');
    sep.className = 'sep';
    sep.innerHTML = '<svg width="10" height="10" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><path d="M9 6l6 6-6 6"/></svg>';
    el.appendChild(sep);
    const a = document.createElement('a');
    a.textContent = p;
    const target = acc;
    a.onclick = () => { currentPath = target; refreshList(); };
    el.appendChild(a);
  }
}

async function refreshList() {
  renderCrumbs();
  const list = document.getElementById('fileList');
  const counter = document.getElementById('itemCount');
  list.innerHTML = '<div class="empty">Loading…</div>';
  counter.textContent = '…';
  try {
    const r = await fetch('/api/list?path=' + encodeURIComponent(currentPath));
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || 'list failed');
    list.innerHTML = '';
    if (currentPath !== '/') {
      const row = document.createElement('div');
      row.className = 'row parent';
      row.innerHTML = '<div class="icon">' + ICON_PARENT + '</div><div class="name">Parent folder</div>';
      row.onclick = () => { currentPath = parentPath(currentPath); refreshList(); };
      row.style.cursor = 'pointer';
      list.appendChild(row);
    }
    counter.textContent = d.items.length + (d.items.length === 1 ? ' item' : ' items');
    if (d.items.length === 0) {
      list.appendChild(emptyState(currentPath === '/'
          ? 'SD card is empty. Upload a file or create a folder to get started.'
          : 'This folder is empty.'));
      return;
    }
    d.items.sort((a, b) => (b.isDir - a.isDir) || a.name.localeCompare(b.name));
    for (const it of d.items) list.appendChild(makeRow(it));
  } catch (e) {
    list.innerHTML = '';
    counter.textContent = '!';
    list.appendChild(emptyState('Error: ' + e.message));
  }
}

function emptyState(msg) {
  const r = document.createElement('div');
  r.className = 'empty';
  r.innerHTML = '<div class="big">·</div>' + '';
  const p = document.createElement('div');
  p.textContent = msg;
  r.appendChild(p);
  return r;
}

function makeRow(it) {
  const row = document.createElement('div');
  row.className = 'row' + (it.isDir ? ' is-folder' : '');
  const icon = document.createElement('div');
  icon.className = 'icon';
  icon.innerHTML = it.isDir ? ICON_FOLDER : ICON_FILE;
  const name = document.createElement('div');
  name.className = 'name';
  if (it.isDir) {
    const a = document.createElement('a');
    a.textContent = it.name;
    a.onclick = () => { currentPath = joinPath(currentPath, it.name); refreshList(); };
    name.appendChild(a);
  } else {
    name.textContent = it.name;
  }
  const size = document.createElement('div');
  size.className = 'size';
  size.textContent = it.isDir ? '' : fmtSize(it.size);
  const actions = document.createElement('div');
  actions.className = 'actions';
  if (!it.isDir) {
    const dl = document.createElement('button');
    dl.className = 'btn sm ghost';
    dl.title = 'Download';
    dl.innerHTML = ICON_DL;
    dl.onclick = () => { window.location = '/api/download?path=' + encodeURIComponent(joinPath(currentPath, it.name)); };
    actions.appendChild(dl);
  }
  const del = document.createElement('button');
  del.className = 'btn sm danger';
  del.title = 'Delete';
  del.innerHTML = ICON_TRASH;
  del.onclick = () => deleteEntry(it);
  actions.appendChild(del);
  row.appendChild(icon);
  row.appendChild(name);
  row.appendChild(size);
  row.appendChild(actions);
  return row;
}

async function deleteEntry(it) {
  const target = joinPath(currentPath, it.name);
  if (!confirm('Delete ' + (it.isDir ? 'folder' : 'file') + ' "' + it.name + '"?')) return;
  try {
    const r = await fetch('/api/delete', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({path: target})
    });
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || 'delete failed');
    toast('Deleted');
    refreshList();
  } catch (e) { toast(e.message, true); }
}

async function promptMkdir() {
  const name = prompt('Folder name:');
  if (!name) return;
  if (name.indexOf('/') >= 0 || name.indexOf('..') >= 0) { toast('Invalid name', true); return; }
  try {
    const r = await fetch('/api/mkdir', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({path: currentPath, name: name})
    });
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || 'mkdir failed');
    toast('Folder created');
    refreshList();
  } catch (e) { toast(e.message, true); }
}

function doUpload() {
  const inp = document.getElementById('fileInput');
  if (!inp.files.length) return;
  const f = inp.files[0];
  const fd = new FormData();
  fd.append('file', f);
  const xhr = new XMLHttpRequest();
  const prog = document.getElementById('upProgress');
  prog.style.display = 'block';
  prog.firstElementChild.style.width = '0%';
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = (e.loaded / e.total * 100).toFixed(1);
      prog.firstElementChild.style.width = pct + '%';
    }
  };
  xhr.onload = () => {
    prog.style.display = 'none';
    inp.value = '';
    if (xhr.status === 200) { toast('Uploaded'); refreshList(); }
    else {
      let msg = 'Upload failed (' + xhr.status + ')';
      try { const d = JSON.parse(xhr.responseText); if (d.error) msg = d.error; } catch (_) {}
      toast(msg, true);
    }
  };
  xhr.onerror = () => { prog.style.display = 'none'; toast('Upload error', true); };
  xhr.open('POST', '/api/upload?path=' + encodeURIComponent(currentPath));
  xhr.send(fd);
}

const BUTTON_ACTIONS = [
  {v: 0, label: 'None'},
  {v: 1, label: 'Light toggle'},
  {v: 2, label: 'Library'},
  {v: 3, label: 'Sleep'},
  {v: 4, label: 'Next page'},
  {v: 5, label: 'Prev page'},
  {v: 6, label: 'Menu'}
];

function populateButtonActionSelects() {
  document.querySelectorAll('select.btnAction').forEach(sel => {
    if (sel.options.length > 0) return;
    BUTTON_ACTIONS.forEach(a => {
      const o = document.createElement('option');
      o.value = String(a.v);
      o.textContent = a.label;
      sel.appendChild(o);
    });
  });
}

function setSeg(target, value) {
  const v = String(value);
  const hidden = document.getElementById(target);
  if (hidden) hidden.value = v;
  document.querySelectorAll('.seg[data-target="' + target + '"] button').forEach(b => {
    const active = b.dataset.v === v;
    b.classList.toggle('active', active);
    b.setAttribute('aria-pressed', active ? 'true' : 'false');
  });
}

function bindSegmented() {
  document.querySelectorAll('.seg').forEach(seg => {
    const target = seg.dataset.target;
    seg.addEventListener('click', e => {
      const btn = e.target.closest('button[data-v]');
      if (!btn) return;
      setSeg(target, btn.dataset.v);
      markDirty();
    });
  });
}

function bindBrightness() {
  const r = document.getElementById('set_frontlightBrightness');
  const lbl = document.getElementById('val_brightness');
  r.addEventListener('input', () => { lbl.textContent = r.value + '%'; });
}

function syncBodyMirror(toggleId, bodyId) {
  const tgl = document.getElementById(toggleId);
  const body = document.getElementById(bodyId);
  const apply = () => body.classList.toggle('is-disabled', !tgl.checked);
  tgl.addEventListener('change', apply);
  apply();
}

function togglePass() {
  const inp = document.getElementById('set_wifiPass');
  const btn = document.getElementById('passReveal');
  const showing = inp.type === 'text';
  inp.type = showing ? 'password' : 'text';
  btn.setAttribute('aria-label', showing ? 'Show password' : 'Hide password');
}

let isDirty = false;
let suppressDirty = false;

function markDirty() {
  if (suppressDirty || isDirty) return;
  isDirty = true;
  document.getElementById('saveBar').classList.add('is-dirty');
  document.getElementById('dirtyLabel').textContent = 'Unsaved changes';
  document.getElementById('saveBtn').disabled = false;
  document.getElementById('discardBtn').disabled = false;
}

function markClean() {
  isDirty = false;
  document.getElementById('saveBar').classList.remove('is-dirty');
  document.getElementById('dirtyLabel').textContent = 'All changes saved';
  document.getElementById('saveBtn').disabled = true;
  document.getElementById('discardBtn').disabled = true;
}

function bindDirtyTracking() {
  document.querySelectorAll('#settingsView input, #settingsView select').forEach(el => {
    const useInput = el.type === 'text' || el.type === 'password' || el.type === 'number' || el.type === 'range';
    el.addEventListener(useInput ? 'input' : 'change', markDirty);
  });
}

let settingsBound = false;

async function loadSettings() {
  populateButtonActionSelects();
  if (!settingsBound) {
    bindSegmented();
    bindBrightness();
    syncBodyMirror('set_frontlightEnabled', 'frontlightBody');
    syncBodyMirror('set_userButtonEnabled', 'userBtnBody');
    syncBodyMirror('set_bootButtonEnabled', 'bootBtnBody');
    bindDirtyTracking();
    settingsBound = true;
  }
  suppressDirty = true;
  try {
    const r = await fetch('/api/settings');
    const d = await r.json();
    if (!d.ok) throw new Error('load failed');
    const s = d.settings;
    document.getElementById('set_wifiSSID').value = s.wifiSSID || '';
    document.getElementById('set_wifiPass').value = '';
    setSeg('set_fontSizeLevel', s.fontSizeLevel ?? 2);
    setSeg('set_lineSpacingLevel', s.lineSpacingLevel ?? 2);
    document.getElementById('set_sleepTimeoutMin').value = s.sleepTimeoutMin;
    document.getElementById('set_refreshEveryPages').value = s.refreshEveryPages;
    document.getElementById('set_fontFamily').value = String(s.fontFamily ?? 0);
    document.getElementById('set_showPageNumbers').checked = !!s.showPageNumbers;
    document.getElementById('set_showBattery').checked = !!s.showBattery;
    document.getElementById('set_frontlightEnabled').checked = !!s.frontlightEnabled;
    document.getElementById('set_frontlightBrightness').value = s.frontlightBrightness ?? 0;
    document.getElementById('val_brightness').textContent = (s.frontlightBrightness ?? 0) + '%';
    document.getElementById('set_userButtonEnabled').checked = !!s.userButtonEnabled;
    document.getElementById('set_userButtonTapAction').value = String(s.userButtonTapAction ?? 0);
    document.getElementById('set_userButtonDoubleAction').value = String(s.userButtonDoubleAction ?? 0);
    document.getElementById('set_userButtonLongAction').value = String(s.userButtonLongAction ?? 0);
    document.getElementById('set_bootButtonEnabled').checked = !!s.bootButtonEnabled;
    document.getElementById('set_bootButtonTapAction').value = String(s.bootButtonTapAction ?? 0);
    document.getElementById('set_bootButtonDoubleAction').value = String(s.bootButtonDoubleAction ?? 0);
    document.getElementById('set_bootButtonLongAction').value = String(s.bootButtonLongAction ?? 0);
    ['set_frontlightEnabled','set_userButtonEnabled','set_bootButtonEnabled'].forEach(id => {
      document.getElementById(id).dispatchEvent(new Event('change'));
    });
    markClean();
  } catch (e) { toast(e.message, true); }
  suppressDirty = false;
}

async function saveSettings() {
  const payload = {
    wifiSSID: document.getElementById('set_wifiSSID').value,
    wifiPass: document.getElementById('set_wifiPass').value,
    fontSizeLevel: +document.getElementById('set_fontSizeLevel').value,
    lineSpacingLevel: +document.getElementById('set_lineSpacingLevel').value,
    sleepTimeoutMin: +document.getElementById('set_sleepTimeoutMin').value,
    refreshEveryPages: +document.getElementById('set_refreshEveryPages').value,
    fontFamily: +document.getElementById('set_fontFamily').value,
    showPageNumbers: document.getElementById('set_showPageNumbers').checked,
    showBattery: document.getElementById('set_showBattery').checked,
    frontlightEnabled: document.getElementById('set_frontlightEnabled').checked,
    frontlightBrightness: +document.getElementById('set_frontlightBrightness').value,
    userButtonEnabled: document.getElementById('set_userButtonEnabled').checked,
    userButtonTapAction: +document.getElementById('set_userButtonTapAction').value,
    userButtonDoubleAction: +document.getElementById('set_userButtonDoubleAction').value,
    userButtonLongAction: +document.getElementById('set_userButtonLongAction').value,
    bootButtonEnabled: document.getElementById('set_bootButtonEnabled').checked,
    bootButtonTapAction: +document.getElementById('set_bootButtonTapAction').value,
    bootButtonDoubleAction: +document.getElementById('set_bootButtonDoubleAction').value,
    bootButtonLongAction: +document.getElementById('set_bootButtonLongAction').value
  };
  const saveBtn = document.getElementById('saveBtn');
  const origLabel = saveBtn.textContent;
  saveBtn.disabled = true;
  saveBtn.textContent = 'Saving…';
  try {
    const r = await fetch('/api/settings', {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify(payload)
    });
    const d = await r.json();
    if (!d.ok) throw new Error(d.error || 'save failed');
    document.getElementById('set_wifiPass').value = '';
    toast('Settings saved');
    markClean();
  } catch (e) {
    toast(e.message, true);
    saveBtn.disabled = false;
  } finally {
    saveBtn.textContent = origLabel;
  }
}

refreshList();
</script>
</body>
</html>
)rawliteral";

// ─── Helpers ───────────────────────────────────────────────────────

// Reject obvious traversal attempts. SD paths must be absolute, contain no
// ".." segments, no dot-prefixed segments (which the firmware uses for
// internal state — settings.json, .progress, .linecache), and stay free of
// NUL bytes / backslashes / CR-LF. This blocks filesystem traversal,
// HTTP-header smuggling, AND read/write/delete of internal state files
// (which include the plaintext WiFi password).
static bool segmentStartsWithDot(const String& p) {
    // Walk slash-delimited segments and reject any that begin with '.'.
    int start = 0;
    while (start < (int)p.length()) {
        int slash = p.indexOf('/', start);
        int end = (slash < 0) ? (int)p.length() : slash;
        if (end > start && p.charAt(start) == '.') return true;
        if (slash < 0) break;
        start = slash + 1;
    }
    return false;
}

static bool isSafePath(const String& p) {
    if (p.length() == 0 || p[0] != '/') return false;
    if (p.indexOf("..") >= 0) return false;
    if (p.indexOf('\\') >= 0) return false;
    if (p.indexOf('\r') >= 0 || p.indexOf('\n') >= 0) return false;
    for (size_t i = 0; i < p.length(); i++) {
        if (p[i] == '\0') return false;
    }
    if (segmentStartsWithDot(p)) return false;
    return true;
}

static bool isSafeName(const String& n) {
    if (n.length() == 0) return false;
    if (n.startsWith(" ") || n.endsWith(" ")) return false;
    if (n.startsWith(".")) return false;     // block creating new dotfiles
    if (n.indexOf('/') >= 0 || n.indexOf('\\') >= 0) return false;
    if (n.indexOf("..") >= 0) return false;
    if (n.indexOf('"') >= 0) return false;
    if (n.indexOf('\r') >= 0 || n.indexOf('\n') >= 0) return false;
    for (size_t i = 0; i < n.length(); i++) {
        if (n[i] == '\0') return false;
    }
    return true;
}

// Restrict to a printable ASCII subset for header use. Any character outside
// this allow-list is replaced with '_'. Result is always non-empty.
static String sanitizeHeaderFilename(const String& name) {
    String out;
    out.reserve(name.length());
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (isalnum((uint8_t)c) || c == '.' || c == '_' || c == '-' || c == ' ') {
            out += c;
        } else {
            out += '_';
        }
    }
    if (out.length() == 0) out = "download";
    return out;
}

static String basename(const String& path) {
    int slash = path.lastIndexOf('/');
    if (slash < 0) return path;
    return path.substring(slash + 1);
}

static String joinPath(const String& base, const String& name) {
    if (base.endsWith("/")) return base + name;
    return base + "/" + name;
}

// Escape minimal JSON string contents (quote, backslash, control chars).
static String jsonEscape(const String& s) {
    String out;
    out.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static void sendJsonError(int code, const char* msg) {
    String body = String("{\"ok\":false,\"error\":\"") + jsonEscape(msg) + "\"}";
    _server.send(code, "application/json", body);
}

// ─── HTTP handlers ─────────────────────────────────────────────────

static void handleRoot() {
    // send_P streams directly from PROGMEM without copying the ~12 KB HTML
    // body through an Arduino String allocation on the heap.
    _server.send_P(200, PSTR("text/html"), UPLOAD_HTML);
}

// Cap directory listings to keep the JSON response within heap budget.
static const int MAX_LIST_ITEMS = 500;

static void handleApiList() {
    String path = _server.arg("path");
    if (path.length() == 0) path = "/";
    if (!isSafePath(path)) { sendJsonError(400, "Invalid path"); return; }

    File dir = SD.open(path);
    if (!dir) { sendJsonError(404, "Not found"); return; }
    if (!dir.isDirectory()) { dir.close(); sendJsonError(400, "Not a directory"); return; }

    String json;
    // Pre-reserve to avoid up to ~9 reallocations during the 500-item loop.
    json.reserve(MAX_LIST_ITEMS * 80 + 128);
    json = "{\"ok\":true,\"path\":\"" + jsonEscape(path) + "\",\"items\":[";
    bool first = true;
    int count = 0;
    File entry;
    bool truncated = false;
    while ((entry = dir.openNextFile())) {
        if (count >= MAX_LIST_ITEMS) { entry.close(); truncated = true; break; }
        String name = entry.name();
        // entry.name() may be a full path on some SD lib versions — keep
        // only the basename so the UI can append it onto currentPath.
        name = basename(name);
        // Hide our internal dotfiles/dotfolders
        if (name.length() == 0 || name.startsWith(".")) {
            entry.close();
            continue;
        }
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + jsonEscape(name) + "\"";
        json += ",\"isDir\":" + String(entry.isDirectory() ? "true" : "false");
        if (!entry.isDirectory()) json += ",\"size\":" + String((uint32_t)entry.size());
        json += "}";
        entry.close();
        count++;
    }
    dir.close();
    json += "]";
    if (truncated) json += ",\"truncated\":true";
    json += "}";
    _server.send(200, "application/json", json);
}

static void handleApiMkdir() {
    String body = _server.arg("plain");
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) { sendJsonError(400, "Invalid JSON"); return; }

    String parent = doc["path"].as<String>();
    String name   = doc["name"].as<String>();
    if (!isSafePath(parent) || !isSafeName(name)) {
        sendJsonError(400, "Invalid path or name");
        return;
    }
    String full = joinPath(parent, name);
    if (SD.exists(full.c_str())) { sendJsonError(409, "Already exists"); return; }
    if (!SD.mkdir(full.c_str())) { sendJsonError(500, "mkdir failed"); return; }
    Serial.printf("WebUI mkdir: %s\n", full.c_str());
    _server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiDelete() {
    String body = _server.arg("plain");
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) { sendJsonError(400, "Invalid JSON"); return; }

    String path = doc["path"].as<String>();
    if (!isSafePath(path)) { sendJsonError(400, "Invalid path"); return; }
    if (path == "/") { sendJsonError(400, "Cannot delete root"); return; }

    File f = SD.open(path);
    if (!f) { sendJsonError(404, "Not found"); return; }
    bool isDir = f.isDirectory();
    f.close();

    bool ok = isDir ? SD.rmdir(path.c_str()) : SD.remove(path.c_str());
    if (!ok) {
        sendJsonError(500, isDir ? "rmdir failed (folder not empty?)" : "remove failed");
        return;
    }
    Serial.printf("WebUI delete: %s\n", path.c_str());
    _server.send(200, "application/json", "{\"ok\":true}");
}

static void handleApiDownload() {
    String path = _server.arg("path");
    if (!isSafePath(path)) { sendJsonError(400, "Invalid path"); return; }
    File f = SD.open(path, FILE_READ);
    if (!f) { sendJsonError(404, "Not found"); return; }
    if (f.isDirectory()) { f.close(); sendJsonError(400, "Is a directory"); return; }

    // Strict ASCII subset for the filename header — protects against CRLF
    // header injection from filenames containing crafted bytes.
    String name = sanitizeHeaderFilename(basename(path));
    _server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    _server.sendHeader("X-Content-Type-Options", "nosniff");
    _server.streamFile(f, "application/octet-stream");
    f.close();
}

// Multipart upload (POST /api/upload?path=/dir)
//
// ESP32 WebServer routes the upload buffers through a separate handler that
// runs BEFORE the main route handler. handleApiUploadData() reads the target
// dir from the query string at FILE_START and stitches the full path then.
//
// State carried between FILE_START / WRITE / END / ABORTED:
//   _uploadFile     — open SD handle (or invalid)
//   _uploadFullPath — destination path so we can SD.remove() on abort
//   _uploadFailed   — sticky flag read by handleApiUploadComplete to send
//                     a meaningful HTTP error instead of falsely reporting
//                     "ok" when validation/IO failed during streaming
//   _uploadError    — human-readable failure reason for the JSON response

// Hard cap on multipart filename header length; anything beyond this is
// almost certainly an attacker probing for heap exhaustion.
static const size_t MAX_UPLOAD_FILENAME = 255;
// Hard cap on per-upload size (defensive — the SD card itself caps total
// storage but a single hung connection should not be allowed to fill it).
static const uint32_t MAX_UPLOAD_BYTES = 200UL * 1024UL * 1024UL;  // 200 MB

static String   _uploadFullPath;
static bool     _uploadFailed   = false;
static String   _uploadError;
// Independent running counter — `upload.totalSize` from the ESP32 WebServer
// derives from the Content-Length header. A chunked POST without that header
// keeps `totalSize` at 0 forever, bypassing the cap. We track ourselves.
static uint32_t _uploadBytesWritten = 0;

static void closeUploadHandle() {
    if (_uploadFile) _uploadFile.close();
    _uploadFile = File();   // explicit invalid state, portable across SD libs
}

// Mark the in-flight upload as failed and discard any partial bytes on disk.
// Idempotent — safe to call from multiple branches.
static void abortUpload(const char* reason) {
    Serial.printf("Upload abort: %s\n", reason);
    closeUploadHandle();
    // We now write to <path>.tmp and only rename on FILE_END, so the
    // partial data lives in the .tmp file — clean that up rather than the
    // final path (which may not yet exist).
    if (_uploadFullPath.length() > 0) {
        String tmpPath = _uploadFullPath + ".tmp";
        if (SD.exists(tmpPath.c_str())) {
            SD.remove(tmpPath.c_str());
            Serial.printf("Removed partial upload: %s\n", tmpPath.c_str());
        }
    }
    _uploadFullPath = "";
    _uploadFailed   = true;
    if (_uploadError.length() == 0) _uploadError = reason;
}

static void resetUploadState() {
    closeUploadHandle();
    _uploadFullPath     = "";
    _uploadFailed       = false;
    _uploadError        = "";
    _uploadBytesWritten = 0;
}

static void handleApiUploadComplete() {
    if (_uploadFailed) {
        String msg = _uploadError.length() > 0 ? _uploadError : String("Upload failed");
        sendJsonError(400, msg.c_str());
    } else {
        _server.send(200, "application/json", "{\"ok\":true}");
    }
    // Always clean module state after the response is sent so the next
    // upload begins on a known-clean slate.
    resetUploadState();
}

static void handleApiUploadData() {
    HTTPUpload& upload = _server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        // Fresh upload — clear any leftover state from the previous request.
        resetUploadState();

        if (upload.filename.length() > MAX_UPLOAD_FILENAME) {
            _uploadFailed = true;
            _uploadError  = "Filename too long";
            Serial.println("Upload rejected: filename too long");
            return;
        }
        // Pull target path freshly from the query each upload — _server.arg()
        // is valid throughout the upload lifecycle. Reject (don't silently
        // fall back to root) so unexpected upload destinations are visible
        // to the client.
        String dir = _server.arg("path");
        if (!isSafePath(dir)) {
            _uploadFailed = true;
            _uploadError  = "Invalid upload path";
            Serial.println("Upload rejected: invalid ?path=");
            return;
        }

        String filename = basename(upload.filename);
        if (!isSafeName(filename)) {
            _uploadFailed = true;
            _uploadError  = "Unsafe filename";
            Serial.printf("Upload rejected: unsafe filename %s\n", filename.c_str());
            return;
        }
        _uploadFullPath = joinPath(dir, filename);
        // Write to a sibling .tmp first so a power loss / disconnect mid-
        // upload doesn't leave a truncated file with the real name. On
        // UPLOAD_FILE_END we rename .tmp → final atomically.
        String tmpPath = _uploadFullPath + ".tmp";
        Serial.printf("WebUI upload start: %s (tmp=%s)\n",
                      _uploadFullPath.c_str(), tmpPath.c_str());
        // Clear any leftover from a prior aborted upload of the same name.
        if (SD.exists(tmpPath.c_str())) SD.remove(tmpPath.c_str());
        _uploadFile = SD.open(tmpPath.c_str(), FILE_WRITE);
        if (!_uploadFile) {
            _uploadFailed = true;
            _uploadError  = "SD open failed";
            Serial.println("Failed to open upload tmp target");
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (_uploadFailed) return;
        if (!_uploadFile) return;
        // Track ourselves — `upload.totalSize` is unreliable when the client
        // omits Content-Length (chunked upload).
        _uploadBytesWritten += upload.currentSize;
        if (_uploadBytesWritten > MAX_UPLOAD_BYTES) {
            _uploadError = "Exceeds size cap";
            abortUpload("size cap exceeded");
            return;
        }
        size_t got = _uploadFile.write(upload.buf, upload.currentSize);
        if (got != upload.currentSize) {
            // Short write means SD is full or the card threw an error.
            // Bail immediately so we don't silently truncate the file.
            _uploadError = "SD write failed (card full?)";
            abortUpload("short SD write");
            return;
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!_uploadFailed && _uploadFile) {
            Serial.printf("WebUI upload done: %u bytes\n", upload.totalSize);
        }
        closeUploadHandle();
        // Promote tmp → final only on a clean transfer.
        if (!_uploadFailed && _uploadFullPath.length() > 0) {
            String tmpPath = _uploadFullPath + ".tmp";
            // Remove any pre-existing destination so rename succeeds on FAT.
            if (SD.exists(_uploadFullPath.c_str())) {
                SD.remove(_uploadFullPath.c_str());
            }
            if (!SD.rename(tmpPath.c_str(), _uploadFullPath.c_str())) {
                Serial.printf("Upload: rename %s → %s failed — partial file removed\n",
                              tmpPath.c_str(), _uploadFullPath.c_str());
                SD.remove(tmpPath.c_str());
                _uploadError  = "Rename failed";
                _uploadFailed = true;
            }
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        // Client disconnected mid-upload — release the SD handle AND remove
        // the partial file so it doesn't linger on disk.
        _uploadError = "Client aborted";
        abortUpload("client disconnected");
    }
}

// ─── Settings (full) ───────────────────────────────────────────────

static void handleApiSettingsGet() {
    Settings& s = settings_get();
    StaticJsonDocument<1024> doc;
    doc["ok"] = true;
    JsonObject set = doc.createNestedObject("settings");
    set["wifiSSID"]            = s.wifiSSID;
    // wifiPass intentionally omitted — never expose the stored password.
    set["fontSizeLevel"]       = s.fontSizeLevel;
    set["lineSpacingLevel"]    = s.lineSpacingLevel;
    set["fontFamily"]          = s.fontFamily;
    set["sleepTimeoutMin"]     = s.sleepTimeoutMin;
    set["refreshEveryPages"]   = s.refreshEveryPages;
    set["showPageNumbers"]     = s.showPageNumbers;
    set["showBattery"]         = s.showBattery;
    set["frontlightEnabled"]   = s.frontlightEnabled;
    set["frontlightBrightness"]= s.frontlightBrightness;
    set["userButtonEnabled"]      = s.userButtonEnabled;
    set["userButtonTapAction"]    = s.userButtonTapAction;
    set["userButtonDoubleAction"] = s.userButtonDoubleAction;
    set["userButtonLongAction"]   = s.userButtonLongAction;
    set["bootButtonEnabled"]      = s.bootButtonEnabled;
    set["bootButtonTapAction"]    = s.bootButtonTapAction;
    set["bootButtonDoubleAction"] = s.bootButtonDoubleAction;
    set["bootButtonLongAction"]   = s.bootButtonLongAction;
    String body;
    serializeJson(doc, body);
    _server.send(200, "application/json", body);
}

static void handleApiSettingsPost() {
    String body = _server.arg("plain");
    // Cap before parsing — guards against multi-MB POST bodies that would
    // otherwise allocate fully into RAM before ArduinoJson rejects them.
    if (body.length() > 2048) {
        sendJsonError(413, "Body too large");
        return;
    }
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, body)) { sendJsonError(400, "Invalid JSON"); return; }

    Settings& s = settings_get();
    bool flightChanged = false;

    if (doc.containsKey("wifiSSID")) {
        String ssid = doc["wifiSSID"].as<String>();
        // 802.11 SSID is max 32 octets. Reject anything longer to bound the
        // heap String and avoid a corrupted save on the SD-resident JSON.
        if (ssid.length() <= 32) s.wifiSSID = ssid;
    }
    if (doc.containsKey("wifiPass")) {
        String pw = doc["wifiPass"].as<String>();
        // WPA2-PSK max length is 63 ASCII chars (or 64 hex). Reject anything
        // longer to bound the heap String and avoid DoS via huge payloads.
        if (pw.length() > 0 && pw.length() <= 64) s.wifiPass = pw;
    }
    if (doc.containsKey("fontSizeLevel")) {
        int v = doc["fontSizeLevel"].as<int>();
        // FONT_SIZE_LEVEL_COUNT = 5 (XS,S,M,M-L,L). Reject anything beyond
        // the actual array bounds — fontSizeLevel is used as a direct index
        // into FONT_LINE_SPACINGS / FONT_MARGIN_X_VALUES.  Match the
        // fontFamily path: a 400 response keeps the client UI from
        // believing a bad value was accepted.
        if (v >= 0 && v < FONT_SIZE_LEVEL_COUNT) {
            s.fontSizeLevel = v;
        } else {
            Serial.printf("WebUI: rejected fontSizeLevel=%d (valid 0..%d)\n",
                          v, FONT_SIZE_LEVEL_COUNT - 1);
            sendJsonError(400, "fontSizeLevel out of range");
            return;
        }
    }
    if (doc.containsKey("lineSpacingLevel")) {
        int v = doc["lineSpacingLevel"].as<int>();
        if (v >= 0 && v < LINE_SPACING_LEVEL_COUNT) {
            s.lineSpacingLevel = (uint8_t)v;
        } else {
            Serial.printf("WebUI: rejected lineSpacingLevel=%d (valid 0..%d)\n",
                          v, LINE_SPACING_LEVEL_COUNT - 1);
            sendJsonError(400, "lineSpacingLevel out of range");
            return;
        }
    }
    if (doc.containsKey("fontFamily")) {
        int v = doc["fontFamily"].as<int>();
        // Reject bad values at the API boundary instead of silently
        // dropping them — otherwise the client sees a 200/ok response
        // but the device keeps the previous family, with no UI feedback.
        if (v >= 0 && v < (int)FONT_FAMILY_COUNT) {
            s.fontFamily = (uint8_t)v;
        } else {
            Serial.printf("WebUI: rejected fontFamily=%d (valid 0..%d)\n",
                          v, (int)FONT_FAMILY_COUNT - 1);
            sendJsonError(400, "fontFamily out of range");
            return;
        }
    } else if (doc.containsKey("serifFont")) {
        // Backwards-compat: pre-v3 clients still POST a bool.  Map it the
        // same way settings_init() migrates the on-disk JSON.
        s.fontFamily = doc["serifFont"].as<bool>() ? 1 : 0;
    }
    if (doc.containsKey("sleepTimeoutMin")) {
        int v = doc["sleepTimeoutMin"].as<int>();
        if (v >= 1 && v <= 240) s.sleepTimeoutMin = v;
    }
    if (doc.containsKey("refreshEveryPages")) {
        int v = doc["refreshEveryPages"].as<int>();
        if (v >= 1 && v <= 50) s.refreshEveryPages = v;
    }
    if (doc.containsKey("showPageNumbers")) s.showPageNumbers = doc["showPageNumbers"].as<bool>();
    if (doc.containsKey("showBattery"))     s.showBattery     = doc["showBattery"].as<bool>();
    if (doc.containsKey("frontlightEnabled")) {
        s.frontlightEnabled = doc["frontlightEnabled"].as<bool>();
        flightChanged = true;
    }
    if (doc.containsKey("frontlightBrightness")) {
        int v = doc["frontlightBrightness"].as<int>();
        if (v < 0) v = 0; if (v > 100) v = 100;
        s.frontlightBrightness = (uint8_t)v;
        flightChanged = true;
    }

    // Buttons.  Action values must be < BTN_ACTION_COUNT — clamp to None on
    // anything out of range so a buggy client cannot brick a gesture.
    auto applyAction = [&](const char* key, uint8_t& dst) {
        if (!doc.containsKey(key)) return;
        int v = doc[key].as<int>();
        if (v < 0 || v >= (int)BTN_ACTION_COUNT) v = 0;
        dst = (uint8_t)v;
    };
    if (doc.containsKey("userButtonEnabled")) s.userButtonEnabled = doc["userButtonEnabled"].as<bool>();
    applyAction("userButtonTapAction",    s.userButtonTapAction);
    applyAction("userButtonDoubleAction", s.userButtonDoubleAction);
    applyAction("userButtonLongAction",   s.userButtonLongAction);
    if (doc.containsKey("bootButtonEnabled")) s.bootButtonEnabled = doc["bootButtonEnabled"].as<bool>();
    applyAction("bootButtonTapAction",    s.bootButtonTapAction);
    applyAction("bootButtonDoubleAction", s.bootButtonDoubleAction);
    applyAction("bootButtonLongAction",   s.bootButtonLongAction);

    if (!settings_save()) {
        sendJsonError(500, "SD write failed");
        return;
    }
    if (flightChanged) frontlight_apply_from_settings();
    Serial.println("WebUI: settings updated");
    _server.send(200, "application/json", "{\"ok\":true}");
}

// ─── Public API ────────────────────────────────────────────────────

void wifi_upload_init() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _connecting = false;
    _error = false;
    _errorMsg = "";
}

void wifi_upload_start() {
    if (_running || _connecting) return;

    const Settings& s = settings_get();
    if (s.wifiSSID.length() == 0) {
        _error = true;
        _errorMsg = "No WiFi configured";
        Serial.println("WiFi: No SSID configured");
        return;
    }

    Serial.printf("WiFi: Starting connection to %s\n", s.wifiSSID.c_str());

    _connecting = true;
    _error = false;
    _errorMsg = "";
    _connectStart = millis();

    WiFi.mode(WIFI_STA);
    WiFi.begin(s.wifiSSID.c_str(), s.wifiPass.c_str());
}

void wifi_upload_stop() {
    if (!_running && !_connecting) return;
    _server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    _running = false;
    _connecting = false;
    _error = false;
    _errorMsg = "";
}

void wifi_upload_handle() {
    if (_running) {
        _server.handleClient();
        return;
    }

    if (_connecting) {
        wl_status_t status = WiFi.status();

        if (status == WL_CONNECTED) {
            Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());

            _server.on("/", HTTP_GET, handleRoot);
            _server.on("/api/list",     HTTP_GET,  handleApiList);
            _server.on("/api/mkdir",    HTTP_POST, handleApiMkdir);
            _server.on("/api/delete",   HTTP_POST, handleApiDelete);
            _server.on("/api/download", HTTP_GET,  handleApiDownload);
            _server.on("/api/upload",   HTTP_POST, handleApiUploadComplete, handleApiUploadData);
            _server.on("/api/settings", HTTP_GET,  handleApiSettingsGet);
            _server.on("/api/settings", HTTP_POST, handleApiSettingsPost);
            _server.begin();

            _running = true;
            _connecting = false;
            _error = false;
            _errorMsg = "";
        } else if (millis() - _connectStart >= CONNECT_TIMEOUT_MS) {
            Serial.println("WiFi connection failed: timeout");
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);

            _error = true;
            _errorMsg = "Connection failed";
            _connecting = false;
        }
    }
}

bool wifi_upload_running()    { return _running; }
bool wifi_upload_connecting() { return _connecting; }
bool wifi_upload_has_error()  { return _error; }
const char* wifi_upload_get_error() { return _errorMsg.c_str(); }

String wifi_upload_ip() {
    if (_running && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "Not connected";
}
