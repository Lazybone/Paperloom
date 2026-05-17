#include "ui_settings.h"
#include "config.h"
#include "../settings.h"
#include "../display.h"
#include "../battery.h"
#include "../reader.h"
#include "../cover_renderer.h"
#include "../frontlight.h"
#include "../button_action.h"

#include "ui_common.h"

// ─── Layout constants ───────────────────────────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;
static const int FONT_H = 50;
static const int SETTINGS_ROW_H = FONT_H + 8;
static const int SETTINGS_TAB_H = 64;

// Page identifiers
static constexpr int PAGE_READING = 0;
static constexpr int PAGE_LIBRARY = 1;
static constexpr int PAGE_DEVICE  = 2;
static constexpr int PAGE_BUTTONS = 3;  // Submenu reached from Device

static int settingsPage = PAGE_READING;

// Lets a touch handler that has already done its own partial flush
// suppress the post-touch full repaint from handleSettingsTouch().
// Same pattern used in ui_reader.cpp / ui_toast.cpp.
extern void setNeedsRedraw(bool val);

// Generic picker overlay. Used for button-action selection (per gesture),
// brightness level, and font family.  Only one picker is active at a time.
enum PickerKind {
    PICKER_NONE = 0,
    PICKER_BUTTON_ACTION,
    PICKER_BRIGHTNESS,
    PICKER_FONT_FAMILY,
};

static PickerKind pickerKind   = PICKER_NONE;
static int        pickerTarget = -1;  // PICKER_BUTTON_ACTION: 0..5 → buttonPickerSlot()

static const int BRIGHTNESS_STEP_COUNT = 11;  // 0%, 10%, …, 100%

static uint8_t* buttonPickerSlot(Settings& s, int target) {
    switch (target) {
        case 0: return &s.userButtonTapAction;
        case 1: return &s.userButtonDoubleAction;
        case 2: return &s.userButtonLongAction;
        case 3: return &s.bootButtonTapAction;
        case 4: return &s.bootButtonDoubleAction;
        case 5: return &s.bootButtonLongAction;
        default: return nullptr;
    }
}

static const char* buttonPickerTitle(int target) {
    switch (target) {
        case 0: return "IO48 Tap";
        case 1: return "IO48 Double";
        case 2: return "IO48 Hold";
        case 3: return "Boot Tap";
        case 4: return "Boot Double";
        case 5: return "Boot Hold";
        default: return "Action";
    }
}

static int pickerItemCount() {
    switch (pickerKind) {
        case PICKER_BUTTON_ACTION: return BTN_ACTION_COUNT;
        case PICKER_BRIGHTNESS:    return BRIGHTNESS_STEP_COUNT;
        case PICKER_FONT_FAMILY:   return FONT_FAMILY_COUNT;
        default:                   return 0;
    }
}

static const char* pickerItemLabel(int i, char* buf, size_t n) {
    switch (pickerKind) {
        case PICKER_BUTTON_ACTION:
            return button_action_name((uint8_t)i);
        case PICKER_BRIGHTNESS:
            snprintf(buf, n, "%d%%", i * 10);
            return buf;
        case PICKER_FONT_FAMILY:
            return (i >= 0 && i < FONT_FAMILY_COUNT) ? kFontFamilyNames[i] : "?";
        default:
            return "";
    }
}

static int pickerCurrentIndex(Settings& s) {
    switch (pickerKind) {
        case PICKER_BUTTON_ACTION: {
            uint8_t* slot = buttonPickerSlot(s, pickerTarget);
            return slot ? *slot : 0;
        }
        case PICKER_BRIGHTNESS: {
            int idx = (s.frontlightBrightness + 5) / 10;
            if (idx < 0) idx = 0;
            if (idx >= BRIGHTNESS_STEP_COUNT) idx = BRIGHTNESS_STEP_COUNT - 1;
            return idx;
        }
        case PICKER_FONT_FAMILY:
            return (s.fontFamily < FONT_FAMILY_COUNT) ? s.fontFamily : 0;
        default:
            return -1;
    }
}

static const char* pickerHeader(char* buf, size_t n) {
    switch (pickerKind) {
        case PICKER_BUTTON_ACTION:
            snprintf(buf, n, "Action: %s", buttonPickerTitle(pickerTarget));
            return buf;
        case PICKER_BRIGHTNESS:  return "Brightness";
        case PICKER_FONT_FAMILY: return "Font Family";
        default:                 return "";
    }
}

