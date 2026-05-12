#include "hw_disable_unused.h"
#include <Arduino.h>
#include "config.h"

void hw_disable_unused_init() {
    // ─── SX1262 deselektieren und im Reset halten ─────────────────
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);          // SPI-Slave nicht aktiv

    pinMode(LORA_RST, OUTPUT);
    digitalWrite(LORA_RST, LOW);          // im Reset → tiefster Strom

    pinMode(LORA_BUSY, INPUT);            // floatet OK – Pull-down auf Board
    pinMode(LORA_IRQ, INPUT);

    Serial.println("[HW] SX1262 LoRa deaktiviert (CS HIGH, RST LOW)");

    // GPS_RX (44) and GPS_TX (43) are UART0 RX/TX on ESP32-S3 — used by the
    // on-board USB-serial chip for Serial. Forcing them to INPUT breaks Serial
    // logging and can panic the chip when Serial is the active console
    // (e.g. ARDUINO_USB_CDC_ON_BOOT=0). Leave them alone; GPS module is
    // already unpowered on the Pro board so the UART is electrically idle.
}
