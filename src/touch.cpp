#include "touch.h"
#include "config.h"
#include <Wire.h>
#include <TouchDrvGT911.hpp>

// Pro touch via Arduino Wire. Wire is initialized in main.cpp::setup()
// BEFORE display_init(), so Wire owns the I²C-0 driver and epdiy (patched
// by tools/patch_epdiy.py) reuses it instead of trying to install its own.
//
// We pass -1/-1 to setPins to tell SensorLib NOT to drive RST/INT GPIOs —
// the GT911 already responds at 0x5D after power-up (confirmed by earlier
// scans), and previously driving the documented INT=GPIO 3 / RST=GPIO 9
// pins caused the next epdiy display update to crash, suggesting one of
// those GPIOs is wired to something epdiy needs on this board revision.

static TouchDrvGT911 _touch;
static bool _initialized = false;

bool touch_init() {
    _touch.setPins(-1, -1);

    // Probe both possible GT911 addresses on the Wire bus we already share.
    Wire.beginTransmission(0x14);
    bool found_14 = (Wire.endTransmission() == 0);
    Wire.beginTransmission(0x5D);
    bool found_5d = (Wire.endTransmission() == 0);

    uint8_t addr = found_5d ? 0x5D : (found_14 ? 0x14 : 0);
    if (addr == 0) {
        Serial.println("GT911 NOT found on shared I²C-0 bus");
        return false;
    }

    if (!_touch.begin(Wire, addr, TOUCH_SDA, TOUCH_SCL)) {
        Serial.printf("GT911 begin() failed at 0x%02X\n", addr);
        return false;
    }

    _touch.setMaxCoordinates(PHYS_WIDTH, PHYS_HEIGHT);
    _touch.setSwapXY(true);
    _touch.setMirrorXY(false, true);

    Serial.printf("GT911 initialized at 0x%02X (Pro, shared Wire on I²C-0)\n", addr);
    _initialized = true;
    return true;
}

void touch_sleep() {
    if (!_initialized) return;
    // GT911 command 0x05 = enter low-power sleep. Chip stops scanning the
    // capacitive matrix; idle current drops from ~1-2 mA to <100 µA.
    // Recovery requires hardware reset, which happens automatically on the
    // next boot via touch_init() probing the bus.
    _touch.sleep();
}

bool touch_read(TouchPoint &pt) {
    if (!_initialized) return false;

    // Do NOT call _touch.isPressed() first on the Pro. With setPins(-1, -1)
    // SensorLib's isPressed() falls through to its no-arg getPoint() (see
    // TouchDrvGT911.hpp:319-325) which reads the status register AND clears
    // the GT911's data-ready buffer. The subsequent getPoint(x, y, 1) call
    // then finds an empty buffer and reports zero touches — every tap is
    // silently swallowed. Reading the point array directly returns count
    // and coords in one transaction and only clears the buffer once.
    int16_t x[1] = {0};
    int16_t y[1] = {0};
    uint8_t count = _touch.getPoint(x, y, 1);
    if (count == 0) return false;

    pt.x = (PORTRAIT_W - 1) - y[0];
    pt.y = x[0];
    return true;
}
