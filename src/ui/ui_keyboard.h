#pragma once

#include <Arduino.h>
#include "state.h"

// On-screen QWERTY keyboard for touch input.
//
// Caller flow:
//   1. Set callbacks via ui_keyboard_open() before transitioning to STATE_WIFI_KEYBOARD.
//   2. main.cpp routes draw/touch to ui_keyboard_draw()/ui_keyboard_touch().
//   3. On Done/Cancel the appropriate callback fires; the callback's return
//      value becomes the next AppState. To chain another keyboard step,
//      simply call ui_keyboard_open() inside the cb and return STATE_WIFI_KEYBOARD.
//
// The keyboard owns its own buffer; the caller hands over an initial value.

using KeyboardDoneCb   = AppState (*)(const String& text);
using KeyboardCancelCb = AppState (*)();

void ui_keyboard_open(const char* title,
                      const String& initial,
                      bool isPassword,
                      KeyboardDoneCb onDone,
                      KeyboardCancelCb onCancel);

void     ui_keyboard_draw();
AppState ui_keyboard_touch(int x, int y);
