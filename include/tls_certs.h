#pragma once
// Project-wide pinned trust bundle for HTTPS connections.
// Covers GitHub OTA endpoints (USERTrust ECC, ISRG Root X1, DigiCert Global)
// and Let's Encrypt-issued endpoints such as kosync.eu (ISRG Root X1).
//
// Update this bundle when issuing CAs rotate roots.
// NOT marked PROGMEM: WiFiClientSecure::setCACert() stores the raw pointer
// without copying.
extern const char PAPERLOOM_TRUSTED_ROOTS[];
