# Partial-Update-Refactor — Plan

Ziel: Display-Rendering von "immer Vollbild MODE_GC16" auf **dirty-rect partial updates mit intent-basierter API** umstellen. Quelle: `docs/epidy_partial_updates.md` + Multi-Reviewer-Audit (architect, code-explorer, performance, silent-failure).

## Ausgangslage

| Aspekt | Ist |
|---|---|
| Waveform-Modus | nur `MODE_GC16` (hardcoded in `epd_be_draw_grayscale_image`) |
| Reader-Draw | `display_fill_screen(15)` + voller Header/Body/Footer-Redraw pro Page-Turn |
| Partial-Infra | `display_update_reader_body` existiert, wird aber nicht aktiv genutzt |
| Rotation | `rotatePortraitToLandscape` läuft full-screen auf jedem Partial |
| `_partialCount` | dead code im Reader-Pfad |
| VCOM | 2400 mV (high, gut für Partials) |
| Header/Footer | 66 / 50 px (config.h:101-104), Body ~828 px |
| Settings-Stripes | bekannt, entstanden mit GC16-Partial |
| `needsRedraw` | einzelner bool, keine Zone-Granularität |
| Temperatur | hardcoded 25 °C |
| Watchdog | kein `wdt_reset` während Display-Updates |

## Kritische Constraints (aus Reviews)

1. **Backend hat keinen Mode-Param** → muss erweitert werden vor allem anderen.
2. **Back-FB-Drift** bei unabhängigen Zone-Flushes → atomic frame-end flush.
3. **Drawing-Restriction**: `display_fill_screen(15)` muss raus, nur dirty-region zeichnen — sonst kein CPU-Win.
4. **Rotation per Region** statt full-screen.
5. **Counter nur am Frame-Ende** — keine Zone-Resets.
6. **Wake-Pfad**: back_fb=weiß, Panel=Sleep-Image → erster Draw zwingend GC16 full.
7. **Overlay**: kein epdiy-back_fb-Snapshot — entweder `_pfb`-Snapshot oder Underlying-Zone neu zeichnen.
8. **Cross-Zone-Union verboten** — pro Zone ein `epd_hl_update_area`-Call.

## Zielarchitektur

### Intent-basierte API (statt Waveform-Leak)

```cpp
enum class ChangeKind {
    GlyphTick,         // Battery, Clock        → DU4
    HighlightToggle,   // Button-Selection      → DU4
    TextReflow,        // Page-Turn, Body       → GL16
    StructuralRedraw,  // Tab-/Screen-Wechsel   → GL16
    WakeFull,          // Wake from sleep       → GC16 full + counter reset
    SleepImage,        // hold-state preserve   → spezial
    AntiGhost          // forced periodic clean → GC16 full
};

enum class Zone {
    ReaderHeader,
    ReaderBody,
    ReaderFooter,
    Overlay,           // TOC/Menu/Bookmarks/GoTo/Picker
    FullScreen        // Library/Settings/OTA/WiFi/Splash/Error
};

void display_begin_frame();
void display_mark_dirty(Zone z, ChangeKind k);
void display_flush();              // atomic: alle dirty zones, ein poweron, counter++
void display_force_full_refresh(); // escape hatch
```

UI nennt **nie** Waveforms. Mode-Mapping + Anti-Ghost-Policy in `display.cpp`.

### Zone-Regeln

- Pro Zone: eigener Rect, eigenes dirty-flag, eigene gewünschte ChangeKind
- `display_flush()` iteriert dirty Zones, emittiert **ein** `epd_hl_update_area` pro Zone
- Kein Union über Zonen-Grenzen
- `static_assert` dass Reader-Zonen Screen lückenlos tilen

### Back-FB-Invariante

Zeichenoperationen außerhalb aktuell dirty-markierter Zone verboten. Jede Draw-Funktion pro Zone gated durch dirty-flag.

## Phasen

### Phase −1 — Settings-Spike

**Vor** allem anderen. Auf Hardware: Settings mit `GL16` partial probieren (manueller Test-Branch).
- Stripes weg? → Mode-Theorie bestätigt, weiter
- Stripes bleiben? → back_fb-Desync untersuchen bevor API steht

Effort: S | Risk: H | Owner: human-test

### Phase 0 — Backend + Display-Foundation

