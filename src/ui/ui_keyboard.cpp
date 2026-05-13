#include "ui_keyboard.h"
#include "config.h"
#include "../display.h"

#include "ui_common.h"

// ─── Layout constants (portrait 540 x 960) ─────────────────────────
static const int W = PORTRAIT_W;
static const int H = PORTRAIT_H;

static const int TITLE_Y       = HEADER_HEIGHT + 12;     // header label area
static const int INPUT_Y       = HEADER_HEIGHT + 70;     // input rect top
static const int INPUT_H       = 80;
static const int KEYS_Y        = HEADER_HEIGHT + 200;    // first key row top
static const int KEY_W         = 52;
static const int KEY_H         = 110;
static const int KEY_X         = (W - KEY_W * 10) / 2;   // 10 keys per row, centered
static const int FUNC_ROW_Y    = KEYS_Y + KEY_H * 3;     // function row
static const int FUNC_ROW_H    = KEY_H;
// Function row spans the full panel width so the right-most "Done" key isn't
// clipped off the panel — the 4 segments below MUST sum to W.
static const int FN_ROW_X      = 0;
static const int FN_MODE_W     = 100;
static const int FN_SPACE_W    = 240;
static const int FN_CANCEL_W   = 100;
static const int FN_DONE_W     = W - FN_MODE_W - FN_SPACE_W - FN_CANCEL_W;
static_assert(FN_MODE_W + FN_SPACE_W + FN_CANCEL_W + FN_DONE_W == W,
              "Keyboard function row must span exactly W");

// ─── Module state ──────────────────────────────────────────────────
enum class KbMode { Lower, Upper, Symbols };

static String          _title;
static String          _buffer;
static bool            _isPassword = false;
static KbMode          _mode       = KbMode::Lower;
static KeyboardDoneCb  _onDone     = nullptr;
static KeyboardCancelCb _onCancel  = nullptr;

// ─── Layouts ───────────────────────────────────────────────────────
// 3 rows x 10 columns. Empty string = blank slot, "BS" = backspace,
// "SH" = shift / mode toggle.

static const char* LAYOUT_LOWER[3][10] = {
    {"q","w","e","r","t","y","u","i","o","p"},
    {"a","s","d","f","g","h","j","k","l","BS"},
    {"SH","z","x","c","v","b","n","m",".","-"},
};

static const char* LAYOUT_UPPER[3][10] = {
    {"Q","W","E","R","T","Y","U","I","O","P"},
    {"A","S","D","F","G","H","J","K","L","BS"},
    {"SH","Z","X","C","V","B","N","M",".","-"},
};

static const char* LAYOUT_SYMS[3][10] = {
    {"1","2","3","4","5","6","7","8","9","0"},
    {"!","@","#","$","%","&","*","(",")","BS"},
    {"SH","_","+","=","/",":",";","?",",","."},
};

static const char* (*current_layout())[10] {
    switch (_mode) {
        case KbMode::Upper:   return LAYOUT_UPPER;
        case KbMode::Symbols: return LAYOUT_SYMS;
        case KbMode::Lower:
        default:              return LAYOUT_LOWER;
    }
}

static const char* mode_button_label() {
    // Label reflects what the button toggles TO, not the current mode.
    switch (_mode) {
        case KbMode::Symbols: return "abc";   // back to lowercase letters
        case KbMode::Upper:   return "123";   // jump to symbols
        case KbMode::Lower:
        default:              return "123";   // jump to symbols
    }
}

// ─── Public API ────────────────────────────────────────────────────

void ui_keyboard_open(const char* title,
                      const String& initial,
                      bool isPassword,
                      KeyboardDoneCb onDone,
                      KeyboardCancelCb onCancel) {
    _title      = title ? String(title) : String("Enter text");
    _buffer     = initial;
    _isPassword = isPassword;
    _mode       = KbMode::Lower;
    _onDone     = onDone;
    _onCancel   = onCancel;
}

// Render input value (masked or raw) clipped to fit the input box.
// Password mode: mask all but the last character so the user can verify
// each keystroke without exposing the full secret.
static String displayed_input() {
    if (!_isPassword || _buffer.length() == 0) return _buffer;
    String masked;
    masked.reserve(_buffer.length());
    for (size_t i = 0; i + 1 < _buffer.length(); i++) masked += '*';
    masked += _buffer.charAt(_buffer.length() - 1);
    return masked;
}

static void draw_key_box(int x, int y, int w, int h, const char* label, bool emphasize) {
    if (emphasize) {
        display_draw_filled_rect(x + 1, y + 1, w - 2, h - 2, 12);
    }
    display_draw_rect(x, y, w, h, 0);
    if (!label || label[0] == '\0') return;
    int tw = display_text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h + display_font_height()) / 2 - 6;
    display_draw_text(tx, ty, label, 0);
}

