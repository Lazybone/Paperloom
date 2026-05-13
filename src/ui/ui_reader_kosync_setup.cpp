// ui_reader_kosync_setup.cpp — On-device KoSync credential entry (WP-5).
//
// Renders an inline three-field form (Server / Username / Password) with a
// compact QWERTY keyboard. Tap a field row to switch the active field; tap
// the keyboard to edit the active field's buffer. Tap [Speichern] to
// validate, MD5-hash the password, persist via settings_save() and return
// to STATE_READER. Tap [Abbrechen] to discard and return.
//
// The plaintext password buffer never leaves this module: it is zeroed
// in-place before persistence and on cancel/save, and it is never logged.
//
// Mirrors the on-screen-keyboard pattern from ui_wifi_setup.cpp /
// ui_keyboard.cpp, but inlines the keyboard so all three fields remain
// visible while editing.

#include "ui_reader_kosync_setup.h"

#include "ui_common.h"
#include "ui_toast.h"
#include "config.h"
#include "../display.h"
#include "../settings.h"

#include <Arduino.h>
#include <mbedtls/md5.h>
#include <string.h>

// ─── Layout constants (portrait 540 x 960) ─────────────────────────
namespace {

constexpr int W = PORTRAIT_W;
constexpr int H = PORTRAIT_H;

// Three field display rows directly below the header.
constexpr int FIELD_TOP   = HEADER_HEIGHT + 10;
constexpr int FIELD_H     = 100;
constexpr int FIELD_GAP   = 4;

// Compact keyboard (3 letter rows + 1 function row), sized smaller than the
// stock ui_keyboard so the field area stays visible above it.
constexpr int KEYS_Y      = FIELD_TOP + 3 * FIELD_H + 16;
constexpr int KEY_W       = 52;
constexpr int KEY_H       = 90;
constexpr int KEY_X       = (W - KEY_W * 10) / 2;       // 10 keys per row, centered
constexpr int FUNC_ROW_Y  = KEYS_Y + KEY_H * 3;
constexpr int FUNC_ROW_H  = KEY_H;

// Function row segments span the full panel width (must sum to W).
constexpr int FN_ROW_X    = 0;
constexpr int FN_MODE_W   = 100;
constexpr int FN_SPACE_W  = 240;
constexpr int FN_BACK_W   = 100;          // backspace
constexpr int FN_SHIFT_W  = W - FN_MODE_W - FN_SPACE_W - FN_BACK_W;
static_assert(FN_MODE_W + FN_SPACE_W + FN_BACK_W + FN_SHIFT_W == W,
              "kosync-setup function row must span exactly W");

// Footer bar (Speichern / Abbrechen) — same chrome as ui_wifi_setup.
constexpr int FOOTER_TOP  = H - FOOTER_HEIGHT;
constexpr int FN_SAVE_W   = W / 2;
constexpr int FN_CANCEL_W = W - FN_SAVE_W;

// MD5 hex digest length (32 lowercase hex chars).
constexpr size_t kMd5HexLen = 32;

// Field input length limits (mirror Settings validation contract in WP-3).
constexpr size_t kUserMaxLen    = 32;
constexpr size_t kPasswordMaxLen = 128;
constexpr size_t kServerMaxLen   = 128;

enum class SetupField { Server, Username, Password };
enum class KbMode { Lower, Upper, Symbols };

// ─── Module state ──────────────────────────────────────────────────
SetupField g_field    = SetupField::Server;
KbMode     g_kbMode   = KbMode::Lower;
String     g_server;     // initially set from settings on enter()
String     g_user;
String     g_password;   // plaintext, in-memory only; never persisted

// ─── Layouts (mirror ui_keyboard.cpp) ──────────────────────────────
const char* LAYOUT_LOWER[3][10] = {
    {"q","w","e","r","t","y","u","i","o","p"},
    {"a","s","d","f","g","h","j","k","l",":"},
    {"z","x","c","v","b","n","m",".","/","-"},
};

const char* LAYOUT_UPPER[3][10] = {
    {"Q","W","E","R","T","Y","U","I","O","P"},
    {"A","S","D","F","G","H","J","K","L",":"},
    {"Z","X","C","V","B","N","M",".","/","-"},
};

const char* LAYOUT_SYMS[3][10] = {
    {"1","2","3","4","5","6","7","8","9","0"},
    {"!","@","#","$","%","&","*","(",")","_"},
    {"+","=","/",":",";","?",",",".","'","\""},
};

const char* (*current_layout())[10] {
    switch (g_kbMode) {
        case KbMode::Upper:   return LAYOUT_UPPER;
        case KbMode::Symbols: return LAYOUT_SYMS;
        case KbMode::Lower:
        default:              return LAYOUT_LOWER;
    }
}

const char* mode_button_label() {
    return (g_kbMode == KbMode::Symbols) ? "abc" : "123";
}

// ─── Field helpers ─────────────────────────────────────────────────

String& active_buffer() {
    switch (g_field) {
        case SetupField::Username: return g_user;
        case SetupField::Password: return g_password;
        case SetupField::Server:
        default:                   return g_server;
    }
}

size_t active_max_len() {
    switch (g_field) {
        case SetupField::Username: return kUserMaxLen;
        case SetupField::Password: return kPasswordMaxLen;
        case SetupField::Server:
        default:                   return kServerMaxLen;
    }
}

int field_y(SetupField f) {
    int idx = static_cast<int>(f);
    return FIELD_TOP + idx * FIELD_H + (idx > 0 ? idx * FIELD_GAP : 0);
}

SetupField field_for_y(int y) {
    for (int i = 0; i < 3; ++i) {
        int top    = FIELD_TOP + i * FIELD_H + i * FIELD_GAP;
        int bottom = top + FIELD_H;
        if (y >= top && y < bottom) return static_cast<SetupField>(i);
    }
    return g_field;  // caller checks for hit separately; this is a safe default
}

const char* field_label(SetupField f) {
    switch (f) {
        case SetupField::Username: return "Benutzername";
        case SetupField::Password: return "Passwort";
        case SetupField::Server:
        default:                   return "Server URL";
    }
}

// Zero a String's underlying buffer in place. We hand the compiler a
// volatile pointer so it can't elide the writes, then read one byte back to
// further discourage dead-store elimination. Finally we drop the String so
// the heap slot is freed.
void zeroize_password() {
    if (g_password.length() > 0) {
        volatile char* vp = (volatile char*)g_password.c_str();
        const size_t n = g_password.length();
        for (size_t i = 0; i < n; ++i) vp[i] = 0;
        (void)vp[0];  // read-back to prevent compiler elision
    }
    g_password = String();
}

// ─── Validation ────────────────────────────────────────────────────

bool is_valid_server(const String& s) {
    // Scheme normalization: any case of "https://" is acceptable, but we
    // require the scheme prefix to be present. The settings layer (WP-3)
    // is responsible for case-normalizing the persisted value; here we
    // simply enforce the byte-prefix after a local lower-casing of the
    // scheme region.
    if (s.length() < 9) return false;  // "https://X" minimum
    String scheme = s.substring(0, 8);
    scheme.toLowerCase();
    if (scheme != "https://") return false;
    // Reject whitespace anywhere — the URL parser is intolerant of it and
    // an accidental trailing space is the most common copy-paste bug.
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
    }
    return true;
}

