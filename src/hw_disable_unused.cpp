#include "hw_disable_unused.h"
#include <Arduino.h>
#include <driver/gpio.h>
#include "config.h"

void hw_disable_unused_init() {
    // Release any deep-sleep hold left over from a previous firmware build
    // that activated gpio_deep_sleep_hold_en(). Otherwise pins remain latched
    // through soft-reset and Wire.begin / display_init hang silently.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)LORA_CS);
    gpio_hold_dis((gpio_num_t)LORA_RST);

    // ─── SX1262 deselektieren und im Reset halten ─────────────────
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);          // SPI-Slave nicht aktiv

    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);          // im Reset → tiefster Strom

    pinMode(LORA_BUSY, INPUT);            // floatet OK – Pull-down auf Board
    pinMode(LORA_IRQ, INPUT);

    Serial.println("[HW] SX1262 LoRa deaktiviert (CS HIGH, RST LOW)");

    // ─── L76K GPS ────────────────────────────────────────────────
    // GPS_RX=44 / GPS_TX=43 sind die IO_MUX-Default-Pins für UART0 und
    // damit der ESP-IDF-Panic-/Logging-Konsole. Pin-Modi anfassen oder
    // ein zweites UART darauf binden killt die Console-IO und führt zu
    // RTC-SW-CPU-Reset, sobald die nächste log-Zeile geflusht wird.
    //
    // Konsequenz: GPS lässt sich von Software aus NICHT deaktivieren,
    // solange UART0 die Konsole bedient. Wenn die Hardware-Variante
    // ein bestücktes + gepowertes GPS-Modul mitbringt, braucht es
    // entweder eine dedizierte Power-Rail-GPIO (board-redesign) oder
    // den Wechsel der Konsole weg von UART0 (sdkconfig +
    // CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG). Bis dahin: keine pinMode-
    // Calls auf 43/44, GPS-Verbrauch akzeptieren falls bestückt.
}
