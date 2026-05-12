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
        case PAGE_BUTTONS: return 4;
        default:           return 0;
    }
}

// Tab bar directly under header. Active tab inverts to white; band is light gray.
static void drawSettingsTabs() {
    int tabY = HEADER_HEIGHT;
    display_draw_filled_rect(0, tabY, W, SETTINGS_TAB_H, 14);

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

// ═══════════════════════════════════════════════════════════════════
// Settings screen drawing
// ═══════════════════════════════════════════════════════════════════

void ui_settings_draw(bool& settingsFromLibrary) {
    display_set_font_size(2);  // chrome always in Inter
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

        // WiFi Upload
        drawRow(y, "WiFi Upload", "[ Open ]");
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
        // User Button enable
        drawRow(y, "User Button", s.userButtonEnabled ? "[ ON ]" : "[ OFF ]");
        y += SETTINGS_ROW_H;

        // Tap action
        char btnTapLabel[32];
        snprintf(btnTapLabel, sizeof(btnTapLabel), "< %s >",
                 button_action_name(s.userButtonTapAction));
        drawRow(y, "Tap", btnTapLabel);
        y += SETTINGS_ROW_H;

        // Double-tap action
        char btnDblLabel[32];
        snprintf(btnDblLabel, sizeof(btnDblLabel), "< %s >",
                 button_action_name(s.userButtonDoubleAction));
        drawRow(y, "Double Tap", btnDblLabel);
        y += SETTINGS_ROW_H;

        // Long-press action
        char btnLngLabel[32];
        snprintf(btnLngLabel, sizeof(btnLngLabel), "< %s >",
                 button_action_name(s.userButtonLongAction));
        drawRow(y, "Hold", btnLngLabel);
        y += SETTINGS_ROW_H;
    }

    drawBottomBar("[ Back ]");
    // Settings always uses a full medium refresh.  Partial / region updates
    // produced vertical stripe ghosts on this panel because most pixels on
    // the screen are unchanged whitespace and the EPD waveform doesn't fully
    // settle them.  The brief flash on every tap is the lesser evil.
    display_update_medium();
    settingsFromLibrary = false;
}

// ═══════════════════════════════════════════════════════════════════
// Settings touch handling
// ═══════════════════════════════════════════════════════════════════

AppState ui_settings_touch(int x, int y, BookReader& reader) {
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
        if (settingsPage == PAGE_READING) {
            switch (row) {
                case 0: // Font Size
                    if (rightSide) {
                        s.fontSizeLevel = (s.fontSizeLevel + 1) % FONT_SIZE_LEVEL_COUNT;
                    } else {
                        s.fontSizeLevel = (s.fontSizeLevel + FONT_SIZE_LEVEL_COUNT - 1) % FONT_SIZE_LEVEL_COUNT;
                    }
                    display_set_font(s.fontSizeLevel, s.fontFamily);
                    break;
                case 1: // Font Family — cycle Sans → Serif → Slab → Sans …
                    if (rightSide) {
                        s.fontFamily = (uint8_t)((s.fontFamily + 1) % FONT_FAMILY_COUNT);
                    } else {
                        s.fontFamily = (uint8_t)((s.fontFamily + FONT_FAMILY_COUNT - 1) % FONT_FAMILY_COUNT);
                    }
                    display_set_font(s.fontSizeLevel, s.fontFamily);
                    break;
                case 2: // Line spacing
                    if (rightSide) {
                        s.lineSpacingLevel = (s.lineSpacingLevel + 1) % LINE_SPACING_LEVEL_COUNT;
                    } else {
                        s.lineSpacingLevel = (s.lineSpacingLevel + LINE_SPACING_LEVEL_COUNT - 1) % LINE_SPACING_LEVEL_COUNT;
                    }
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
                    break;
                }
                case 6: // Page Numbers
                    s.showPageNumbers = !s.showPageNumbers;
                    break;
            }
        } else if (settingsPage == PAGE_LIBRARY) {
            switch (row) {
                case 0: // Library View
                    s.libraryViewMode = (s.libraryViewMode + 1) % 2;
                    if (s.libraryViewMode == 1) {
                        s.posterShowCovers = true;
                    }
                    cover_cache_clear();
                    break;
                case 1: // Library Sort
                    if (rightSide) {
                        s.librarySortOrder = (s.librarySortOrder + 1) % 4;
                    } else {
                        s.librarySortOrder = (s.librarySortOrder + 3) % 4;
                    }
                    break;
                case 2: // Poster Covers
                    s.posterShowCovers = !s.posterShowCovers;
                    cover_cache_clear();
                    break;
            }
        } else if (settingsPage == PAGE_DEVICE) {
            switch (row) {
                case 0: // Battery Display
                    s.showBattery = !s.showBattery;
                    break;
                case 1: // Frontlight on/off
                    s.frontlightEnabled = !s.frontlightEnabled;
                    frontlight_apply_from_settings();
                    break;
                case 2: { // Brightness
                    int b = s.frontlightBrightness;
                    if (rightSide) {
                        b += 10; if (b > 100) b = 100;
                    } else {
                        b -= 10; if (b < 0) b = 0;
                    }
                    s.frontlightBrightness = (uint8_t)b;
                    frontlight_apply_from_settings();
                    break;
                }
                case 3: // Button Setup → submenu
                    settingsPage = PAGE_BUTTONS;
                    return STATE_SETTINGS;
                case 4: // WiFi Upload
                    return STATE_WIFI;
                case 5: // WiFi Setup (on-device scan + keyboard)
                    return STATE_WIFI_SETUP;
                case 6: // WiFi SSID (display only)
                    break;
                case 7: // Firmware Update
                    return STATE_OTA_CHECK;
            }
        } else if (settingsPage == PAGE_BUTTONS) {
            switch (row) {
                case 0: // User Button enable
                    s.userButtonEnabled = !s.userButtonEnabled;
                    break;
                case 1: { // Tap action
                    int v = s.userButtonTapAction;
                    v = rightSide ? (v + 1) % BTN_ACTION_COUNT
                                  : (v + BTN_ACTION_COUNT - 1) % BTN_ACTION_COUNT;
                    s.userButtonTapAction = (uint8_t)v;
                    break;
                }
                case 2: { // Double-tap action
                    int v = s.userButtonDoubleAction;
                    v = rightSide ? (v + 1) % BTN_ACTION_COUNT
                                  : (v + BTN_ACTION_COUNT - 1) % BTN_ACTION_COUNT;
                    s.userButtonDoubleAction = (uint8_t)v;
                    break;
                }
                case 3: { // Hold action
                    int v = s.userButtonLongAction;
                    v = rightSide ? (v + 1) % BTN_ACTION_COUNT
                                  : (v + BTN_ACTION_COUNT - 1) % BTN_ACTION_COUNT;
                    s.userButtonLongAction = (uint8_t)v;
                    break;
                }
            }
        }
        return STATE_SETTINGS;
    }

    return STATE_SETTINGS;
}
