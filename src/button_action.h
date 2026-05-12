#pragma once

#include <stdint.h>

// Configurable actions for the user-facing buttons on T5S3 PRO
// (BOOT button on GPIO 0 and user button on PCA9535 IO12). Each gesture
// (tap / double / long) maps independently to one of these actions via
// settings. Persisted as uint8_t in settings.json; keep stable across
// firmware versions — append new values, never reorder.
enum ButtonAction : uint8_t {
    BTN_ACTION_NONE             = 0,
    BTN_ACTION_BACKLIGHT_TOGGLE = 1,  // Toggle frontlight on/off
    BTN_ACTION_LIBRARY          = 2,  // Return to library from reader
    BTN_ACTION_SLEEP            = 3,  // Enter deep sleep
    BTN_ACTION_NEXT_PAGE        = 4,  // Reader: next page
    BTN_ACTION_PREV_PAGE        = 5,  // Reader: previous page
    BTN_ACTION_MENU             = 6,  // Reader: open menu overlay

    BTN_ACTION_COUNT                  // sentinel; not a real action
};

// Human-readable label for a given action (for settings UI).
const char* button_action_name(uint8_t action);

// Execute the action. No-op for BTN_ACTION_NONE or unknown values.
// Safe to call from main loop context.
void button_action_execute(uint8_t action);