**Backend (`src/epd_backend_epdiy.{h,cpp}`)**:
- `epd_be_draw_grayscale_image(EpdRect area, const uint8_t* data, EpdDrawMode mode, int temp_c)` — Mode-Param hinzu, an `epd_hl_update_area` / `epd_hl_update_screen` durchreichen
- Temperatur: `epd_ambient_temperature()` aus TPS65185, pro Update lesen + cachen (2s TTL)
- `esp_task_wdt_reset()` in `epd_be_clear_area_cycles`-Loop

**Display (`src/display.{h,cpp}`)**:
- `ChangeKind`, `Zone` Enums in `display.h`
- `Zone`-Tabelle (rect pro Zone)
- `display_begin_frame` / `_mark_dirty` / `_flush` / `_force_full_refresh`
- Mode-Mapping intern: ChangeKind → EpdDrawMode
- `framesSinceFullRefresh` nur in `display_flush()` inkrementiert
- Zone-Flush nutzt `rotatePortraitRegion` (nie `rotatePortraitToLandscape`)
- Anti-Ghost: bei `framesSinceFullRefresh >= threshold` automatisch GC16 full
- Power-Batch: 80 ms Hold-Window vor `poweroff_all`
- `_partialCount` Reset in `display_update_reader_body` entfernen
- Dead code (`extractLandscapeArea`) löschen

**Migration-Shim**: `needsRedraw` bleibt vorerst, ruft intern `display_begin_frame()` am Start jeder Draw-Funktion.

Effort: L | Risk: M | Win: enabler

### Phase 1 — Reader (3 Zonen)

`src/ui/ui_reader.cpp`:
- `display_fill_screen(15)` entfernen
- `ui_reader_draw_header(bool dirty)` — füllt nur Header-Rect `(0,0,540,66)` weiß, zeichnet Battery + Title + Bookmark-Marker, `mark_dirty(ReaderHeader, GlyphTick or StructuralRedraw)`
- `ui_reader_draw_body(bool dirty)` — füllt nur Body-Rect `(0,82,540,~812)` weiß, zeichnet Text/Image, `mark_dirty(ReaderBody, TextReflow)`
- `ui_reader_draw_footer(bool dirty)` — füllt nur Footer-Rect `(0,910,540,50)` weiß, zeichnet Pagenum + Progress, `mark_dirty(ReaderFooter, TextReflow)`
- Top-Level `ui_reader_draw(flags)` setzt dirty-flags basierend auf:
  - `forceFullRefresh` → alle 3 Zonen dirty, ChangeKind `WakeFull` (oder force_full_refresh direkt)
  - Page-Turn → Body dirty (TextReflow), Footer dirty (TextReflow), Header dirty wenn Battery-Delta
  - Battery-Tick only → Header dirty (GlyphTick), DU4
  - Chapter-Jump → Body dirty (TextReflow) + force_full_refresh nach N
- `display_flush()` am Ende ein einziges Mal

Layout-Constants:
```cpp
static_assert(HEADER_HEIGHT + BODY_HEIGHT + FOOTER_HEIGHT == SCREEN_HEIGHT, "Zones must tile");
```

Anti-Ghost threshold: 6-8 Page-Turns (nicht 20).

Effort: L | Risk: M | Win: huge

### Phase 2 — Overlays (TOC/Menu/Bookmarks/GoTo/Picker)

`src/ui/ui_reader.cpp` (`ui_reader_toc_*`, `_menu_*`, `_bookmarks_*`, `_goto_*`):
- Overlay öffnen → `mark_dirty(Overlay, StructuralRedraw)`, flush
- Overlay schließen → underlying Reader-Zone neu zeichnen via `ui_reader_draw_<zone>()`, `mark_dirty(ReaderBody, StructuralRedraw)`, flush
- **Kein** Snapshot

Effort: M | Risk: L | Win: medium

### Phase 3 — Library

`src/ui/ui_library.cpp`:
- Tile-Selection-Highlight → kleines Rect, ChangeKind `HighlightToggle` (DU4)
- Filter-Tab-Wechsel → grid-region + tab-strip, `StructuralRedraw`
- Scroll → grid-region, `StructuralRedraw`
- Header bleibt, wenn nicht betroffen

Effort: M | Risk: L | Win: medium

### Phase 4 — Settings (nach Phase −1)

Wenn Spike OK:
- Per-Row-Update → `StructuralRedraw` row-Rect
- Picker-Overlay → `Overlay`-Zone (siehe Phase 2)
- Tab-Wechsel → content-pane-region, Header bleibt
- Flush-Counter Threshold 8 (nicht turn-count)

Wenn Spike scheitert: Settings bleibt `FullScreen` Zone (medium refresh), Rest profitiert trotzdem.

Effort: M | Risk: H | Win: medium

