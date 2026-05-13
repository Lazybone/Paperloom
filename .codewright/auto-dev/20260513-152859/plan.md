# Auto-Dev Plan — Partial-Update-Refactor

Source: `docs/partial_updates_plan.md` (4-reviewer-validated).
Output from Phase 2 Planner.

## Task Overview

- **Goal**: Display rendering von "immer Vollbild MODE_GC16" auf dirty-rect partial updates mit intent-basierter API umstellen.
- **Approach**: Backend Mode-Parameter erweitern. Display-Layer bekommt intent-basierte API (ChangeKind/Zone). Reader splittet in 3 Zonen (Header/Body/Footer) mit atomic frame-flush. Wake-Pfad fixt. Library/Overlays/Sonstige nutzen FullScreen-Shim oder neue API.

## Work Packages

### WP-0.1: Backend-Mode-Param + Temp-Caching + WDT-Reset
- **Files**: `src/epd_backend.h`, `src/epd_backend_epdiy.cpp`
- **Action**: modify
- **Description**:
  - `epd_be_draw_grayscale_image(EpdRect area, uint8_t* data, int mode, int temp_c)` — neue Sig mit Mode + Temp
  - Mode an `epd_hl_update_area` / `epd_hl_update_screen` durchreichen statt hardcoded `MODE_GC16`
  - `get_ambient_temperature()` Helper: ruft `epd_ambient_temperature()`, cacht 2s (TTL via `millis()`)
  - `esp_task_wdt_reset()` in `epd_be_clear_area_cycles` — Loop um `epd_clear_area_cycles` mit Feeds (siehe upstream ob iterierbar; sonst `vTaskDelay(1)` als Tick-Trigger)
- **Depends on**: []

### WP-0.2: Display-Foundation — Intent-API + Zone-Tabelle + Shim
- **Files**: `src/display.h`, `src/display.cpp`
- **Action**: modify
- **Description**:
  - `enum class ChangeKind` (GlyphTick/HighlightToggle/TextReflow/StructuralRedraw/WakeFull/SleepImage/AntiGhost) in `display.h`
  - `enum class Zone` (ReaderHeader/ReaderBody/ReaderFooter/Overlay/FullScreen) in `display.h`
  - `display_begin_frame()`, `display_mark_dirty(Zone, ChangeKind)`, `display_flush()`, `display_force_full_refresh()`
  - Zone-Tabelle (`ZoneState _zones[5]` mit rect + dirty + intent)
  - Mode-Mapping `changekind_to_mode()` intern
  - `framesSinceFullRefresh` counter nur in `display_flush()` am Frame-Ende
  - AntiGhost-Trigger: counter >= `REFRESH_INTERVAL_READER` (default 6) → upgrade alle Reader-Zonen auf `WakeFull`
  - **Region-Rotation**: nutze `rotatePortraitRegion` (siehe Display-Helpers, bereits existent in display.cpp), nicht `rotatePortraitToLandscape`
  - **Atomic flush**: pro dirty Zone EIN `epd_be_draw_grayscale_image`-Call, kein Cross-Zone-Union
  - **Power-Batching**: 80ms hold-window vor `epd_be_poweroff_all`
  - **Shim**: Existierende `display_update*()` Funktionen umbauen auf `begin_frame + mark_dirty + flush` (für Legacy-Caller)
  - **Dead-Code-Removal**: `_partialCount` Variable + alle Resets entfernen; `extractLandscapeArea()` löschen
- **Depends on**: [WP-0.1]

### WP-1: Reader 3-Zonen-Split
- **Files**: `src/ui/ui_reader.cpp`
- **Action**: modify
- **Description**:
  - `display_fill_screen(15)` in `ui_reader_draw()` (Z.82) entfernen
  - 3 neue static Funktionen: `ui_reader_draw_header()`, `ui_reader_draw_body()`, `ui_reader_draw_footer()` — jede füllt nur ihren Zone-Rect weiß + zeichnet ihren Content
  - Top-Level `ui_reader_draw()` entscheidet pro Zone via `readerRefresh`-Flags + Battery-Delta:
    - Force-Full → alle 3 Zonen + `display_force_full_refresh()`
    - Page-Turn → Body + Footer mit `TextReflow`
    - Battery-Tick only → Header mit `GlyphTick`
    - Chapter-Jump → Body mit `TextReflow`
  - `static_assert(HEADER_HEIGHT + BODY_HEIGHT + FOOTER_HEIGHT == SCREEN_HEIGHT - GAP)` in display.cpp oder ui_reader.cpp
  - Anti-Ghost-Threshold: 6 (statt 20)