static void pickerApply(Settings& s, int idx) {
    switch (pickerKind) {
        case PICKER_BUTTON_ACTION: {
            uint8_t* slot = buttonPickerSlot(s, pickerTarget);
            if (slot) *slot = (uint8_t)idx;
            break;
        }
        case PICKER_BRIGHTNESS: {
            int v = idx * 10;
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            s.frontlightBrightness = (uint8_t)v;
            frontlight_apply_from_settings();
            break;
        }
        case PICKER_FONT_FAMILY:
            if (idx >= 0 && idx < FONT_FAMILY_COUNT) {
                s.fontFamily = (uint8_t)idx;
                display_set_font(s.fontSizeLevel, s.fontFamily);
            }
            break;
        default:
            break;
    }
}

static void pickerClose() {
    pickerKind   = PICKER_NONE;
    pickerTarget = -1;
}

// Persist settings immediately after a user-visible change.  Failures are
// logged but non-fatal — the in-RAM state still reflects the new value
// and the on-back save in the footer handler is a second chance.
static void saveSettingsNow() {
    if (!settings_save()) {
        Serial.println("Settings: immediate save failed");
    }
}

// ─── Settings option arrays ─────────────────────────────────────────
static const char* fontSizeNames[] = {"XS", "S", "M", "M-L", "L"};
static_assert(sizeof(fontSizeNames) / sizeof(fontSizeNames[0]) == FONT_SIZE_LEVEL_COUNT,
              "fontSizeNames must have one entry per FONT_SIZE_LEVEL_COUNT");
static const char* lineSpacingNames[] = {"Compact", "Normal", "Relaxed", "Spacious", "Extra"};
static_assert(sizeof(lineSpacingNames) / sizeof(lineSpacingNames[0]) == LINE_SPACING_LEVEL_COUNT,
              "lineSpacingNames must have one entry per LINE_SPACING_LEVEL_COUNT");
static const char* libraryViewNames[] = {"List", "Poster"};
static const char* librarySortNames[] = {"Title", "Author", "Recent", "Size"};
static const int sleepOptions[] = {2, 5, 10, 15, 30};
static const int NUM_SLEEP_OPTIONS = 5;
static const int refreshOptions[] = {1, 2, 4, 6, 10};
static const int NUM_REFRESH_OPTIONS = 5;

// ─── Helpers ────────────────────────────────────────────────────────

static int findSleepIdx() {
    int val = settings_get().sleepTimeoutMin;
    for (int i = 0; i < NUM_SLEEP_OPTIONS; i++) {
        if (sleepOptions[i] == val) return i;
    }
    return 1;
}

static int findRefreshIdx() {
    int val = settings_get().refreshEveryPages;
    for (int i = 0; i < NUM_REFRESH_OPTIONS; i++) {
        if (refreshOptions[i] == val) return i;
    }
    return 1;
}

static const char* pageTitle(int page) {
    switch (page) {
        case PAGE_READING: return "Settings: Reading";
        case PAGE_LIBRARY: return "Settings: Library";
        case PAGE_DEVICE:  return "Settings: Device";
        case PAGE_BUTTONS: return "Settings: Buttons";
        default:           return "Settings";
    }
}

static int rowCountForPage(int page) {
    switch (page) {
        case PAGE_READING: return 7;
        case PAGE_LIBRARY: return 3;
        case PAGE_DEVICE:  return 8;
        case PAGE_BUTTONS: return 8;
        default:           return 0;
    }
}

// Tab bar directly under header. Active tab inverts to white; band is light gray.
static void drawSettingsTabs() {
    int tabY = HEADER_HEIGHT;
    display_draw_filled_rect(0, tabY, W, SETTINGS_TAB_H, 15);

    const char* tabLabels[3] = {"Reading", "Library", "Device"};
    int tabW = W / 3;
    for (int i = 0; i < 3; i++) {
        int tx = i * tabW;
        if (settingsPage == i) {
            display_draw_filled_rect(tx + 2, tabY + 2, tabW - 4, SETTINGS_TAB_H - 4, 15);
            display_draw_hline(tx + 2, tabY + SETTINGS_TAB_H - 2, tabW - 4, 0);
        }
        int tw = display_text_width(tabLabels[i]);
        display_draw_text(tx + (tabW - tw) / 2, tabY + SETTINGS_TAB_H - 18, tabLabels[i], 0);
    }
    display_draw_hline(0, tabY + SETTINGS_TAB_H, W, 10);
}

