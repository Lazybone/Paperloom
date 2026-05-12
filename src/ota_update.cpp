#include "ota_update.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "esp_ota_ops.h"
#include "esp_wifi.h"

// ─── Internal state ────────────────────────────────────────────────
static String _downloadUrl;
static size_t _downloadSize = 0;

// ─── Allow-list & secure-client helpers ────────────────────────────

// GitHub release downloads land on either github.com (the redirect entry)
// or *.githubusercontent.com (the actual S3-backed object). Reject any
// other host so a compromised API response can't redirect us elsewhere.
static bool isAllowedDownloadUrl(const String& url) {
    return url.startsWith("https://github.com/") ||
           url.startsWith("https://objects.githubusercontent.com/") ||
           url.startsWith("https://github-releases.githubusercontent.com/");
}

// GitHub TLS trust anchors. As of 2026-05 GitHub serves at least two
// different chains depending on the host:
//
//   api.github.com                    -> Sectigo / USERTrust ECC
//   objects.githubusercontent.com     -> Let's Encrypt / ISRG Root X1
//
// Plus DigiCert Global Root CA, the historical anchor, retained as a
// fallback in case GitHub rotates back. mbedTLS' x509_crt_parse() accepts
// multiple concatenated PEM blocks as one trust store, so setCACert() on
// this bundle covers all three.
//
// When GitHub rotates anchors again, append the new root here and ship
// a release. Rolling individual anchors out of the bundle is fine after
// a transition window.
//
// NOT marked PROGMEM: WiFiClientSecure::setCACert() stores the raw pointer
// without copying, and we want unambiguous RAM/flash addressability across
// future Arduino-ESP32 changes (some accessors do `strlen()` on the input
// which behaves correctly only on a directly-readable address).
static const char GITHUB_TRUSTED_ROOTS[] = R"CERT(
-----BEGIN CERTIFICATE-----
MIICjzCCAhWgAwIBAgIQXIuZxVqUxdJxVt7NiYDMJjAKBggqhkjOPQQDAzCBiDEL
MAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNl
eSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMT
JVVTRVJUcnVzdCBFQ0MgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMTAwMjAx
MDAwMDAwWhcNMzgwMTE4MjM1OTU5WjCBiDELMAkGA1UEBhMCVVMxEzARBgNVBAgT
Ck5ldyBKZXJzZXkxFDASBgNVBAcTC0plcnNleSBDaXR5MR4wHAYDVQQKExVUaGUg
VVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNVBAMTJVVTRVJUcnVzdCBFQ0MgQ2VydGlm
aWNhdGlvbiBBdXRob3JpdHkwdjAQBgcqhkjOPQIBBgUrgQQAIgNiAAQarFRaqflo
I+d61SRvU8Za2EurxtW20eZzca7dnNYMYf3boIkDuAUU7FfO7l0/4iGzzvfUinng
o4N+LZfQYcTxmdwlkWOrfzCjtHDix6EznPO/LlxTsV+zfTJ/ijTjeXmjQjBAMB0G
A1UdDgQWBBQ64QmG1M8ZwpZ2dEl23OA1xmNjmjAOBgNVHQ8BAf8EBAMCAQYwDwYD
VR0TAQH/BAUwAwEB/zAKBggqhkjOPQQDAwNoADBlAjA2Z6EWCNzklwBBHU6+4WMB
zzuqQhFkoJ2UOQIReVx7Hfpkue4WQrO/isIJxOzksU0CMQDpKmFHjFJKS04YcPbW
RNZu9YO6bVi9JNlWSOrvxKJGgYhqOkbRqZtNyWHa0V1Xahg=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB
CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97
nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt
43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P
T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMXwhuNXheaQB/IjLoyEX1S7m
LoR5l8bMWp0+qgSm+E3Mc78Cb83VSZv/dfBZGJcsy/PVmPiJkAHQIDAQABo0Iw
QDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUA95Q
NVbRTLtm8KPiGxvDl7I90VUwDQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1E
nE9SsPTfrgT1eXkIoyQY/EsrhMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyI
mZOMkXDiqw8cvpOp/2PV5Adg06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIt
tep3Sp+dWOIrWcBAI+0tKIJFPnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na
4UU+Krk2U886UAb3LujEV0lsYSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9k
uXclVzDAGySj4dzp30d8tbQkCAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp
1ZWVbd4=
-----END CERTIFICATE-----
)CERT";