- **Depends on**: [WP-0.2]

### WP-6: Wake-Pfad-Fix
- **Files**: `src/main.cpp`
- **Action**: modify
- **Description**:
  - `showWakeFeedback()` Aufruf (Z.139) entweder skippen wenn `wakingFromSleep` oder NACH erstem GC16 full
  - Wake-Pfad (~Z.779-790): erster Draw muss `readerRefresh.forceFullRefresh = true` setzen → erster `ui_reader_draw` macht GC16 full → back_fb sync
  - Kommentar dokumentiert: back_fb nach Wake nicht trusten bis erster GC16 full
- **Depends on**: [WP-1]

### WP-2: Overlays — Underlying Redraw statt Snapshot
- **Files**: `src/ui/ui_reader.cpp` (Overlay-Pfade), zusätzlich evtl. neue Helpers
- **Action**: modify
- **Description**:
  - TOC/Menu/Bookmarks/GoTo/Picker open: `mark_dirty(Overlay, StructuralRedraw)` + Overlay-Rect setzen, flush
  - Close: underlying Reader-Zone (meist Body) via `ui_reader_draw_body()` neu zeichnen + `mark_dirty(ReaderBody, StructuralRedraw)`, flush
  - **Kein** epdiy-back_fb-Snapshot, kein `_pfb`-Stash
- **Depends on**: [WP-1]

### WP-3: Library Partial Updates
- **Files**: `src/ui/ui_library.cpp`
- **Action**: modify
- **Description**:
  - Tile-Selection-Highlight: kleines rect, `HighlightToggle` (DU4)
  - Filter-Tab-Wechsel: tab-strip + grid-region, `StructuralRedraw`
  - Scroll: grid-region, `StructuralRedraw`
  - Header bleibt unangetastet wenn nicht betroffen
- **Depends on**: [WP-0.2]

### WP-5: Sonstige Screens (OTA, WiFi, Settings-Shim, Sleep-Image, Splash, Error)
- **Files**: `src/ui/ui_settings.cpp`, `src/ui/ui_wifi.cpp` (falls vorhanden), `src/ui/ui_ota.cpp` oder `src/ui/ui_update.cpp`, `src/sleep_image.cpp`, andere `src/ui/*.cpp` mit `display_update*`-Calls
- **Action**: modify
- **Description**:
  - **Settings** (Phase 4 im Original-Plan): vorerst FullScreen-Zone, no-op-Migration — wrap existing `display_update_medium()` in `begin_frame + mark_dirty(FullScreen, StructuralRedraw) + flush`
  - **OTA-Progress**: footer-region wenn möglich, `GlyphTick` (DU4) für ticks; sonst FullScreen
  - **WiFi-Setup/Keyboard**: FullScreen-Zone, `StructuralRedraw`
  - **Sleep-Image**: BLEIBT `display_update_sleep()` — kein Partial, hold-state preserve
  - **Splash/Error**: `display_force_full_refresh()` immer
- **Depends on**: [WP-0.2]

### WP-7: Instrumentation + Tuning + needsRedraw-Cleanup
- **Files**: `src/display.cpp`, `include/config.h`, `src/main.cpp` (für `needsRedraw`-Shim)
- **Action**: modify
- **Description**:
  - Log pro Flush: `[FLUSH] zones=N, mode=..., rect=(x,y,w,h), ms=T`
  - `REFRESH_INTERVAL_READER` als Define in `config.h` (default 6)
  - Wenn alle UI-Pfade migriert: `needsRedraw` bleibt, aber Doku-Kommentar dass es jetzt im Wesentlichen `display_begin_frame`-Trigger ist
- **Depends on**: [WP-1, WP-2, WP-3, WP-5, WP-6]

## Execution Order

WP-0.2 und WP-0.3 wurden zu einem WP-0.2 zusammengeführt (gleiche Datei `display.cpp`, kein echter Parallelvorteil).

- **Group 1 (sequential)**: WP-0.1
- **Group 2 (sequential, depends on G1)**: WP-0.2
- **Group 3 (sequential, depends on G2)**: WP-1
- **Group 4 (sequential, depends on G3)**: WP-6
- **Group 5 (parallel, depends on G4)**: WP-2, WP-3, WP-5
- **Group 6 (sequential, depends on G5)**: WP-7

Pro Group: commit + `pio run` build-verify.

## UI Mockup Assessment

- **ui_mockup**: not_needed
- **Rationale**: Display-Layer-Refactor — kein sichtbarer Layout/Color/Position-Change. Nur Refresh-Verhalten (schneller, weniger Flicker). Pixel-Output identisch.