// Draw a single labeled row with a right-aligned value, plus underline.
static void drawRow(int y, const char* label, const char* value) {
    display_draw_text(MARGIN_X, y + FONT_H - 4, label, 0);
    int vw = display_text_width(value);
    display_draw_text(W - MARGIN_X - vw, y + FONT_H - 4, value, 0);
    display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
}

// Mark and flush a single settings row as a partial Overlay update.
// Caller MUST have already drawn the row's content into the framebuffer
// (typically a display_draw_filled_rect over the row's y-band + a drawRow
// call). Picks Zone::Overlay because the rect is dynamic per call (each
// settings row has its own y).
//
// Use sparingly: every flush pays one PMIC poweron/poweroff bracket
// (~10–15 ms on top of the waveform cost). For burst row changes the
// user-perceivable cost is dominated by tap-to-tap human cadence anyway.
//
// Future work: factor the row-drawing logic out of ui_settings_draw so
// the touch handler can call e.g. drawRowAt(rowIdx) + flushSettingsRow()
// without recomputing layout. Until that's done, this helper is wired
// only into the Page Numbers toggle as a pilot.
static void flushSettingsRow(int row_y, int row_h, ChangeKind kind) {
    display_set_overlay_rect(0, row_y, W, row_h);
    display_mark_dirty(Zone::Overlay, kind);
    display_flush();
}

// ═══════════════════════════════════════════════════════════════════
// Settings screen drawing
// ═══════════════════════════════════════════════════════════════════

