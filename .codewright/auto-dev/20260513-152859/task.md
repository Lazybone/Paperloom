# Task

Partial-Update-Refactor für Paperloom (ESP32 e-paper reader, epdiy).
Display rendering von "immer Vollbild MODE_GC16" auf dirty-rect partial updates mit intent-basierter API umstellen.

## Quelle

Komplette Architektur + alle kritischen Constraints: `docs/partial_updates_plan.md`.

Plan wurde durch 4-Reviewer-Audit (architect, code-explorer, performance, silent-failure) validiert. CRITICAL findings sind im Plan adressiert.

## Phase 1 — Requirement-Analyst übersprungen

Plan ist explizit + reviewt. Keine Clarification nötig. Direkt zu Phase 2 Planner für WP-Breakdown + Execution-Order.

## User-Vorgaben (vor auto-dev Start)

- Phase −1 (Settings-Spike Hardware-Test) übersprungen — nicht auto-dev-tauglich.
- Settings (Phase 4 im Plan) vorerst als FullScreen-Zone (no-op Migration).
- Reihenfolge: 0 → 1 → 6 → 2 → 3 → 5 → 7 (Settings = 4 als FullScreen-Shim).
- Backend-Mode-Param-Erweiterung (Phase 0) ist Voraussetzung für alles — zuerst.
- Atomic frame-flush + Drawing-Restriction zur Dirty-Region nicht-verhandelbar.
- Counter nur in `display_flush()` am Frame-Ende.
- Pro Zone EIN `epd_hl_update_area`-Call, kein Cross-Zone-Union.
- Wake-Pfad zwingend forced GC16 vor erstem Partial.
- `_partialCount` Reset in `display_update_reader_body` entfernen.
- Dead code (`extractLandscapeArea`) löschen.
- `static_assert` Reader-Zonen tilen lückenlos.
- `epd_ambient_temperature()` lesen + cachen (2s TTL), an Backend durchreichen.
- `esp_task_wdt_reset()` in `epd_be_clear_area_cycles`-Loop.
- `display_fill_screen(15)` in `ui_reader.cpp` durch per-Zone-Fill ersetzen.

## Build-Target

PlatformIO ESP32 (vorhandenes `platformio.ini`). Pro Phase commit. Nach jeder Phase Build verifizieren.