### Phase 5 — Sonstige Screens

- Sleep-Image: bleibt `display_update_sleep()`, **kein** Partial
- OTA: Progress-Bar als footer-region-Update, DU4 ticks
- WiFi-Setup/Keyboard: `FullScreen` Zone, GC16 (vorerst)
- Splash/Error: `display_force_full_refresh()` immer

Effort: S | Risk: L | Win: small

### Phase 6 — Wake-Pfad-Fix

`src/main.cpp` (Wake-Pfad ~Z.779-970):
- `wakingFromSleep` → erster Draw zwingend `display_force_full_refresh()`
- `showWakeFeedback()` (main.cpp:139): aktuell `display_update_reader_body` vor erstem Full = silent buggy → entweder GC16 full davor oder skippen bis nach erstem Full
- `back_fb` nach Wake nicht trusten

Effort: S | Risk: M | Win: correctness

### Phase 7 — Instrumentation + Tuning

- Log pro Flush: `zone, mode, rect, ms`
- Battery-Messung vor/nach (Reader idle, Reader page-turn)
- `refreshEveryPages` Default {1,2,4,6,10} → {1,2,4,6,8}, Default = 6
- Per-Zone-Forced-Refresh Interval tunen
- `needsRedraw` → `any_zone_dirty()` shim entfernen wenn alle Pfade migriert

Effort: S | Risk: L | Win: quality

## Reihenfolge

`-1 → 0 → 1 → 6 → 2 → 3 → 5 → 4 → 7`

Settings spät weil Spike-Risiko isoliert; Wake-Fix nach Reader weil Reader-Test Wake triggert.

## Mode-Mapping (intern in display.cpp)

| ChangeKind | EpdDrawMode | Begründung |
|---|---|---|
| GlyphTick | DU4 | 4 Graustufen, schnell, kein Flicker, für AA-Glyph + Icon |
| HighlightToggle | DU4 | nicht DU — DU thresholdt AA-Text |
| TextReflow | GL16 | 16 Graustufen, kein Flicker, für Body-Text |
| StructuralRedraw | GL16 | gleicher Grund |
| WakeFull | GC16 | full clear, reset counter |
| SleepImage | spezial | hold-state preserve |
| AntiGhost | GC16 | periodic clean |

## Counter-Logik

```cpp
// In display_flush() am Ende:
framesSinceFullRefresh++;
if (framesSinceFullRefresh >= REFRESH_INTERVAL_READER /* 6-8 */) {
    // upgrade auf GC16 full
    issue_full_gc16();
    framesSinceFullRefresh = 0;
}
```

Settings: separater `flushesSinceFullSettings` counter, threshold 8.

## Test-Plan

| Phase | Test |
|---|---|
| -1 | Hardware-Visual: 20× Settings-Row-Toggle mit GL16 — Stripes? |
| 0 | Build green, alle bestehenden Pfade funktionieren (Zone=FullScreen Shim) |
| 1 | Reader: 50 Page-Turns, kein Ghost auf Body nach forced GC16 every 6-8 |
| 1 | Battery-Tick → nur Header flackert, Body unverändert |
| 2 | Overlay open/close → underlying Reader sauber, kein Stripe |
| 3 | Library: Filter-Tab + Tile-Tap getrennt |
| 4 | Settings: Picker-open/close, kein Ghost |
| 6 | Wake from sleep: erste Anzeige sauber, kein Schmierbild |
| 7 | Logged ms pro Flush sinkt um Faktor X für Battery-Tick (40 mWh → 0.07 mWh erwartet) |

## Risiken + Mitigations

| Risk | Mitigation |
|---|---|
| Settings-Stripes auch mit GL16 | Phase −1 Spike isoliert vor API-Bau |
| Back-FB-Drift bei Multi-Zone-Flush | atomic frame-flush, draw-restriction outside dirty zone |
| Watchdog reset mid GC16 | wdt_reset in clear_area_cycles |
| Wake-Pfad falscher Diff | force GC16 als erstes nach wake |
| Power-PMIC-Wear durch zu viele poweron/off | 80ms hold-window batching |

## Out-of-Scope (vorerst)

- Sleep-FB-Retention in RTC PSRAM
- Debug-HUD-Overlay
- Library-Cover-Snapshot (erst wenn Re-Render zu teuer)
- Touch-ISR-Migration

## Referenzen

- `docs/epidy_partial_updates.md`
- epdiy: https://github.com/vroland/epdiy
- API-Doku: https://epdiy.readthedocs.io/en/latest/api.html