void ui_settings_draw(bool& settingsFromLibrary) {
    display_set_font_size(2);  // chrome always in Inter

    // Picker overlay (button action / brightness / font family) short-
    // circuits the regular settings draw.
    if (pickerKind != PICKER_NONE) {
        display_fill_screen(15);
        char hdrBuf[48];
        drawHeader(pickerHeader(hdrBuf, sizeof(hdrBuf)));

        Settings& s = settings_get();
        int current = pickerCurrentIndex(s);
        int count   = pickerItemCount();

        int yy = HEADER_HEIGHT + MARGIN_Y + 10;
        for (int i = 0; i < count; i++) {
            char nameBuf[24];
            const char* name = pickerItemLabel(i, nameBuf, sizeof(nameBuf));
            // Highlight current selection with a marker on the left.
            const char* marker = (i == current) ? "> " : "  ";
            char line[48];
            snprintf(line, sizeof(line), "%s%s", marker, name);
            display_draw_text(MARGIN_X, yy + FONT_H - 4, line, 0);
            display_draw_hline(MARGIN_X, yy + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
            yy += SETTINGS_ROW_H;
        }

        drawBottomBar("[ Cancel ]");
        // Uses Zone::FullScreen, NOT Zone::Overlay: this overlay paints
        // drawHeader at y=0..66 and drawBottomBar at y=910..960 — both
        // outside the body-overlay rect (66..910). FullScreen + GL16
        // partial sends the whole portrait buffer so chrome stays
        // visible. (Same constraint as TOC/Bookmarks/GoTo in ui_reader.cpp.)
        display_begin_frame();
        display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
        display_flush();
        settingsFromLibrary = false;
        return;
    }

    display_fill_screen(15);
    drawHeader(pageTitle(settingsPage));

    bool isSubmenu = (settingsPage == PAGE_BUTTONS);
    if (!isSubmenu) {
        drawSettingsTabs();
    }

    Settings& s = settings_get();
    int y = HEADER_HEIGHT + (isSubmenu ? 0 : SETTINGS_TAB_H) + MARGIN_Y + 10;

    if (settingsPage == PAGE_READING) {
        // Font Size
        char fsLabel[32];
        snprintf(fsLabel, sizeof(fsLabel), "< %s >", fontSizeNames[s.fontSizeLevel]);
        drawRow(y, "Font Size", fsLabel);
        y += SETTINGS_ROW_H;

        // Font Family — Sans (Lexend) / Serif (Literata) / Slab (Bitter).
        // Label strings come from kFontFamilyNames in display.cpp so the
        // UI cannot drift from the serial-log labels.
        char ffLabel[32];
        uint8_t famIdx = s.fontFamily;
        if (famIdx >= FONT_FAMILY_COUNT) famIdx = 0;
        snprintf(ffLabel, sizeof(ffLabel), "< %s >", kFontFamilyNames[famIdx]);
        drawRow(y, "Font Family", ffLabel);
        y += SETTINGS_ROW_H;

        // Line spacing
        char lsLabel[32];
        snprintf(lsLabel, sizeof(lsLabel), "< %s >",
                 lineSpacingNames[min((int)s.lineSpacingLevel, LINE_SPACING_LEVEL_COUNT - 1)]);
        drawRow(y, "Line Spacing", lsLabel);
        y += SETTINGS_ROW_H;

        // Font preview line in selected font size + family
        display_set_font(s.fontSizeLevel, s.fontFamily);
        const char* preview = "The quick brown fox jumps over the lazy dog";
        display_draw_text(MARGIN_X, y + display_font_height() - 4, preview, 0);
        display_draw_hline(MARGIN_X, y + SETTINGS_ROW_H - 4, W - MARGIN_X * 2, 12);
        y += SETTINGS_ROW_H;
        display_set_font_size(2);  // back to UI font

        // Sleep Timeout
        char slLabel[32];
        snprintf(slLabel, sizeof(slLabel), "< %dm >", s.sleepTimeoutMin);
        drawRow(y, "Sleep Timeout", slLabel);
        y += SETTINGS_ROW_H;

        // Cleanup Refresh
        char rfLabel[32];
        snprintf(rfLabel, sizeof(rfLabel), "< %d pg >", s.refreshEveryPages);
        drawRow(y, "Cleanup Refresh", rfLabel);
        y += SETTINGS_ROW_H;

        // Page Numbers
        drawRow(y, "Page Numbers", s.showPageNumbers ? "[ ON ]" : "[ OFF ]");
        y += SETTINGS_ROW_H;
    } else if (settingsPage == PAGE_LIBRARY) {
        // Library View
        char lvLabel[32];
        snprintf(lvLabel, sizeof(lvLabel), "< %s >", libraryViewNames[s.libraryViewMode]);
        drawRow(y, "Library View", lvLabel);
        y += SETTINGS_ROW_H;

        // Library Sort
        char sortLabel[32];
        snprintf(sortLabel, sizeof(sortLabel), "< %s >", librarySortNames[min((int)s.librarySortOrder, 3)]);
        drawRow(y, "Library Sort", sortLabel);
        y += SETTINGS_ROW_H;

        // Poster Covers
        drawRow(y, "Poster Covers", s.posterShowCovers ? "[ ON ]" : "[ OFF ]");
        y += SETTINGS_ROW_H;
    } else if (settingsPage == PAGE_DEVICE) {
        // Battery Display
        drawRow(y, "Battery Display", s.showBattery ? "[ ON ]" : "[ OFF ]");
        y += SETTINGS_ROW_H;

        // Frontlight on/off
        drawRow(y, "Frontlight", s.frontlightEnabled ? "[ ON ]" : "[ OFF ]");
        y += SETTINGS_ROW_H;

        // Frontlight brightness
        char flbLabel[32];
        snprintf(flbLabel, sizeof(flbLabel), "< %d%% >", s.frontlightBrightness);
        drawRow(y, "Brightness", flbLabel);
        y += SETTINGS_ROW_H;

        // Button Setup → submenu
        drawRow(y, "Button Setup", "[ > ]");
        y += SETTINGS_ROW_H;

        // WiFi Manager
        drawRow(y, "WiFi Manager", "[ Open ]");
        y += SETTINGS_ROW_H;

        // WiFi Setup (on-device scanner + keyboard)
        drawRow(y, "WiFi Setup", "[ Configure ]");
        y += SETTINGS_ROW_H;

        // WiFi SSID (display only)
        String ssid = s.wifiSSID;
        if (ssid.length() > 15) ssid = ssid.substring(0, 12) + "...";
        drawRow(y, "WiFi SSID", ssid.c_str());
        y += SETTINGS_ROW_H;

        // Firmware Update
        drawRow(y, "Firmware Update", "[ Check ]");
        y += SETTINGS_ROW_H;
    } else if (settingsPage == PAGE_BUTTONS) {
        // Boot Button enable
        drawRow(y, "Boot Button", s.bootButtonEnabled ? "[ ON ]" : "[ OFF ]");
        y += SETTINGS_ROW_H;

        // Boot Tap action
        char bootTapLabel[32];
        snprintf(bootTapLabel, sizeof(bootTapLabel), "< %s >",
                 button_action_name(s.bootButtonTapAction));
        drawRow(y, "Boot Tap", bootTapLabel);
        y += SETTINGS_ROW_H;

        // Boot Double-tap action
        char bootDblLabel[32];
        snprintf(bootDblLabel, sizeof(bootDblLabel), "< %s >",
                 button_action_name(s.bootButtonDoubleAction));
        drawRow(y, "Boot Double", bootDblLabel);
        y += SETTINGS_ROW_H;

        // Boot Long-press action
        char bootLngLabel[32];
        snprintf(bootLngLabel, sizeof(bootLngLabel), "< %s >",
                 button_action_name(s.bootButtonLongAction));
        drawRow(y, "Boot Hold", bootLngLabel);
        y += SETTINGS_ROW_H;

        // User Button enable
        drawRow(y, "IO48 Button", s.userButtonEnabled ? "[ ON ]" : "[ OFF ]");
        y += SETTINGS_ROW_H;

        // Tap action
        char btnTapLabel[32];
        snprintf(btnTapLabel, sizeof(btnTapLabel), "< %s >",
                 button_action_name(s.userButtonTapAction));
        drawRow(y, "IO48 Tap", btnTapLabel);
        y += SETTINGS_ROW_H;

        // Double-tap action
        char btnDblLabel[32];
        snprintf(btnDblLabel, sizeof(btnDblLabel), "< %s >",
                 button_action_name(s.userButtonDoubleAction));
        drawRow(y, "IO48 Double", btnDblLabel);
        y += SETTINGS_ROW_H;

        // Long-press action
        char btnLngLabel[32];
        snprintf(btnLngLabel, sizeof(btnLngLabel), "< %s >",
                 button_action_name(s.userButtonLongAction));
        drawRow(y, "IO48 Hold", btnLngLabel);
        y += SETTINGS_ROW_H;
    }

    drawBottomBar("[ Back ]");
    // Settings is a FullScreen zone in the intent API. The hardware spike
    // (Phase -1 of the partial-update refactor) was skipped, so per-row
    // partials are out of scope here — the screen is treated as one big
    // surface.
    //
    // StructuralRedraw (GL16 non-flashing partial). First hardware test
    // showed ghosting under GL16 because drawHeader / drawBottomBar were
    // filling the header/footer bands with dark gray (value 2) — the
    // gray-to-gray transitions don't settle under partial waveforms and
    // left visible text shadow on tab switch. drawHeader/drawBottomBar
    // were converted to white-fill + black-text + single rule line,
    // which transitions cleanly under GL16. The anti-ghost counter
    // (REFRESH_INTERVAL_READER, applied to all screens) enforces a
    // periodic clean GC16 refresh.
    display_begin_frame();
    display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
    display_flush();
    settingsFromLibrary = false;
}

// ═══════════════════════════════════════════════════════════════════
// Settings touch handling
// ═══════════════════════════════════════════════════════════════════

AppState ui_settings_touch(int x, int y, BookReader& reader) {
    // Picker overlay handles its own hit-testing.
    if (pickerKind != PICKER_NONE) {
        // Footer → cancel (no change).
        if (y > H - FOOTER_HEIGHT) {
            pickerClose();
            return STATE_SETTINGS;
        }
        int rowY = HEADER_HEIGHT + MARGIN_Y + 10;
        int row  = (y - rowY) / SETTINGS_ROW_H;
        if (row >= 0 && row < pickerItemCount()) {
            Settings& s = settings_get();
            pickerApply(s, row);
            pickerClose();
            saveSettingsNow();
        }
        return STATE_SETTINGS;
    }

    // Footer → back
    if (y > H - FOOTER_HEIGHT) {
        // Buttons submenu: back goes to Device page, stay in settings
        if (settingsPage == PAGE_BUTTONS) {
            settingsPage = PAGE_DEVICE;
            return STATE_SETTINGS;
        }

        if (!settings_save()) {
            Serial.println("Settings: save failed on back-button (SD?)");
        }
        if (reader.isOpen()) {
            int savedChapter = reader.getCurrentChapter();
            int savedPage = reader.getCurrentPage();
            reader.recalculateLayout();
            reader.jumpToChapter(savedChapter);
            reader.restorePage(savedPage);
            return STATE_READER;
        } else {
            return STATE_LIBRARY;
        }
    }

    bool isSubmenu = (settingsPage == PAGE_BUTTONS);

    // Top tab bar hit-test (main pages only)
    if (!isSubmenu && y >= HEADER_HEIGHT && y < HEADER_HEIGHT + SETTINGS_TAB_H) {
        int tabW = W / 3;
        int idx = x / tabW;
        if (idx < 0) idx = 0;
        if (idx > 2) idx = 2;
        settingsPage = idx;
        return STATE_SETTINGS;
    }

    Settings& s = settings_get();
    int rowY = HEADER_HEIGHT + (isSubmenu ? 0 : SETTINGS_TAB_H) + MARGIN_Y + 10;

    int row = (y - rowY) / SETTINGS_ROW_H;
    // Arrow values like "< 30% >" render right-aligned, so both '<' and '>'
    // sit in the right half of the screen. Splitting at W/2 made every tap
    // on the value an INCREMENT — invisible for wrap-around settings, but
    // it locked Brightness at its max because Brightness clamps. Push the
    // split right of the '<' character so tapping '<' decrements as expected.
    bool rightSide = (x > W - 80);
    int rowCount = rowCountForPage(settingsPage);

    if (row >= 0 && row < rowCount) {
        // Track whether this tap actually mutated Settings so we only
        // pay the SD+NVS write cost when there is something new to save.
        // Opening a picker overlay, navigating to a submenu, or tapping a
        // display-only row leaves this false.
        bool mutated = false;

        if (settingsPage == PAGE_READING) {
            switch (row) {
                case 0: // Font Size
                    if (rightSide) {
                        s.fontSizeLevel = (s.fontSizeLevel + 1) % FONT_SIZE_LEVEL_COUNT;
                    } else {
                        s.fontSizeLevel = (s.fontSizeLevel + FONT_SIZE_LEVEL_COUNT - 1) % FONT_SIZE_LEVEL_COUNT;
                    }
                    display_set_font(s.fontSizeLevel, s.fontFamily);
                    mutated = true;
                    break;
                case 1: // Font Family — open picker overlay
                    pickerKind   = PICKER_FONT_FAMILY;
                    pickerTarget = -1;
                    (void)rightSide;
                    break;
                case 2: // Line spacing
                    if (rightSide) {
                        s.lineSpacingLevel = (s.lineSpacingLevel + 1) % LINE_SPACING_LEVEL_COUNT;
                    } else {
                        s.lineSpacingLevel = (s.lineSpacingLevel + LINE_SPACING_LEVEL_COUNT - 1) % LINE_SPACING_LEVEL_COUNT;
                    }
                    mutated = true;
                    break;
                case 3: // Font preview — no action
                    break;
                case 4: { // Sleep Timeout
                    int idx = findSleepIdx();
                    if (rightSide) {
                        idx = (idx + 1) % NUM_SLEEP_OPTIONS;
                    } else {
                        idx = (idx + NUM_SLEEP_OPTIONS - 1) % NUM_SLEEP_OPTIONS;
                    }
                    s.sleepTimeoutMin = sleepOptions[idx];
                    mutated = true;
                    break;
                }
                case 5: { // Cleanup Refresh
                    int idx = findRefreshIdx();
                    if (rightSide) {
                        idx = (idx + 1) % NUM_REFRESH_OPTIONS;
                    } else {
                        idx = (idx + NUM_REFRESH_OPTIONS - 1) % NUM_REFRESH_OPTIONS;
                    }
                    s.refreshEveryPages = refreshOptions[idx];
                    mutated = true;
                    break;
                }
                case 6: { // Page Numbers — per-row partial flush pilot.
                    // This is the only settings row currently wired to the
                    // flushSettingsRow() helper (WP-D pilot). Other rows
                    // still rely on the post-touch full repaint via
                    // handleSettingsTouch()'s needsRedraw=true. Migrating
                    // them requires factoring row-drawing out of
                    // ui_settings_draw — deferred.
                    s.showPageNumbers = !s.showPageNumbers;
                    saveSettingsNow();

                    // Reading page is not a submenu, so the row band starts
                    // below the tab strip.
                    // Keep in sync with PAGE_READING drawRow ordering
                    // in ui_settings_draw (Font Size, Font Family,
                    // Line Spacing, Font Preview, Sleep Timeout,
                    // Cleanup Refresh, then Page Numbers = idx 6).
                    const int row_y =
                        HEADER_HEIGHT + SETTINGS_TAB_H + MARGIN_Y + 10
                        + 6 * SETTINGS_ROW_H;
                    display_begin_frame();
                    display_draw_filled_rect(0, row_y, W, SETTINGS_ROW_H, 15);
                    drawRow(row_y, "Page Numbers",
                            s.showPageNumbers ? "[ ON ]" : "[ OFF ]");
                    flushSettingsRow(row_y, SETTINGS_ROW_H,
                                     ChangeKind::StructuralRedraw);

                    // Suppress the post-touch full repaint that
                    // handleSettingsTouch() would otherwise trigger; we
                    // already painted the only thing that changed.
                    setNeedsRedraw(false);
                    return STATE_SETTINGS;
                }
            }
        } else if (settingsPage == PAGE_LIBRARY) {
            switch (row) {
                case 0: // Library View
                    s.libraryViewMode = (s.libraryViewMode + 1) % 2;
                    if (s.libraryViewMode == 1) {
                        s.posterShowCovers = true;
                    }
                    cover_cache_clear();
                    mutated = true;
                    break;
                case 1: // Library Sort
                    if (rightSide) {
                        s.librarySortOrder = (s.librarySortOrder + 1) % 4;
                    } else {
                        s.librarySortOrder = (s.librarySortOrder + 3) % 4;
                    }
                    mutated = true;
                    break;
                case 2: // Poster Covers
                    s.posterShowCovers = !s.posterShowCovers;
                    cover_cache_clear();
                    mutated = true;
                    break;
            }
        } else if (settingsPage == PAGE_DEVICE) {
            switch (row) {
                case 0: // Battery Display
                    s.showBattery = !s.showBattery;
                    mutated = true;
                    break;
                case 1: // Frontlight on/off
                    s.frontlightEnabled = !s.frontlightEnabled;
                    frontlight_apply_from_settings();
                    mutated = true;
                    break;
                case 2: // Brightness — open picker overlay
                    pickerKind   = PICKER_BRIGHTNESS;
                    pickerTarget = -1;
                    break;
                case 3: // Button Setup → submenu
                    settingsPage = PAGE_BUTTONS;
                    return STATE_SETTINGS;
                case 4: // WiFi Manager
                    return STATE_WIFI;
                case 5: // WiFi Setup (on-device scan + keyboard)
                    return STATE_WIFI_SETUP;
                case 6: // WiFi SSID (display only)
                    break;
                case 7: // Firmware Update
                    return STATE_OTA_CHECK;
            }
        } else if (settingsPage == PAGE_BUTTONS) {
            // Action rows open a picker overlay instead of cycling.  Index
            // matches the order returned by buttonPickerSlot().
            static const int kPickerRowToTarget[8] = {
                -1,  // 0: Boot Button enable (toggle)
                 3,  // 1: Boot Tap
                 4,  // 2: Boot Double
                 5,  // 3: Boot Hold
                -1,  // 4: User Button enable (toggle)
                 0,  // 5: IO48 Tap
                 1,  // 6: IO48 Double
                 2,  // 7: IO48 Hold
            };
            (void)rightSide;
            if (row == 0) {
                s.bootButtonEnabled = !s.bootButtonEnabled;
                mutated = true;
            } else if (row == 4) {
                s.userButtonEnabled = !s.userButtonEnabled;
                mutated = true;
            } else {
                int target = kPickerRowToTarget[row];
                if (target >= 0) {
                    pickerKind   = PICKER_BUTTON_ACTION;
                    pickerTarget = target;
                }
            }
        }

        if (mutated) saveSettingsNow();
        return STATE_SETTINGS;
    }

    return STATE_SETTINGS;
}