void ui_keyboard_draw() {
    display_set_font_size(2);  // chrome always in Inter
    display_fill_screen(15);
    drawHeader("Keyboard", false);

    // Title line beneath header
    display_draw_text(MARGIN_X, TITLE_Y + display_font_height(), _title.c_str(), 0);

    // Input box
    display_draw_rect(MARGIN_X, INPUT_Y, W - MARGIN_X * 2, INPUT_H, 0);
    String shown = displayed_input();
    // Clip text to box width: drop chars from the front while too wide.
    int maxW = W - MARGIN_X * 2 - 24;
    while (shown.length() > 0 && display_text_width(shown.c_str()) > maxW) {
        shown = shown.substring(1);
    }
    display_draw_text(MARGIN_X + 12,
                      INPUT_Y + (INPUT_H + display_font_height()) / 2 - 6,
                      shown.c_str(), 0);

    // Key grid (3 rows of 10)
    auto layout = current_layout();
    bool shiftActive = (_mode == KbMode::Upper);
    bool symbolsActive = (_mode == KbMode::Symbols);

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 10; c++) {
            const char* label = layout[r][c];
            if (label == nullptr || label[0] == '\0') continue;
            int x = KEY_X + c * KEY_W;
            int y = KEYS_Y + r * KEY_H;

            const char* drawn = label;
            bool emphasize = false;
            if (strcmp(label, "BS") == 0) { drawn = "Del"; }
            else if (strcmp(label, "SH") == 0) {
                // Label reflects current mode so the user can read it at a
                // glance without inspecting which keys are visible.
                drawn = symbolsActive ? "abc" : (shiftActive ? "ABC" : "abc");
                emphasize = shiftActive || symbolsActive;
            }
            draw_key_box(x, y, KEY_W, KEY_H, drawn, emphasize);
        }
    }

    // Function row: [Mode] [Space] [Cancel] [Done] — spans full panel width
    int fx = FN_ROW_X;
    draw_key_box(fx, FUNC_ROW_Y, FN_MODE_W, FUNC_ROW_H, mode_button_label(), false);
    fx += FN_MODE_W;
    draw_key_box(fx, FUNC_ROW_Y, FN_SPACE_W, FUNC_ROW_H, "Space", false);
    fx += FN_SPACE_W;
    draw_key_box(fx, FUNC_ROW_Y, FN_CANCEL_W, FUNC_ROW_H, "Cancel", false);
    fx += FN_CANCEL_W;
    draw_key_box(fx, FUNC_ROW_Y, FN_DONE_W, FUNC_ROW_H, "Done", false);

    display_begin_frame();
    display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
    display_flush();
}

// Handle a key character coming from the layout.
static void handle_char_key(const char* key) {
    if (strcmp(key, "BS") == 0) {
        if (_buffer.length() > 0) _buffer.remove(_buffer.length() - 1);
        return;
    }
    if (strcmp(key, "SH") == 0) {
        if (_mode == KbMode::Symbols) {
            _mode = KbMode::Lower;
        } else {
            _mode = (_mode == KbMode::Lower) ? KbMode::Upper : KbMode::Lower;
        }
        return;
    }
    // Append printable single character (layout entries are length 1 here)
    _buffer += key;
}

AppState ui_keyboard_touch(int x, int y) {
    // Function row first (occupies full bottom strip of keyboard area)
    int funcBottom = FUNC_ROW_Y + FUNC_ROW_H;
    if (y >= FUNC_ROW_Y && y < funcBottom) {
        int fx = FN_ROW_X;
        if (x >= fx && x < fx + FN_MODE_W) {
            // Cycle: Lower -> Symbols -> Lower (Upper reachable via SH)
            _mode = (_mode == KbMode::Symbols) ? KbMode::Lower : KbMode::Symbols;
            return STATE_WIFI_KEYBOARD;
        }
        fx += FN_MODE_W;
        if (x >= fx && x < fx + FN_SPACE_W) {
            _buffer += ' ';
            return STATE_WIFI_KEYBOARD;
        }
        fx += FN_SPACE_W;
        if (x >= fx && x < fx + FN_CANCEL_W) {
            KeyboardCancelCb cb = _onCancel;
            _onCancel = nullptr;
            _onDone = nullptr;
            return cb ? cb() : STATE_WIFI_SETUP;
        }
        fx += FN_CANCEL_W;
        if (x >= fx && x < fx + FN_DONE_W) {
            KeyboardDoneCb cb = _onDone;
            String value = _buffer;
            _onDone = nullptr;
            _onCancel = nullptr;
            return cb ? cb(value) : STATE_WIFI_SETUP;
        }
        return STATE_WIFI_KEYBOARD;
    }

    // Key grid hit-testing
    if (y >= KEYS_Y && y < KEYS_Y + KEY_H * 3 &&
        x >= KEY_X && x < KEY_X + KEY_W * 10) {
        int row = (y - KEYS_Y) / KEY_H;
        int col = (x - KEY_X) / KEY_W;
        if (row >= 0 && row < 3 && col >= 0 && col < 10) {
            auto layout = current_layout();
            const char* key = layout[row][col];
            if (key && key[0] != '\0') handle_char_key(key);
        }
    }
    return STATE_WIFI_KEYBOARD;
}
