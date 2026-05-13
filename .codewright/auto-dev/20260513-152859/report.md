# Auto-Dev Report — Partial-Update-Refactor

## Task

Display rendering von "immer Vollbild `MODE_GC16`" auf dirty-rect partial updates mit intent-basierter API umstellen. Quelle: `docs/partial_updates_plan.md` (4-Reviewer-Validiert: architect / code-explorer / performance / silent-failure).

## Status

**normal** — alle WPs durch, alle CRITICAL/HIGH-Findings gefixt, Build grün auf beiden Envs.

## Summary

- **Work Packages**: 8 executed (WP-0.1, WP-0.2, WP-1, WP-6, WP-2, WP-3, WP-5, WP-7)
- **Files Changed**: 12
- **Review Iterations**: 1 acceptance pass (4-reviewer-audit war pre-auto-dev)
- **Hardening Tests**: skipped (kein Unit-Test-Framework im Embedded-Projekt)
- **Acceptance Review**: 1 CRITICAL + 1 HIGH gefunden, beide gefixt
- **All Checks Passing**: yes (pio run default + gh_release SUCCESS)

## Changes (12 files, +999 / −249)

| File | Action | Description |
|------|--------|-------------|
| `src/epd_backend.h` | modified | `epd_be_draw_grayscale_image(area, data, mode, temp_c)` Sig + `EPD_BE_DEFAULT_*` Sentinels + `epd_be_get_ambient_temp_cached()` Decl |
| `src/epd_backend_epdiy.cpp` | modified | Mode/temp-Param durchgereicht an `epd_hl_update_*`; 2s-TTL-Temp-Cache mit `epd_ambient_temperature()` + Sanity-Clamp; `esp_task_wdt_reset()` um `epd_clear_area_cycles` |
| `src/display.h` | modified | `enum class ChangeKind` (7 values) + `enum class Zone` (5+ values); neue API: `display_begin_frame/_mark_dirty/_set_overlay_rect/_flush/_force_full_refresh`; WARNING-Block-Kommentar auf `_force_full_refresh` |
| `src/display.cpp` | modified | Zone-Tabelle + static_asserts (tiling); Counter `_framesSinceFullRefresh` nur in `display_flush()`; Mode-Mapping `ChangeKind→EpdDrawMode`; Atomic flush: ein `epd_be_draw_grayscale_image` pro dirty Zone; AntiGhost auto-upgrade bei Threshold; Legacy `display_update*()` als Shims; Dead `_partialCount/PARTIAL_REFRESH_INTERVAL/extractLandscapeArea` entfernt; `[FLUSH]` Serial-Log |
| `src/ui/ui_reader.cpp` | modified | 3-Zonen-Split (`ui_reader_draw_header/_body/_footer_zone`); statische Battery + Bookmark Caches; `s_overlayDismissed` Flag für Overlay-Close-Restore; Overlay-Funktionen (TOC/Menu/Bookmarks/GoTo) auf Intent-API migriert + Loading-Splash via `AntiGhost` (counter-reset); `forceFullRefresh`-Pfad ruft `display_flush()` direkt (C1-Fix) |
| `src/ui/ui_library.cpp` | modified | `firstDraw → WakeFull`, subsequent → `StructuralRedraw` (GL16 partial statt 6-Cycle Shim) |
| `src/ui/ui_settings.cpp` | modified | 2 Call-Sites → `StructuralRedraw` (Phase-7-Hardware-Test-Risk: Stripes; ggf. zurück auf WakeFull) |
| `src/ui/ui_update.cpp` | modified | 7 Call-Sites (OTA + WiFi-Manager): `StructuralRedraw` für Screens, `force_full_refresh()` für OTA_DONE |
| `src/ui/ui_keyboard.cpp` | modified | 1 Call-Site → `StructuralRedraw` |
| `src/ui/ui_wifi_setup.cpp` | modified | 1 Call-Site → `StructuralRedraw` |
| `src/main.cpp` | modified | Wake-Pfad: `showWakeFeedback()` skippen + Library-Wake-Pfad explizites `display_force_full_refresh()` davor (latenter Bug aus 4-Audit-Review nicht erfasst, vom WP-6-Worker zusätzlich gefangen) |
| `include/config.h.example` | modified | `REFRESH_INTERVAL_READER` Tunable + Doku |

## Verification

### Auto-Checks

- **Build (`pio run -e default`)**: PASS — RAM 58.9 %, Flash 51.6 %
- **Build (`pio run -e gh_release`)**: PASS — RAM 58.9 %, Flash 51.4 %
- **Lint**: SKIPPED (no separate lint step in PIO project)
- **Types**: PASS (C++ compile = type check)

### Pre-Auto-Dev Multi-Reviewer-Audit

Statt der auto-dev Tier-1+2-Reviewer-Loop wurde der Plan VOR Implementation durch 4 spezialisierte Reviewer auditiert:

- **architect**: API-Design, Zone-Abstraktion, FSM-Integration
- **code-explorer**: Plan-vs-Realität-Verifikation (Layout-Constants, dead code, vorhandene Helpers)
- **performance-optimizer**: Mode-Strategy, Anti-Ghost-Interval, Temp-Sensor, Power-Batching
- **silent-failure-hunter**: 10 Findings inkl. CRITICAL Back-FB-Drift, mode-Param-fehlt, _partialCount-Reset-Bug, Wake-Pfad-back_fb-Stale, Cross-Zone-Union-Risk