static void configureSecureClient(WiFiClientSecure& c) {
    // Pin GitHub's known root CAs. The Arduino-ESP32 default build does
    // NOT embed a Mozilla bundle, so without this we'd fall back to
    // setInsecure() — MITM-vulnerable. The bundle covers api.github.com
    // (USERTrust ECC), the release CDN (ISRG Root X1), and DigiCert as
    // a historical fallback. Update GITHUB_TRUSTED_ROOTS above when
    // GitHub rotates anchors.
    c.setCACert(GITHUB_TRUSTED_ROOTS);
    c.setHandshakeTimeout(15);  // seconds
}

// ─── Version comparison ────────────────────────────────────────────

static bool parseVersion(const char* s, int& major, int& minor, int& patch) {
    // Accept optional leading 'v'
    if (s[0] == 'v' || s[0] == 'V') s++;
    return sscanf(s, "%d.%d.%d", &major, &minor, &patch) == 3;
}

bool ota_is_update_newer(const String& latestTag) {
    int cMaj = 0, cMin = 0, cPat = 0;
    int nMaj = 0, nMin = 0, nPat = 0;

    if (!parseVersion(FIRMWARE_VERSION, cMaj, cMin, cPat)) return false;
    if (!parseVersion(latestTag.c_str(), nMaj, nMin, nPat)) return false;

    if (nMaj != cMaj) return nMaj > cMaj;
    if (nMin != cMin) return nMin > cMin;
    return nPat > cPat;
}

// ─── GitHub release check ──────────────────────────────────────────

