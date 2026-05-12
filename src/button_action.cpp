#include "button_action.h"

const char* button_action_name(uint8_t action) {
    switch (action) {
        case BTN_ACTION_NONE:             return "None";
        case BTN_ACTION_BACKLIGHT_TOGGLE: return "Light toggle";
        case BTN_ACTION_LIBRARY:          return "Library";
        case BTN_ACTION_SLEEP:            return "Sleep";
        case BTN_ACTION_NEXT_PAGE:        return "Next page";
        case BTN_ACTION_PREV_PAGE:        return "Prev page";
        case BTN_ACTION_MENU:             return "Menu";
        default:                          return "?";
    }
}

// button_action_execute is implemented in main.cpp so it can dispatch into
// the file-local static functions (page nav, sleep, state changes).
