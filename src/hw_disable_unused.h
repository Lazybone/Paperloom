#pragma once

// Hält LoRa SX1262 und GPS auf dem T5S3 Pro in einem sicheren,
// nicht-aktiven Zustand. Auf anderen Targets ist die Funktion ein No-Op.
void hw_disable_unused_init();