bool ota_check_for_update(String& latestVersion) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("OTA: WiFi not connected");
        return false;
    }

    WiFiClientSecure secure;
    configureSecureClient(secure);

    HTTPClient http;
    http.setUserAgent(String("Paperloom-ESP32-") + FIRMWARE_VERSION);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    if (!http.begin(secure, "https://api.github.com/repos/Lazybone/Paperloom/releases/latest")) {
        Serial.println("OTA: HTTP begin failed (TLS/handshake?)");
        return false;
    }

    int code = http.GET();
    if (code != 200) {
        Serial.printf("OTA: GitHub API returned %d\n", code);
        http.end();
        return false;
    }

    // Cap the response to bound heap pressure — read manually rather than
    // calling getString(), because Content-Length may be -1 (chunked /
    // unknown) and getString() would happily consume an unbounded body.
    static const size_t MAX_API_RESPONSE = 65536;
    int hinted = http.getSize();
    if (hinted > 0 && (size_t)hinted > MAX_API_RESPONSE) {
        Serial.printf("OTA: response too large (Content-Length=%d), aborting\n", hinted);
        http.end();
        return false;
    }

    String payload;
    // Reserve up to the full cap to avoid up to ~4 reallocations as the
    // chunked body grows toward 64 KB.
    payload.reserve(hinted > 0 ? (size_t)hinted : MAX_API_RESPONSE);
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        Serial.println("OTA: no stream from API request");
        http.end();
        return false;
    }
    {
        const size_t bufSize = 1024;
        char buf[bufSize + 1];   // +1 for trailing NUL so we can concat as C-string
        size_t total = 0;
        unsigned long lastData = millis();
        while (stream->connected() || stream->available()) {
            size_t avail = stream->available();
            if (avail == 0) {
                if (millis() - lastData > 10000) break;  // 10 s no-data timeout
                delay(5);
                continue;
            }
            size_t toRead = avail < bufSize ? avail : bufSize;
            int got = stream->readBytes((uint8_t*)buf, toRead);
            if (got <= 0) break;
            lastData = millis();
            if (total + got > MAX_API_RESPONSE) {
                Serial.println("OTA: response exceeded cap mid-stream, aborting");
                http.end();
                return false;
            }
            buf[got] = '\0';
            payload.concat(buf);
            total += got;
        }
    }
    http.end();

    // Parse JSON
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("OTA: JSON parse error: %s\n", err.c_str());
        return false;
    }

    latestVersion = doc["tag_name"].as<String>();
    if (latestVersion.length() == 0) {
        Serial.println("OTA: no tag_name in response");
        return false;
    }

    // Find firmware.bin in assets
    _downloadUrl = "";
    _downloadSize = 0;
    JsonArray assets = doc["assets"].as<JsonArray>();
    for (JsonObject asset : assets) {
        String name = asset["name"].as<String>();
        if (name == "firmware.bin") {
            _downloadUrl = asset["browser_download_url"].as<String>();
            _downloadSize = asset["size"] | 0;
            break;
        }
    }

    if (_downloadUrl.length() == 0) {
        Serial.println("OTA: firmware.bin not found in assets");
        return false;
    }

    // Reject any download URL that doesn't point at GitHub's release CDN.
    // Without this a tampered API response (or compromised account) could
    // hand us an attacker-controlled firmware host.
    if (!isAllowedDownloadUrl(_downloadUrl)) {
        Serial.printf("OTA: rejected download host: %s\n", _downloadUrl.c_str());
        _downloadUrl = "";
        _downloadSize = 0;
        return false;
    }

    Serial.printf("OTA: latest=%s, current=%s, url=%s, size=%u\n",
                  latestVersion.c_str(), FIRMWARE_VERSION,
                  _downloadUrl.c_str(), _downloadSize);

    return ota_is_update_newer(latestVersion);
}

// ─── OTA install via esp_https_ota ─────────────────────────────────