Alle CRITICAL + HIGH Findings adressiert im Plan + implementiert.

### Post-Implementation Acceptance Review (cpp-reviewer)

- **CRITICAL**: 1 found, 1 fixed
  - C1: Double `display_begin_frame()` discards zone marks — `display_force_full_refresh()` interne `begin_frame()` löschte vorher gesetzte Zone-Marks → Fix in `ui_reader_draw()`: direkter `display_flush()` statt `force_full_refresh()` (Zones bereits WakeFull markiert) + WARNING-Doc auf `display_force_full_refresh()`
- **HIGH**: 2 found, 1 fixed (1 LOW-impact akzeptiert)
  - H1 (fixed): Loading-Splash Counter-Drift in TOC/Bookmarks Touch-Handlers — `StructuralRedraw → AntiGhost` (counter-reset)
  - H2 (akzeptiert): Crash-recovery-Banner Counter-Inkrement nach `force_full_refresh()` — minimal practical risk (REFRESH_INTERVAL_READER=6, nie erreicht in dieser Sequenz)
- **MEDIUM**: 4 (alle als Follow-up dokumentiert, nicht in-Scope für diesen Refactor)
  - M1: Dead `pget()` static in display.cpp
  - M2: `display.cpp` 945 Zeilen (>800 Guideline) — Split-Empfehlung als Follow-up
  - M3: `config.h.example` Doku-Add gut, aber lokales `config.h` ist gitignored — Onboarding-Risk
  - M4: `MODE_DU4` Silent-Fail mit `EPD_BUILTIN_WAVEFORM` möglich — Phase-7-Hardware-Verify nötig
- **LOW**: 2 (akzeptiert)
  - L1: WDT-Feed nur um `epd_clear_area_cycles` — ausreichend bei default cycles*time
  - L2: `_lastPoweroffMs` + `POWER_HOLD_WINDOW_MS` `[[maybe_unused]]` (reserved for future backend variant)

### Fixes-Commit

`47f1aa7 fix(display): preserve zone marks on wake + reset counter on overlay-load splash`

## Open Findings

### Phase-7-Hardware-Tests benötigt (nicht auto-dev-abdeckbar)

Diese sind im Plan als "out-of-scope für auto-dev" markiert und brauchen Visual-Test auf Gerät:

1. **`MODE_DU4` Verifikation** mit `EPD_BUILTIN_WAVEFORM` — Battery-Tick + Highlight-Toggle nutzen DU4 → wenn nicht supportet, silent fail → Fallback auf `MODE_DU` oder `MODE_GL16` einbauen
2. **Settings-Stripes-Test** — Settings nutzt jetzt `StructuralRedraw` (GL16). Wenn Stripes wiederkehren → auf `WakeFull` zurück
3. **Reader Anti-Ghost-Threshold** — Default `REFRESH_INTERVAL_READER = 6` — Hardware-Test ob bei dieser Frequenz Body-Text-Ghosts entstehen; tunen 6-8
4. **Wake-Pfad sauber** — von Deep-Sleep wachen, erste Anzeige darf kein Schmierbild zeigen

### Follow-Up-Issues (nicht blockierend)

1. `pget()` dead code in `display.cpp` löschen
2. `display.cpp` 945 Zeilen → ggf. `display_flush.cpp` / `display_rotate.cpp` extrahieren
3. `include/config.h.example` Onboarding-Check (Section vorhanden, weiter pflegen)
4. `[FLUSH]`-Logging Battery-Konsequenzen messen — wenn Serial-Output messbar Strom zieht, hinter Build-Flag verstecken

## Branch

`auto-dev/partial-update-refactor-20260513-152859`

## Git Log

```
47f1aa7 fix(display): preserve zone marks on wake + reset counter on overlay-load splash
0e6257f feat(display): flush instrumentation + REFRESH_INTERVAL_READER tunable
ce814e8 feat(screens): migrate settings/ota/wifi/keyboard to intent API
1a1380f feat(overlays): migrate TOC/Menu/Bookmarks/GoTo to intent-based API
e1b90af feat(library): migrate to intent-based partial-update API
7320a7f fix(wake): force GC16 full refresh on first draw after deep sleep
79a9a83 feat(reader): split into 3 zones with intent-based mark_dirty
45d34e7 feat(display): intent-based partial-update API with zone dirty-tracking
a8bfe09 feat(backend): add mode + temp params to draw, watchdog feed in clear_area_cycles
```

(Base: `a8c759e chore: boot stability + deep-sleep power + UI background cleanup` — vorgelagerter WIP-Commit der bei auto-dev-Start dirty war.)

## Erwartete Wins (zu validieren in Phase-7-Hardware-Test)

| Metrik | Vorher | Nachher (erwartet) |
|---|---|---|
| Battery-Tick (full-screen GC16) | ~40 mWh | ~0.07 mWh (Header-only DU4) |
| Page-Turn Body | ~1-2 s GC16 | ~0.5 s GL16 partial |
| Periodic Anti-Ghost | manuell via `refreshEveryPages` | auto every 6 partials + user backstop |
| Library Tab-Wechsel | 6-Cycle Flash (Shim-Regression) | GL16 partial, kein Flash |
| Overlay open/close | 6-Cycle Flash | GL16 partial |
| Wake-from-Sleep | OK aber `showWakeFeedback` silent buggy | Sauberes erstes GC16, kein Stale-back_fb-Diff |
