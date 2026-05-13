#pragma once
#include <WebServer.h>

// Register kosync-related HTTP endpoints on the given WebServer instance.
//
// Endpoints registered:
//   GET  /api/kosync-settings  → { ok, server, user, deviceName }  (never key)
//   POST /api/kosync-settings  → consumes { server, user, password, deviceName? },
//                                hashes password server-side (MD5 hex, 32 lc),
//                                writes settings, returns { ok } or { ok:false, error }.
//
// WP-6a leaves a clearly-marked seam at the top of the POST handler where
// WP-6c will plug in the PIN gate (lockout + 401/429 responses + PIN consume).
//
// Call this from wifi_upload's server-registration point AFTER the existing
// _server.on(...) bindings.
void kosync_http_register_handlers(WebServer& server);