bool is_valid_username(const String& u) {
    if (u.length() < 1 || u.length() > kUserMaxLen) return false;
    for (size_t i = 0; i < u.length(); ++i) {
        char c = u[i];
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_valid_password(const String& p) {
    return p.length() >= 1 && p.length() <= kPasswordMaxLen;
}

// Compute MD5 over the given plaintext and return 32-char lowercase hex.
// Use mbedtls_md5() one-shot: espressif32@6.4.0's libmbedcrypto.a only
// links this variant; the _starts/_update/_finish wrappers are declared
// but not implemented in the prebuilt binary.
String md5_hex(const String& plaintext) {
    uint8_t digest[16];
    if (mbedtls_md5_ret(reinterpret_cast<const uint8_t*>(plaintext.c_str()),
                        plaintext.length(), digest) != 0) {
        Serial.println("[kosync_setup] mbedtls_md5_ret failed");
        return String();
    }

    char buf[kMd5HexLen + 1];
    for (int i = 0; i < 16; ++i) {
        snprintf(buf + (i * 2), 3, "%02x", digest[i]);
    }
    buf[kMd5HexLen] = '\0';
    return String(buf);
}

// ─── Drawing primitives ────────────────────────────────────────────

void draw_key_box(int x, int y, int w, int h, const char* label, bool emphasize) {
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

// Render a field row showing label + current value. Password is masked.
// The active field gets a slightly thicker border and a light tint.
void draw_field(SetupField f) {
    int y = field_y(f);
    bool active = (g_field == f);

    if (active) {
        display_draw_filled_rect(MARGIN_X + 1, y + 1,
                                 W - MARGIN_X * 2 - 2, FIELD_H - 2, 13);
    }
    display_draw_rect(MARGIN_X, y, W - MARGIN_X * 2, FIELD_H, 0);
    if (active) {
        // Double border for emphasis without animating the layout.
        display_draw_rect(MARGIN_X + 1, y + 1,
                          W - MARGIN_X * 2 - 2, FIELD_H - 2, 0);
    }

    // Label (top of row)
    display_draw_text(MARGIN_X + 12, y + 8 + display_font_height(),
                      field_label(f), 6);

    // Value (bottom of row). Password is rendered as asterisks except the
    // last character, matching the ui_keyboard masking convention.
    String shown;
    if (f == SetupField::Password) {
        const String& pw = g_password;
        if (pw.length() == 0) {
            shown = "";
        } else {
            shown.reserve(pw.length());
            for (size_t i = 0; i + 1 < pw.length(); ++i) shown += '*';
            shown += pw.charAt(pw.length() - 1);
        }
    } else {
        shown = (f == SetupField::Username) ? g_user : g_server;
    }

    // Clip text from the left if it overflows the field width.
    const int maxW = W - MARGIN_X * 2 - 24;
    while (shown.length() > 0 &&
           display_text_width(shown.c_str()) > maxW) {
        shown = shown.substring(1);
    }
    if (shown.length() == 0 && !active) {
        // Subtle placeholder when nothing has been entered yet and the
        // field isn't focused. Active field shows an empty value so the
        // user knows their next keystroke lands there.
        const char* placeholder = (f == SetupField::Server)
                                  ? "https://kosync.eu"
                                  : "(leer)";
        display_draw_text(MARGIN_X + 12,
                          y + FIELD_H - 16,
                          placeholder, 9);
    } else {
        display_draw_text(MARGIN_X + 12,
                          y + FIELD_H - 16,
                          shown.c_str(), 0);
    }
}

// ─── Public API ────────────────────────────────────────────────────

}  // namespace

void ui_kosync_setup_enter() {
    Settings& s = settings_get();
    g_server   = s.kosyncServer;
    if (g_server.length() == 0) g_server = "https://kosync.eu";
    g_user     = s.kosyncUser;
    // NEVER pre-fill password — we only hold the MD5 hash on disk.
    zeroize_password();
    g_field    = SetupField::Server;
    g_kbMode   = KbMode::Lower;
}

void ui_kosync_setup_draw() {
    display_set_font_size(2);
    display_fill_screen(15);
    drawHeader("KoSync Setup", false);

    // Field rows
    draw_field(SetupField::Server);
    draw_field(SetupField::Username);
    draw_field(SetupField::Password);

    // Keyboard grid
    auto layout = current_layout();
    const bool shiftActive   = (g_kbMode == KbMode::Upper);
    const bool symbolsActive = (g_kbMode == KbMode::Symbols);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 10; ++c) {
            const char* label = layout[r][c];
            if (!label || label[0] == '\0') continue;
            int x = KEY_X + c * KEY_W;
            int y = KEYS_Y + r * KEY_H;
            draw_key_box(x, y, KEY_W, KEY_H, label, false);
        }
    }

    // Function row: [Mode] [Space] [Del] [Shift]
    int fx = FN_ROW_X;
    draw_key_box(fx, FUNC_ROW_Y, FN_MODE_W,  FUNC_ROW_H, mode_button_label(), symbolsActive);
    fx += FN_MODE_W;
    draw_key_box(fx, FUNC_ROW_Y, FN_SPACE_W, FUNC_ROW_H, "Space", false);
    fx += FN_SPACE_W;
    draw_key_box(fx, FUNC_ROW_Y, FN_BACK_W,  FUNC_ROW_H, "Del",   false);
    fx += FN_BACK_W;
    {
        const char* lbl = symbolsActive ? "abc" : (shiftActive ? "ABC" : "abc");
        draw_key_box(fx, FUNC_ROW_Y, FN_SHIFT_W, FUNC_ROW_H, lbl, shiftActive);
    }

    // Footer bar (Speichern / Abbrechen) — dark bar to match wifi_setup chrome
    display_draw_filled_rect(0, FOOTER_TOP, W, FOOTER_HEIGHT, 2);
    auto drawSeg = [](int x, int w, const char* label) {
        int tw = display_text_width(label);
        display_draw_text(x + (w - tw) / 2,
                          FOOTER_TOP + FOOTER_HEIGHT - 14,
                          label, 15);
    };
    drawSeg(0,          FN_SAVE_W,   "Speichern");
    display_draw_filled_rect(FN_SAVE_W - 1, FOOTER_TOP + 4, 2, FOOTER_HEIGHT - 8, 10);
    drawSeg(FN_SAVE_W,  FN_CANCEL_W, "Abbrechen");

    display_begin_frame();
    display_mark_dirty(Zone::FullScreen, ChangeKind::StructuralRedraw);
    display_flush();
}