bool ota_install_update(std::function<void(int)> progressCallback) {
    if (_downloadUrl.length() == 0) {
        Serial.println("OTA: no download URL (call checkForUpdate first)");
        return false;
    }
    // Re-validate the URL — paranoia check in case _downloadUrl was set by
    // some future call path that bypassed ota_check_for_update.
    if (!isAllowedDownloadUrl(_downloadUrl)) {
        Serial.printf("OTA: refusing untrusted download host: %s\n", _downloadUrl.c_str());
        return false;
    }

    Serial.printf("OTA: downloading from %s\n", _downloadUrl.c_str());

    // Disable WiFi power saving for reliable download
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Resolve and stream the firmware in one HTTP transaction. STRICT redirect
    // following resolves GitHub's 302 → objects.githubusercontent.com hop
    // automatically; we don't need a separate HEAD probe.
    WiFiClientSecure secure;
    configureSecureClient(secure);

    HTTPClient resolveHttp;
    resolveHttp.setUserAgent(String("Paperloom-ESP32-") + FIRMWARE_VERSION);
    resolveHttp.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    resolveHttp.setTimeout(30000);
    if (!resolveHttp.begin(secure, _downloadUrl)) {
        Serial.println("OTA: HTTPS begin failed");
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    int code = resolveHttp.GET();
    if (code != 200) {
        Serial.printf("OTA: download GET returned %d\n", code);
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    int contentLength = resolveHttp.getSize();
    if (contentLength <= 0 && _downloadSize > 0) {
        contentLength = _downloadSize;
    }
    // Track unknown-length explicitly so the streaming loop below doesn't
    // accidentally cast a negative `int` to `size_t` (which is SIZE_MAX
    // and silently turns "loop until hit content-length" into "loop forever").
    const bool lengthKnown = (contentLength > 0);
    const size_t expectedBytes = lengthKnown ? (size_t)contentLength : 0;

    WiFiClient* stream = resolveHttp.getStreamPtr();
    if (!stream) {
        Serial.println("OTA: failed to get stream");
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    // Use ESP-IDF OTA API directly with the stream
    esp_ota_handle_t otaHandle = 0;
    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(NULL);
    if (!updatePartition) {
        Serial.println("OTA: no update partition found");
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    Serial.printf("OTA: writing to partition '%s' at 0x%x, size %d\n",
                  updatePartition->label, updatePartition->address, contentLength);

    esp_err_t err = esp_ota_begin(updatePartition, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
    if (err != ESP_OK) {
        Serial.printf("OTA: esp_ota_begin failed: %s\n", esp_err_to_name(err));
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    // Read and write in chunks. WiFiClientSecure on ESP32 decrypts up to a
    // 16 KB TLS record at a time; matching the read buffer to that record
    // size avoids repeated short reads and roughly halves wall-clock time
    // on multi-MB downloads.
    const size_t bufSize = 16384;
    uint8_t* buf = (uint8_t*)malloc(bufSize);
    if (!buf) {
        Serial.println("OTA: malloc failed");
        esp_ota_abort(otaHandle);
        resolveHttp.end();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    size_t written = 0;
    int lastPercent = -1;
    bool success = true;
    unsigned long lastDataTime = millis();

    // Hard ceiling — never write more than the OTA partition can hold.
    const size_t partitionSize = (size_t)updatePartition->size;

    while (!lengthKnown || written < expectedBytes) {
        // Check for timeout (30s with no data)
        if (millis() - lastDataTime > 30000) {
            Serial.println("OTA: download timeout");
            success = false;
            break;
        }

        size_t available = stream->available();
        if (available == 0) {
            // Check if connection is still alive
            if (!stream->connected() && stream->available() == 0) {
                if (lengthKnown && written < expectedBytes) {
                    Serial.println("OTA: connection lost prematurely");
                    success = false;
                }
                break;
            }
            delay(10);
            continue;
        }

        lastDataTime = millis();
        size_t toRead = (available < bufSize) ? available : bufSize;
        if (lengthKnown && (written + toRead) > expectedBytes) {
            toRead = expectedBytes - written;
        }
        // Defence-in-depth: never overflow the OTA partition even if the
        // server lies about Content-Length or sends a streaming response
        // larger than the partition.
        if (written + toRead > partitionSize) {
            Serial.printf("OTA: payload exceeds partition (%u > %u)\n",
                          (unsigned)(written + toRead), (unsigned)partitionSize);
            success = false;
            break;
        }

        int bytesRead = stream->readBytes(buf, toRead);
        if (bytesRead <= 0) {
            delay(10);
            continue;
        }

        err = esp_ota_write(otaHandle, buf, bytesRead);
        if (err != ESP_OK) {
            Serial.printf("OTA: write failed at %u: %s\n", (unsigned)written, esp_err_to_name(err));
            success = false;
            break;
        }

        written += bytesRead;

        if (lengthKnown && progressCallback) {
            int pct = (int)(written * 100 / expectedBytes);
            if (pct != lastPercent) {
                lastPercent = pct;
                progressCallback(pct);
            }
        }
    }

    free(buf);
    resolveHttp.end();

    if (!success || (lengthKnown && written != expectedBytes)) {
        Serial.printf("OTA: download incomplete (%u / %d)\n", (unsigned)written, contentLength);
        esp_ota_abort(otaHandle);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
        Serial.printf("OTA: esp_ota_end failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    err = esp_ota_set_boot_partition(updatePartition);
    if (err != ESP_OK) {
        Serial.printf("OTA: set boot partition failed: %s\n", esp_err_to_name(err));
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        return false;
    }

    // Re-enable power saving
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    Serial.printf("OTA: success! Wrote %u bytes to '%s'\n", written, updatePartition->label);
    return true;
}