// ─── Touch handling ────────────────────────────────────────────────

namespace {

void append_char(const char* key) {
    String& buf = active_buffer();
    if (buf.length() >= active_max_len()) return;
    buf += key;
}

void handle_backspace() {
    String& buf = active_buffer();
    if (buf.length() > 0) buf.remove(buf.length() - 1);
}

}  // namespace

AppState ui_kosync_setup_touch(int x, int y) {
    // Footer (Speichern / Abbrechen) — checked first because the bar sits
    // below everything else and has unambiguous y-coordinates.
    if (y >= FOOTER_TOP) {
        if (x < FN_SAVE_W) {
            // Validate all three fields.
            if (!is_valid_server(g_server)) {
                ui_toast_show("Ungültige Eingabe", 2500, true);
                return STATE_KOSYNC_SETUP;
            }
            if (!is_valid_username(g_user)) {
                ui_toast_show("Ungültige Eingabe", 2500, true);
                return STATE_KOSYNC_SETUP;
            }
            if (!is_valid_password(g_password)) {
                ui_toast_show("Passwort fehlt", 2500, true);
                return STATE_KOSYNC_SETUP;
            }

            // Length is fine to log; content is NOT.
            Serial.printf("[kosync_setup] saving creds (pw len=%u)\n",
                          (unsigned)g_password.length());

            const String hex = md5_hex(g_password);
            // Zero the plaintext BEFORE persisting so any failure path
            // beyond this point cannot leak the password.
            zeroize_password();

            Settings& s = settings_get();
            s.kosyncServer = g_server;
            s.kosyncUser   = g_user;
            s.kosyncKey    = hex;
            // s.kosyncDeviceName is left untouched (managed elsewhere).
            // Runtime-only invalid flag clears once we persist fresh creds.
            s.kosyncCredentialsInvalid = false;

            if (!settings_save()) {
                Serial.println("[kosync_setup] settings_save failed (SD?)");
                ui_toast_show("Speichern fehlgeschlagen", 2500, true);
                // Stay on the screen so the user can retry; in-memory
                // creds are already set so a sync attempt would still
                // work this session, but credentials would not survive
                // a reboot.
                return STATE_KOSYNC_SETUP;
            }

            ui_toast_show("Settings saved", 2000, false);
            return STATE_READER;
        }
        // Abbrechen
        zeroize_password();
        return STATE_READER;
    }

    // Field-row hit-test (only within the field region).
    if (y >= FIELD_TOP && y < FIELD_TOP + 3 * FIELD_H + 2 * FIELD_GAP &&
        x >= MARGIN_X && x < W - MARGIN_X) {
        g_field = field_for_y(y);
        return STATE_KOSYNC_SETUP;
    }

    // Function row (above footer, below the letter grid).
    if (y >= FUNC_ROW_Y && y < FUNC_ROW_Y + FUNC_ROW_H) {
        int fx = FN_ROW_X;
        if (x >= fx && x < fx + FN_MODE_W) {
            g_kbMode = (g_kbMode == KbMode::Symbols) ? KbMode::Lower
                                                     : KbMode::Symbols;
            return STATE_KOSYNC_SETUP;
        }
        fx += FN_MODE_W;
        if (x >= fx && x < fx + FN_SPACE_W) {
            // Spaces are not legal in any of our three fields (server URL
            // rejects whitespace, username charset excludes it, and a
            // password with a leading/trailing space is almost always a
            // typo). Suppress space entry rather than letting a value
            // through that the save-validator will then reject.
            return STATE_KOSYNC_SETUP;
        }
        fx += FN_SPACE_W;
        if (x >= fx && x < fx + FN_BACK_W) {
            handle_backspace();
            return STATE_KOSYNC_SETUP;
        }
        fx += FN_BACK_W;
        if (x >= fx && x < fx + FN_SHIFT_W) {
            if (g_kbMode == KbMode::Symbols) {
                g_kbMode = KbMode::Lower;
            } else {
                g_kbMode = (g_kbMode == KbMode::Lower) ? KbMode::Upper
                                                       : KbMode::Lower;
            }
            return STATE_KOSYNC_SETUP;
        }
        return STATE_KOSYNC_SETUP;
    }

    // Key grid hit-test
    if (y >= KEYS_Y && y < KEYS_Y + KEY_H * 3 &&
        x >= KEY_X && x < KEY_X + KEY_W * 10) {
        int row = (y - KEYS_Y) / KEY_H;
        int col = (x - KEY_X) / KEY_W;
        if (row >= 0 && row < 3 && col >= 0 && col < 10) {
            auto layout = current_layout();
            const char* key = layout[row][col];
            if (key && key[0] != '\0') append_char(key);
        }
    }
    return STATE_KOSYNC_SETUP;
}
