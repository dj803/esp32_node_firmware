#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_https_server.h>          // replaces WebServer.h
#include <Preferences.h>               // Arduino NVS wrapper — TLS credential persistence
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/oid.h>
#include "esp_task_wdt.h"               // esp_task_wdt_reset() — keep WDT fed during keygen
#include "config.h"
#include "logging.h"
#include "credentials.h"
#include "app_config.h"
#include "device_id.h"
#include "wifi_recovery.h"   // apStaScanShouldRun() — background STA scan gate
#include "prefs_quiet.h"     // (v0.4.03) prefsTryBegin — silent on missing namespace
#if AP_CAPTIVE_DNS_ENABLED
#include <DNSServer.h>                 // (#34) captive-portal DNS hijack — must come after config.h
#endif

// =============================================================================
// ap_portal.h  —  Configuration portals (HTTPS)
//
// TWO PORTAL MODES — same HTTPS server, different handler sets:
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ MODE 1: AP Mode (apPortalStart)                                          │
// │  Entered when no credentials exist or all retries fail.                  │
// │  Device creates a Wi-Fi access point "ESP32-Config-xxxxxxxxxxxx".        │
// │  Admin connects to that AP and browses to https://192.168.4.1/           │
// │                                                                           │
// │  GET  /           Full setup form — Wi-Fi, MQTT, OTA URL, rotation key   │
// │  POST /save       Saves all fields, restarts                              │
// │  GET  /status     JSON device info                                        │
// └─────────────────────────────────────────────────────────────────────────┘
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ MODE 2: Settings Mode (settingsServerStart / settingsServerStop)         │
// │  Available when already connected to Wi-Fi (OPERATIONAL state).          │
// │  Triggered by an MQTT command on .../cmd/config_mode.                    │
// │  Device starts a temporary HTTPS server on its STA IP.                   │
// │                                                                           │
// │  GET  /settings   Settings-only form — OTA URL + MQTT hierarchy          │
// │  POST /settings   Saves OTA URL + MQTT settings, does NOT restart        │
// │  GET  /status     JSON device info                                        │
// └─────────────────────────────────────────────────────────────────────────┘
//
// SECURITY: Both portals use HTTPS with a self-signed RSA-2048 certificate.
//   The cert is generated on first boot (~10 s), stored in NVS "esp32portal",
//   and loaded on every subsequent boot. CN = device UUID.  The AP portal IP
//   (192.168.4.1) is embedded as an IP SubjectAltName so Chrome can display
//   the "proceed anyway" option even for self-signed certs.
//   Accepted UX cost: browsers will show an "untrusted certificate" warning.
//   Click Advanced → Proceed (Chrome) or Advanced → Accept Risk (Firefox).
//
// OTA JSON URL:
// The OTA JSON URL is entered through these portals — never hardcoded.
// The full setup form (Mode 1) requires it before saving. The settings form
// (Mode 2) can update it independently at any time.
// =============================================================================


// ── HTTPS server handle ────────────────────────────────────────────────────────
static httpd_handle_t _httpsServer = nullptr;

#if AP_CAPTIVE_DNS_ENABLED
// (#34 Phase 2, v0.4.25) Plain HTTP server on port 80 serving a single
// catch-all 302 → https://192.168.4.1/. Runs alongside the HTTPS portal
// on :443 to complete the captive-portal UX: iOS/Android probe e.g.
// http://captive.apple.com/hotspot-detect.html, DNS resolves to us
// (Phase 1), this redirector responds 302 on :80, the OS flags the
// network as captive and pops the sheet pointing to our HTTPS portal.
// Without this, the captive sheet shows "this site can't be reached"
// (DNS resolves but nothing answers on :80) and the operator still has
// to manually navigate to 192.168.4.1.
static httpd_handle_t _httpRedirectServer = nullptr;
#endif


// ── TLS credential storage ─────────────────────────────────────────────────────
// Self-signed RSA-2048 certificate + private key in PEM format.
// Generated once at first-boot (via loadOrGenerateTlsCreds), cached in NVS.
// Both buffers must remain valid for the lifetime of the HTTPS server.
#define PORTAL_TLS_NVS_NAMESPACE  "esp32portal"
#define PORTAL_TLS_CERT_KEY       "tls_cert"
#define PORTAL_TLS_PKEY_KEY       "tls_pkey"
#define PORTAL_TLS_CERT_MAXLEN    2048   // PEM cert ≈ 900 bytes; 2 KB is safe
#define PORTAL_TLS_PKEY_MAXLEN    2048   // PEM RSA-2048 key ≈ 1700 bytes; 2 KB is safe

static char _tlsCertPem[PORTAL_TLS_CERT_MAXLEN];
static char _tlsKeyPem [PORTAL_TLS_PKEY_MAXLEN];


// ── AP-mode background STA scan (v0.3.15) ─────────────────────────────────────
// While the HTTPS portal is running, the radio sits in APSTA and we
// periodically scan for the configured SSID. When the router returns we
// re-associate and restart into OPERATIONAL — no admin intervention needed.
//
// The scan is skipped whenever an admin HTTPS handler ran within the last
// AP_ADMIN_IDLE_MS, so form entry is never interrupted by the radio briefly
// switching channels.
//
// volatile because _lastAdminActivityMs is written from the httpd worker
// task and read from the main apPortalStart() loop on the main task.
static volatile uint32_t _lastAdminActivityMs = 0;
static bool              _apShouldExit        = false;
static uint32_t          _apExitAtMs          = 0;
static CredentialBundle  _apStaBundle         = {};   // copy taken by apPortalStart()

#if AP_CAPTIVE_DNS_ENABLED
// (#34, v0.4.24) DNS server runs in async-UDP context (handled by AsyncUDP
// in the ESP-IDF lwIP task), so apPortalStart()'s main loop doesn't need
// to poll. Module-static so it outlives the function — `apPortalStart()`
// blocks forever, but mathieucarbou's DNSServer holds the AsyncUDP socket
// internally and tearing it down is only relevant if AP mode were ever
// exited without ESP.restart() (which it isn't).
static DNSServer _apDnsServer;
#endif

// Called from every admin HTTPS handler on entry. Bumping this timestamp
// suppresses the background STA scan for AP_ADMIN_IDLE_MS so that an admin
// filling in the setup form isn't knocked offline by a scan mid-submit.
static inline void apTouchAdminActivity() {
    _lastAdminActivityMs = millis();
}

// One-shot WiFi event handler attached by apPortalStart(). On GOT_IP we set
// the exit flag; the main loop polls it and restarts after the grace period
// (cleanest path back to OPERATIONAL — no portal/HTTPS teardown complexity).
static void apStaGotIpHandler(WiFiEvent_t event, WiFiEventInfo_t /*info*/) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        // (v0.3.36) Latch — only the FIRST GOT_IP after entering AP mode
        // schedules the restart. A flapping router that produces two GOT_IP
        // events in quick succession would otherwise overwrite _apExitAtMs
        // with a fresh deadline and slip the restart window indefinitely.
        if (!_apShouldExit) {
            _apShouldExit = true;
            _apExitAtMs   = millis() + AP_STA_RECONNECT_GRACE_MS;
            LOG_I("AP Portal", "STA got IP — scheduling restart in %d ms",
                  AP_STA_RECONNECT_GRACE_MS);
        }
    }
}


// Loads TLS credentials from NVS. Returns true on success.
static bool _loadTlsCreds() {
    Preferences prefs;
    // (v0.4.03) prefsTryBegin: silent if portal_tls namespace missing
    // (fresh device, never generated TLS cert before).
    if (!prefsTryBegin(prefs, PORTAL_TLS_NVS_NAMESPACE, true)) return false;
    bool hasCert = prefs.isKey(PORTAL_TLS_CERT_KEY);
    bool hasKey  = prefs.isKey(PORTAL_TLS_PKEY_KEY);
    if (!hasCert || !hasKey) { prefs.end(); return false; }

    String cert = prefs.getString(PORTAL_TLS_CERT_KEY, "");
    String key  = prefs.getString(PORTAL_TLS_PKEY_KEY,  "");
    prefs.end();

    if (cert.length() < 20 || key.length() < 20) return false;
    cert.toCharArray(_tlsCertPem, PORTAL_TLS_CERT_MAXLEN);
    key.toCharArray(_tlsKeyPem,   PORTAL_TLS_PKEY_MAXLEN);
    return true;
}

// Saves current _tlsCertPem / _tlsKeyPem to NVS.
static void _saveTlsCreds() {
    Preferences prefs;
    prefs.begin(PORTAL_TLS_NVS_NAMESPACE, false);
    bool ok = prefs.putString(PORTAL_TLS_CERT_KEY, _tlsCertPem) > 0
           && prefs.putString(PORTAL_TLS_PKEY_KEY,  _tlsKeyPem)  > 0;
    prefs.end();
    if (ok) {
        LOG_I("AP Portal", "TLS credentials saved to NVS");
    } else {
        LOG_W("AP Portal", "Could not save TLS creds to NVS — will regenerate next boot");
    }
}

// Generates a self-signed RSA-2048 certificate and private key.
// CN = device UUID; IP SAN = 192.168.4.1 (AP portal fixed address).
// Writes results to _tlsCertPem and _tlsKeyPem.
// Returns true on success. Takes ~10 s on first call.
static bool _generateTlsCreds() {
    mbedtls_pk_context       pk;
    mbedtls_x509write_cert   crt;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context rng;

    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&rng);

    bool ok = false;
    int  ret;

    LOG_I("AP Portal", "Generating RSA-2048 self-signed cert (~10 s)...");

    // (v0.3.37) Subscribe the calling task to TWDT explicitly so the
    // esp_task_wdt_reset() calls below are NOT no-ops regardless of caller.
    // Pre-v0.3.37 there was an inconsistent safety model: from setup() the
    // loop task wasn't yet subscribed → resets were silent no-ops →
    // unprotected; from cmd/config_mode (settings server path) the loop task
    // WAS subscribed → resets fed the watchdog. Same code, two safety models.
    // ESP_ERR_INVALID_ARG = already subscribed (benign), same idiom as ota.h.
    {
        esp_err_t wdtE = esp_task_wdt_add(NULL);
        if (wdtE != ESP_OK && wdtE != ESP_ERR_INVALID_ARG) {
            LOG_W("AP Portal", "esp_task_wdt_add(loopTask) failed: %d (proceeding)", wdtE);
        }
    }

    // The whole keygen call blocks the task long enough (up to ~15 s on slow
    // entropy) that the task watchdog will bite. Feed it around each long step.
    esp_task_wdt_reset();

    const char* pers = "esp32_portal_cert";
    ret = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy,
                                 (const unsigned char*)pers, strlen(pers));
    if (ret) { LOG_E("AP Portal", "ctr_drbg_seed: -0x%04X", -ret); goto cleanup; }
    esp_task_wdt_reset();

    ret = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret) { LOG_E("AP Portal", "pk_setup: -0x%04X", -ret); goto cleanup; }

    // mbedtls_rsa_gen_key has no progress callback in the current lwIP/mbedtls
    // build, so we can only feed the WDT immediately before and after. Keeping
    // a generous task WDT timeout (>15 s) is still required.
    esp_task_wdt_reset();
    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk),
                              mbedtls_ctr_drbg_random, &rng, 2048, 65537);
    esp_task_wdt_reset();
    if (ret) { LOG_E("AP Portal", "rsa_gen_key: -0x%04X", -ret); goto cleanup; }
    LOG_I("AP Portal", "RSA-2048 key generated");

    // Set subject / issuer (self-signed: same for both)
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);

    {
        char cn[96];
        snprintf(cn, sizeof(cn), "CN=%s,O=ESP32,C=US", DeviceId::get().c_str());
        if ((ret = mbedtls_x509write_crt_set_subject_name(&crt, cn)) ||
            (ret = mbedtls_x509write_crt_set_issuer_name(&crt,  cn))) {
            LOG_E("AP Portal", "set subject/issuer: -0x%04X", -ret);
            goto cleanup;
        }
    }

    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    {
        unsigned char serial[4] = {0, 0, 0, 1};
        ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
        if (ret) { LOG_E("AP Portal", "set_serial: -0x%04X", -ret); goto cleanup; }
    }

    ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20340101000000");
    if (ret) { LOG_E("AP Portal", "set_validity: -0x%04X", -ret); goto cleanup; }

    ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (ret) { LOG_E("AP Portal", "set_basic_constraints: -0x%04X", -ret); goto cleanup; }

    // Add IP SubjectAltName: 192.168.4.1 (the fixed AP portal address).
    // DER encoding of GeneralNames sequence: SEQUENCE { [7] { 0xc0 0xa8 0x04 0x01 } }
    {
        static const unsigned char sanVal[] = {
            0x30, 0x06,               // SEQUENCE, 6 bytes
            0x87, 0x04,               // [7] iPAddress, 4 bytes
            0xc0, 0xa8, 0x04, 0x01   // 192.168.4.1
        };
        ret = mbedtls_x509write_crt_set_extension(
            &crt,
            MBEDTLS_OID_SUBJECT_ALT_NAME,
            MBEDTLS_OID_SIZE(MBEDTLS_OID_SUBJECT_ALT_NAME),
            0,   // non-critical
            sanVal, sizeof(sanVal));
        if (ret) { LOG_E("AP Portal", "set SAN: -0x%04X", -ret); goto cleanup; }
    }

    // Write PEM certificate
    memset(_tlsCertPem, 0, sizeof(_tlsCertPem));
    ret = mbedtls_x509write_crt_pem(&crt, (unsigned char*)_tlsCertPem,
                                    sizeof(_tlsCertPem),
                                    mbedtls_ctr_drbg_random, &rng);
    if (ret) { LOG_E("AP Portal", "write_crt_pem: -0x%04X", -ret); goto cleanup; }

    // Write PEM private key
    memset(_tlsKeyPem, 0, sizeof(_tlsKeyPem));
    ret = mbedtls_pk_write_key_pem(&pk, (unsigned char*)_tlsKeyPem,
                                   sizeof(_tlsKeyPem));
    if (ret) { LOG_E("AP Portal", "write_key_pem: -0x%04X", -ret); goto cleanup; }

    LOG_I("AP Portal", "Cert generated (cert %u B, key %u B)",
          (unsigned)strlen(_tlsCertPem), (unsigned)strlen(_tlsKeyPem));
    ok = true;

cleanup:
    mbedtls_pk_free(&pk);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&rng);
    return ok;
}

// Loads TLS credentials from NVS or generates them on first boot.
// Returns true when _tlsCertPem / _tlsKeyPem are valid and ready.
// Called from apPortalStart() and settingsServerStart().
static bool loadOrGenerateTlsCreds() {
    if (_loadTlsCreds()) {
        LOG_I("AP Portal", "TLS credentials loaded from NVS (%u + %u chars)",
              (unsigned)strlen(_tlsCertPem), (unsigned)strlen(_tlsKeyPem));
        return true;
    }
    LOG_I("AP Portal", "No TLS creds in NVS — generating (first boot)");
    // (#32, v0.4.25) Heap-gate the keygen. RSA-2048 + DER serialiser + mbedTLS
    // bignum scratch needs ~50 KB of largest-block during the ~10 s key
    // generation. If the heap is fragmented (e.g. device just landed in AP
    // mode after a long uptime), skip keygen and fall back to plain HTTP —
    // operator still has the portal, just over an unencrypted channel on
    // the AP-only network. A power-cycle yields a clean heap and a TLS
    // upgrade on the next attempt.
    {
        uint32_t hFree  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
        uint32_t hBlock = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (hFree < TLS_KEYGEN_HEAP_FREE_MIN || hBlock < TLS_KEYGEN_HEAP_BLOCK_MIN) {
            LOG_W("AP Portal", "Heap gate skip TLS keygen: free=%u (need >=%u) block=%u (need >=%u) — falling back to HTTP",
                  (unsigned)hFree,  (unsigned)TLS_KEYGEN_HEAP_FREE_MIN,
                  (unsigned)hBlock, (unsigned)TLS_KEYGEN_HEAP_BLOCK_MIN);
            return false;
        }
    }
    if (!_generateTlsCreds()) {
        LOG_E("AP Portal", "TLS credential generation failed — portal will start on HTTP");
        return false;
    }
    _saveTlsCreds();
    return true;
}


// ── CSRF token storage ─────────────────────────────────────────────────────────
// Each portal mode keeps a small ring of recently-issued tokens. A fresh token
// is generated on every GET and embedded in the form as a hidden field. The
// POST handler accepts any unexpired slot and clears it on success (single-use).
//
// 128-bit token (32 lowercase hex chars) via esp_random() — unguessable on a
// per-session basis. Ring size 2 avoids the race that existed with a single
// global slot: two concurrent GETs no longer invalidate each other's token,
// which would cause a spurious 403 when the admin finally submitted the form.
//
// A portMUX guards all read/modify/write so concurrent httpd worker tasks
// cannot tear a half-generated token.
//
// SameSite=Strict cookie set on GET prevents the browser from attaching the
// cookie on any cross-origin POST, blocking drive-by CSRF from malicious pages
// the admin happens to visit while the portal is open.

#define CSRF_RING_SLOTS 2
static char _csrfTokensAp[CSRF_RING_SLOTS][33]       = {};  // Tokens for POST /save
static char _csrfTokensSettings[CSRF_RING_SLOTS][33] = {};  // Tokens for POST /settings
static uint8_t _csrfNextAp       = 0;
static uint8_t _csrfNextSettings = 0;
static portMUX_TYPE _csrfMux = portMUX_INITIALIZER_UNLOCKED;

// Generate a fresh token into an arbitrary 33-byte buffer.
static void _generateCsrfRaw(char* out) {
    // Four 32-bit words from the hardware RNG → 128 bits of entropy
    uint8_t bytes[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        memcpy(bytes + i * 4, &r, 4);
    }
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
    }
    out[32] = '\0';
}

// Allocate the next slot for mode M (AP or Settings), copy the token into `out`.
// `ring` points to the per-mode token array; `next` is the per-mode round-robin index.
static void generateCsrfTokenApRing(char* out) {
    portENTER_CRITICAL(&_csrfMux);
    uint8_t slot = _csrfNextAp;
    _csrfNextAp = (uint8_t)((_csrfNextAp + 1) % CSRF_RING_SLOTS);
    _generateCsrfRaw(_csrfTokensAp[slot]);
    memcpy(out, _csrfTokensAp[slot], 33);
    portEXIT_CRITICAL(&_csrfMux);
}

static void generateCsrfTokenSettingsRing(char* out) {
    portENTER_CRITICAL(&_csrfMux);
    uint8_t slot = _csrfNextSettings;
    _csrfNextSettings = (uint8_t)((_csrfNextSettings + 1) % CSRF_RING_SLOTS);
    _generateCsrfRaw(_csrfTokensSettings[slot]);
    memcpy(out, _csrfTokensSettings[slot], 33);
    portEXIT_CRITICAL(&_csrfMux);
}

// Match the submitted token against any ring slot. On match, clear that slot
// (single-use) and return true. Empty slots (first byte NUL) never match.
static bool verifyAndConsumeCsrfAp(const char* submitted) {
    if (!submitted || submitted[0] == '\0') return false;
    bool ok = false;
    portENTER_CRITICAL(&_csrfMux);
    for (int i = 0; i < CSRF_RING_SLOTS; i++) {
        if (_csrfTokensAp[i][0] != '\0' &&
            strncmp(_csrfTokensAp[i], submitted, 32) == 0) {
            _csrfTokensAp[i][0] = '\0';   // invalidate — single-use
            ok = true;
            break;
        }
    }
    portEXIT_CRITICAL(&_csrfMux);
    return ok;
}

static bool verifyAndConsumeCsrfSettings(const char* submitted) {
    if (!submitted || submitted[0] == '\0') return false;
    bool ok = false;
    portENTER_CRITICAL(&_csrfMux);
    for (int i = 0; i < CSRF_RING_SLOTS; i++) {
        if (_csrfTokensSettings[i][0] != '\0' &&
            strncmp(_csrfTokensSettings[i], submitted, 32) == 0) {
            _csrfTokensSettings[i][0] = '\0';
            ok = true;
            break;
        }
    }
    portEXIT_CRITICAL(&_csrfMux);
    return ok;
}


// ── htmlEscape ────────────────────────────────────────────────────────────────
// Escape the five HTML special characters so a value interpolated into a
// double-quoted attribute (e.g. value="...") cannot break out of the attribute,
// close the tag, or inject a <script>.
//
// Values go through this helper on every GET to /settings and / (AP). Sources:
//   - gAppConfig.* fields, persisted in NVS and writable by anyone who
//     compromises NVS or the OTA_URL_RESPONSE ESP-NOW path.
//   - DeviceId::get() (UUID — safe but defensive escaping costs nothing).
//   - POST success echo (cfg.* values just written to NVS).
//
// Returns an Arduino String for ergonomic concatenation with the existing
// `String + String` HTML builders.
static String htmlEscape(const char* in) {
    if (!in) return String();
    String out;
    out.reserve(strlen(in) + 8);
    for (const char* p = in; *p; p++) {
        switch (*p) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += *p;       break;
        }
    }
    return out;
}


// ── POST body reader + URL-form arg parser ─────────────────────────────────────
// readPostBody() reads the entire POST body into _portalPostBody.
// getFormArg() extracts a named field from the URL-encoded body.
// formArg() is a convenience wrapper that returns an Arduino String.

#define PORTAL_MAX_BODY  3072   // plenty for all form fields at max length

static char _portalPostBody[PORTAL_MAX_BODY];

static bool readPostBody(httpd_req_t* req) {
    _portalPostBody[0] = '\0';
    int contentLen = (int)req->content_len;
    if (contentLen <= 0)           return true;      // GET request — no body
    if (contentLen >= PORTAL_MAX_BODY) {
        LOG_W("AP Portal", "POST body too large (%d bytes)", contentLen);
        return false;
    }
    int rx = httpd_req_recv(req, _portalPostBody, (size_t)contentLen);
    if (rx != contentLen) {
        LOG_W("AP Portal", "POST body recv partial (%d / %d)", rx, contentLen);
        _portalPostBody[0] = '\0';
        return false;
    }
    _portalPostBody[contentLen] = '\0';
    return true;
}

// Decode a single %-encoded byte pair (e.g. '%2F' → '/').
static char _pctDecode(char hi, char lo) {
    auto hv = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
        if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
        return 0;
    };
    return (char)((hv(hi) << 4) | hv(lo));
}

// Extract a named field from _portalPostBody and URL-decode it into out[].
// Writes at most (outLen-1) decoded chars; always null-terminates.
static void getFormArg(const char* name, char* out, size_t outLen) {
    out[0] = '\0';
    if (outLen == 0) return;
    size_t nameLen = strlen(name);
    const char* p   = _portalPostBody;
    const char* end = p + strlen(p);

    while (p < end) {
        const char* eq  = (const char*)memchr(p, '=', (size_t)(end - p));
        if (!eq) break;
        const char* amp = (const char*)memchr(eq + 1, '&', (size_t)(end - (eq + 1)));
        const char* valEnd = amp ? amp : end;

        if ((size_t)(eq - p) == nameLen && memcmp(p, name, nameLen) == 0) {
            const char* v   = eq + 1;
            size_t      idx = 0;
            while (v < valEnd && idx < outLen - 1) {
                if (*v == '%' && v + 2 < valEnd) {
                    out[idx++] = _pctDecode(v[1], v[2]);
                    v += 3;
                } else if (*v == '+') {
                    out[idx++] = ' ';
                    v++;
                } else {
                    out[idx++] = *v++;
                }
            }
            out[idx] = '\0';
            return;
        }
        p = amp ? amp + 1 : end;
    }
}

// Convenience: returns a named form field as an Arduino String.
// Allocates on the heap — use only in request handlers, not tight loops.
static String formArg(const char* name) {
    char buf[512];
    getFormArg(name, buf, sizeof(buf));
    return String(buf);
}


// ── Shared utility ─────────────────────────────────────────────────────────────
static String getDeviceId() {
    String uuid = DeviceId::get();
    if (uuid == "uninitialized") {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        char buf[7];
        snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
        return String(buf);
    }
    return uuid;
}

// Shared CSS injected into both HTML forms. Kept in flash (const char[]).
static const char PAGE_STYLE[] =
    "body{font-family:Arial,sans-serif;max-width:520px;margin:40px auto;padding:0 16px}"
    "input{width:100%;box-sizing:border-box;padding:8px;margin:4px 0 12px;"
           "border:1px solid #ccc;border-radius:4px}"
    "button{background:#2E4057;color:#fff;padding:10px 20px;border:none;"
            "border-radius:4px;cursor:pointer;width:100%;margin-top:8px}"
    "h2{color:#2E4057}h3{color:#048A81;margin-top:20px;margin-bottom:4px}"
    ".note{font-size:12px;color:#666;margin:-8px 0 12px}"
    ".ok{background:#e8f5e9;padding:12px;border-radius:4px;color:#2e7d32}"
    ".locate{background:#E07B39}";


#if AP_CAPTIVE_DNS_ENABLED
// (#34 Phase 2) Catch-all HTTP-port-80 handler. Returns 302 to the HTTPS
// portal regardless of which URL the OS captive-detector probed
// (http://captive.apple.com/..., http://connectivitycheck.gstatic.com/...,
// http://www.msftconnecttest.com/..., etc. — DNS hijack from Phase 1
// resolves all of them to us). The 302 is a non-success response so the
// OS flags the network as captive AND knows where to send the user when
// they tap "Sign in".
static esp_err_t apHandleHttpRedirect(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "https://192.168.4.1/");
    // 302 with empty body is RFC-compliant; some clients prefer a small
    // hint body. Keep it tiny — the OS won't render it, and a slow phone
    // browser shouldn't waste bandwidth on captive-detection traffic.
    httpd_resp_send(req, "Redirecting to portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif


// ── Locate — POST /locate: flash LED so the device can be found physically ──────
static esp_err_t apHandleLocate(httpd_req_t* req) {
    apTouchAdminActivity();
    ledFlashLocate();
    httpd_resp_send(req, "ok", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


// ── GET /status — shared by both portal modes ──────────────────────────────────
// Returns a JSON snapshot: device ID, MAC, firmware version, timestamp, IP,
// OTA JSON URL. Useful for Node-RED dashboards and physical device identification.
static esp_err_t apHandleStatus(httpd_req_t* req) {
    apTouchAdminActivity();
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"device_id\":\"%s\","
        "\"mac\":\"%s\","
        "\"firmware_version\":\"" FIRMWARE_VERSION "\","
        "\"firmware_ts\":%u,"
        "\"ip\":\"%s\","
        "\"ota_json_url\":\"%s\"}",
        DeviceId::get().c_str(),
        DeviceId::getMac().c_str(),
        (unsigned int)(uint32_t)FIRMWARE_BUILD_TIMESTAMP,
        WiFi.localIP().toString().c_str(),
        gAppConfig.ota_json_url);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


// =============================================================================
// MODE 1 — AP Mode portal (full setup form)
// =============================================================================

// ── GET / — Full setup form (AP mode only) ────────────────────────────────────
static esp_err_t apHandleRoot(httpd_req_t* req) {
    apTouchAdminActivity();
    char tokenBuf[33];
    generateCsrfTokenApRing(tokenBuf);   // Fresh token into an available ring slot

    // All gAppConfig.* values are HTML-escaped: a value containing " or > would
    // otherwise break out of the value="…" attribute and allow HTML/JS injection
    // via a NVS writer or a crafted ESP-NOW OTA_URL_RESPONSE.
    String html = String("<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Setup</title>"
        "<style>") + PAGE_STYLE + "</style></head><body>"
        "<h2>ESP32 Device Setup</h2>"
        "<h3>Locate Device</h3>"
        "<button type='button' class='locate' "
          "onclick=\"this.textContent='Flashing...';"
                   "fetch('/locate',{method:'POST'})"
                     ".then(()=>this.textContent='Done \u2014 locate flash complete')"
                     ".catch(()=>this.textContent='Error')\">"
          "Locate This Device"
        "</button>"

        "<form method='POST' action='/save'>"
        "<input type='hidden' name='csrf' value='" + String(tokenBuf) + "'>"

        "<h3>Wi-Fi</h3>"
        "<label>SSID *</label>"
        "<input name='wifi_ssid' required>"
        "<label>Password</label>"
        "<input name='wifi_password' type='password'>"

        "<h3>MQTT Broker</h3>"
        "<label>Broker URL * <span class='note'>(e.g. mqtt://192.168.1.10:1883)</span></label>"
        "<input name='mqtt_broker_url' required>"
        "<label>Username</label>"
        "<input name='mqtt_username'>"
        "<label>Password</label>"
        "<input name='mqtt_password' type='password'>"

        "<h3>OTA Firmware Updates</h3>"
        "<p class='note'>Required for automatic firmware updates. "
            "Paste the stable GitHub Pages URL for the ota.json filter file.</p>"
        "<label>OTA JSON URL *</label>"
        "<input name='ota_json_url' value='" + htmlEscape(gAppConfig.ota_json_url) + "' required "
               "placeholder='https://owner.github.io/repo/ota.json'>"

        "<h3>MQTT Topic Hierarchy</h3>"
        "<p class='note'>ISA-95 path: Enterprise/Site/Area/Line/Cell/DeviceType/DeviceId/...</p>"
        "<label>Enterprise</label>"
        "<input name='mq_enterprise' value='" + htmlEscape(gAppConfig.mqtt_enterprise) + "'>"
        "<label>Site</label>"
        "<input name='mq_site' value='" + htmlEscape(gAppConfig.mqtt_site) + "'>"
        "<label>Area</label>"
        "<input name='mq_area' value='" + htmlEscape(gAppConfig.mqtt_area) + "'>"
        "<label>Line</label>"
        "<input name='mq_line' value='" + htmlEscape(gAppConfig.mqtt_line) + "'>"
        "<label>Cell</label>"
        "<input name='mq_cell' value='" + htmlEscape(gAppConfig.mqtt_cell) + "'>"
        "<label>Device Type</label>"
        "<input name='mq_devtype' value='" + htmlEscape(gAppConfig.mqtt_device_type) + "'>"
        "<label>Node Name <span class='note'>(friendly name e.g. Alpha; A-Z 0-9 _ -, max 15 chars)</span></label>"
        "<input name='node_name' maxlength='15' pattern='[A-Za-z0-9_-]{0,15}' value='" + htmlEscape(gAppConfig.node_name) + "'>"

        "<h3>Security</h3>"
        "<label>Rotation Key <span class='note'>(32 hex chars = 16 bytes, optional)</span></label>"
        "<input name='rotation_key' maxlength='32' "
               "placeholder='e.g. 0102030405060708090a0b0c0d0e0f10'>"

        "<button type='submit'>Save &amp; Restart</button>"
        "</form>"
        "</body></html>";

    // Set CSRF cookie for defence-in-depth (SameSite=Strict blocks cross-origin POST)
    char cookieBuf[72];
    snprintf(cookieBuf, sizeof(cookieBuf),
             "csrf=%s; SameSite=Strict; HttpOnly", tokenBuf);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Set-Cookie", cookieBuf);
    httpd_resp_send(req, html.c_str(), (ssize_t)html.length());
    return ESP_OK;
}


// ── POST /save — Full setup, save all fields, restart ─────────────────────────
static esp_err_t apHandleSave(httpd_req_t* req) {
    apTouchAdminActivity();
    if (!readPostBody(req)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Error: could not read request body.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ── CSRF check ────────────────────────────────────────────────────────────
    // Try every ring slot; on match the slot is cleared so the token is single-use.
    String csrfArg = formArg("csrf");
    if (!verifyAndConsumeCsrfAp(csrfArg.c_str())) {
        LOG_W("AP Portal", "POST /save rejected — CSRF check failed");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
            "Error: CSRF check failed. Reload the setup page and try again.",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    String ssid       = formArg("wifi_ssid");
    String murl       = formArg("mqtt_broker_url");
    String otaJsonUrl = formArg("ota_json_url");

    if (ssid.isEmpty() || murl.isEmpty() || otaJsonUrl.isEmpty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
            "Error: wifi_ssid, mqtt_broker_url and ota_json_url are required.",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ── Field length validation ───────────────────────────────────────────────
    CredentialBundle _tmp;
    if (ssid.length()                     > sizeof(_tmp.wifi_ssid)       - 1 ||
        formArg("wifi_password").length() > sizeof(_tmp.wifi_password)   - 1 ||
        murl.length()                     > sizeof(_tmp.mqtt_broker_url) - 1 ||
        formArg("mqtt_username").length() > sizeof(_tmp.mqtt_username)   - 1 ||
        formArg("mqtt_password").length() > sizeof(_tmp.mqtt_password)   - 1 ||
        otaJsonUrl.length()               > APP_CFG_OTA_JSON_URL_LEN     - 1 ||
        formArg("mq_enterprise").length() > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_site").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_area").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_line").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_cell").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_devtype").length()    > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("node_name").length()     > APP_CFG_NODE_NAME_LEN        - 1) {
        LOG_W("AP Portal", "POST /save rejected — field too long");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
            "Error: one or more fields exceed the maximum allowed length.",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ── Save credentials ──────────────────────────────────────────────────────
    CredentialBundle b;
    ssid.toCharArray(b.wifi_ssid,        sizeof(b.wifi_ssid));
    formArg("wifi_password") .toCharArray(b.wifi_password,   sizeof(b.wifi_password));
    murl.toCharArray(b.mqtt_broker_url,  sizeof(b.mqtt_broker_url));
    formArg("mqtt_username") .toCharArray(b.mqtt_username,   sizeof(b.mqtt_username));
    formArg("mqtt_password") .toCharArray(b.mqtt_password,   sizeof(b.mqtt_password));

    String rotHex = formArg("rotation_key");
    if (rotHex.length() == 32) {
        for (int i = 0; i < 16; i++) {
            char hex[3] = { rotHex[i*2], rotHex[i*2+1], 0 };
            b.rotation_key[i] = (uint8_t)strtol(hex, nullptr, 16);
        }
    }
    b.timestamp = FIRMWARE_BUILD_TIMESTAMP;
    b.source    = CredSource::ADMIN;

    if (!CredentialStore::save(b)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Error: failed to save credentials to NVS.",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Clear stale-credentials flag so the next boot goes straight to WIFI_CONNECT.
    CredentialStore::setCredStale(false);

    // ── Save app config (OTA JSON URL + MQTT hierarchy) ──────────────────────
    AppConfig cfg;
    otaJsonUrl.toCharArray(cfg.ota_json_url,    sizeof(cfg.ota_json_url));
    formArg("mq_enterprise").toCharArray(cfg.mqtt_enterprise,  sizeof(cfg.mqtt_enterprise));
    formArg("mq_site")      .toCharArray(cfg.mqtt_site,        sizeof(cfg.mqtt_site));
    formArg("mq_area")      .toCharArray(cfg.mqtt_area,        sizeof(cfg.mqtt_area));
    formArg("mq_line")      .toCharArray(cfg.mqtt_line,        sizeof(cfg.mqtt_line));
    formArg("mq_cell")      .toCharArray(cfg.mqtt_cell,        sizeof(cfg.mqtt_cell));
    formArg("mq_devtype")   .toCharArray(cfg.mqtt_device_type, sizeof(cfg.mqtt_device_type));
    formArg("node_name")    .toCharArray(cfg.node_name,        sizeof(cfg.node_name));

    if (strlen(cfg.mqtt_enterprise) == 0) strncpy(cfg.mqtt_enterprise, MQTT_ENTERPRISE, sizeof(cfg.mqtt_enterprise)-1);
    if (strlen(cfg.mqtt_site)       == 0) strncpy(cfg.mqtt_site,       MQTT_SITE,       sizeof(cfg.mqtt_site)-1);
    if (strlen(cfg.mqtt_area)       == 0) strncpy(cfg.mqtt_area,       MQTT_AREA,       sizeof(cfg.mqtt_area)-1);
    if (strlen(cfg.mqtt_line)       == 0) strncpy(cfg.mqtt_line,       MQTT_LINE,       sizeof(cfg.mqtt_line)-1);
    if (strlen(cfg.mqtt_cell)       == 0) strncpy(cfg.mqtt_cell,       MQTT_CELL,       sizeof(cfg.mqtt_cell)-1);
    if (strlen(cfg.mqtt_device_type)== 0) strncpy(cfg.mqtt_device_type,MQTT_DEVICE_TYPE,sizeof(cfg.mqtt_device_type)-1);

    if (!AppConfigStore::save(cfg)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Error: failed to save app config to NVS.",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "All settings saved. Restarting...", HTTPD_RESP_USE_STRLEN);
    LOG_I("AP Portal", "Settings saved — restarting in 2 s");
    // 2 s gives the browser time to receive the response before the AP disappears.
    delay(2000);
    ESP.restart();
    return ESP_OK;   // unreachable — kept to satisfy compiler
}


// ── apPortalStart — AP mode, blocks forever (with STA scan in v0.3.15) ─────────
// Starts HTTPS on port 443, registers AP-mode URI handlers, and blocks in a
// loop that periodically scans for the configured SSID. On reconnect the
// device restarts into OPERATIONAL automatically — no manual reset required.
//
// staBundle: if non-null and AP_MODE_STA_ENABLED is set, the portal runs in
// APSTA mode and scans for staBundle->wifi_ssid every AP_STA_SCAN_INTERVAL_MS
// while no admin session is active. Pass nullptr (or leave scan disabled) for
// the legacy "AP only" behavior used on genuinely unprovisioned first boots.
//
// The httpd server runs in its own FreeRTOS task.
void apPortalStart(const CredentialBundle* staBundle = nullptr) {
    String deviceId  = getDeviceId();
    String uuidSuffix = deviceId.length() >= 12
                        ? deviceId.substring(deviceId.length() - 12)
                        : deviceId;
    String ssid = String(AP_SSID_PREFIX) + uuidSuffix;

#if AP_MODE_STA_ENABLED
    // APSTA so the radio can keep a soft-AP up AND scan/associate with the
    // configured router. When the router returns the GOT_IP handler sets
    // _apShouldExit and the main loop below restarts into OPERATIONAL.
    bool staEnabled = (staBundle != nullptr) && (staBundle->wifi_ssid[0] != '\0');
    if (staEnabled) {
        _apStaBundle = *staBundle;
        WiFi.mode(WIFI_AP_STA);
        WiFi.onEvent(apStaGotIpHandler);
        LOG_I("AP Portal", "APSTA mode — will rescan for SSID \"%s\" every %d ms",
              _apStaBundle.wifi_ssid, AP_STA_SCAN_INTERVAL_MS);
    } else {
        WiFi.mode(WIFI_AP);
        LOG_I("AP Portal", "AP-only mode — no stored SSID to rescan for");
    }
#else
    (void)staBundle;
    WiFi.mode(WIFI_AP);
#endif
    WiFi.softAP(ssid.c_str(), AP_PASSWORD);
    LOG_I("AP Portal", "SSID: %s  IP: %s", ssid.c_str(), AP_LOCAL_IP);

#if AP_CAPTIVE_DNS_ENABLED
    // (#34 Phase 1) Start the DNS hijack so iOS / Android captive-portal
    // detection probes resolve to us. Empty domain string = catch-all
    // (every A-query gets the AP IP). DNSServer.start() returns false on
    // socket bind failure (port 53 already in use) — log and continue;
    // the portal still works, just without the captive sheet UX.
    //
    // Phase 2 (port-80 redirector) is required before iOS/Android will
    // actually display the portal page in the captive sheet. With Phase 1
    // alone, Android still flags the network as captive but the user lands
    // on a "this site can't be reached" until they manually navigate.
    // Tracked under #34 in SUGGESTED_IMPROVEMENTS.
    IPAddress apIp = WiFi.softAPIP();
    if (_apDnsServer.start(53, "", apIp)) {
        LOG_I("AP Portal", "Captive DNS hijack active — all queries → %s",
              apIp.toString().c_str());
    } else {
        LOG_W("AP Portal", "Captive DNS hijack failed to start (port 53 bind)");
    }
#endif

    // Load or generate TLS credentials
    bool tlsOk = loadOrGenerateTlsCreds();

    // Build server config
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.stack_size    = 8192;   // TLS handshake needs extra stack
    conf.httpd.max_open_sockets = 3;   // Enough for a single admin session

    if (tlsOk) {
        conf.servercert     = (const uint8_t*)_tlsCertPem;
        conf.servercert_len = strlen(_tlsCertPem) + 1;   // +1 includes null for PEM
        conf.prvtkey_pem    = (const uint8_t*)_tlsKeyPem;
        conf.prvtkey_len    = strlen(_tlsKeyPem) + 1;
        conf.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
        conf.port_secure    = 443;
    } else {
        // Cert generation failed — fall back to plain HTTP so provisioning still works
        LOG_W("AP Portal", "Falling back to plain HTTP on port 80 (TLS unavailable)");
        conf.transport_mode  = HTTPD_SSL_TRANSPORT_INSECURE;
        conf.port_insecure   = 80;
    }

    if (httpd_ssl_start(&_httpsServer, &conf) != ESP_OK) {
        LOG_E("AP Portal", "httpd_ssl_start failed — stuck in AP mode with no portal");
        while (true) delay(1000);   // Halt; device restart is the only recovery
    }

    // Register AP mode URI handlers
    static const httpd_uri_t uriRoot   = { "/",       HTTP_GET,  apHandleRoot,   nullptr };
    static const httpd_uri_t uriSave   = { "/save",   HTTP_POST, apHandleSave,   nullptr };
    static const httpd_uri_t uriLocate = { "/locate", HTTP_POST, apHandleLocate, nullptr };
    static const httpd_uri_t uriStatus = { "/status", HTTP_GET,  apHandleStatus, nullptr };
    httpd_register_uri_handler(_httpsServer, &uriRoot);
    httpd_register_uri_handler(_httpsServer, &uriSave);
    httpd_register_uri_handler(_httpsServer, &uriLocate);
    httpd_register_uri_handler(_httpsServer, &uriStatus);

#if AP_CAPTIVE_DNS_ENABLED
    // (#34 Phase 2, v0.4.25) Plain HTTP-on-:80 catch-all redirector.
    // Only meaningful when the HTTPS portal is up (i.e. TLS keygen
    // succeeded); when TLS failed, the existing HTTPD_SSL_TRANSPORT_INSECURE
    // fallback already binds :80 with the real portal handlers, so a
    // second :80 listener would conflict. Wildcard URI matcher catches
    // every probe URL the OS captive-detector might use.
    if (tlsOk) {
        httpd_config_t plainConf = HTTPD_DEFAULT_CONFIG();
        plainConf.server_port    = 80;
        plainConf.ctrl_port      = 32770;   // httpd_ssl uses 32769 (DEF+1); httpd_default uses 32768; we pick 32770 to avoid both
        plainConf.max_uri_handlers = 2;
        plainConf.uri_match_fn   = httpd_uri_match_wildcard;
        if (httpd_start(&_httpRedirectServer, &plainConf) == ESP_OK) {
            static const httpd_uri_t uriCatchGet = {
                "/*", HTTP_GET, apHandleHttpRedirect, nullptr
            };
            static const httpd_uri_t uriCatchHead = {
                "/*", HTTP_HEAD, apHandleHttpRedirect, nullptr
            };
            httpd_register_uri_handler(_httpRedirectServer, &uriCatchGet);
            httpd_register_uri_handler(_httpRedirectServer, &uriCatchHead);
            LOG_I("AP Portal", "Captive HTTP redirector active on :80 → https://192.168.4.1/");
        } else {
            LOG_W("AP Portal", "Captive :80 redirector failed to start — captive sheet will show broken page");
        }
    }
#endif

    const char* scheme = tlsOk ? "https" : "http";
    LOG_I("AP Portal", "Waiting for admin — browse to %s://192.168.4.1/", scheme);

    // The httpd task handles connections. This main-task loop does three jobs:
    //   1. Poll _apShouldExit (set by apStaGotIpHandler when the background
    //      scan re-associated with the router) and restart after the grace
    //      period so OPERATIONAL can resume cleanly.
    //   2. Every AP_STA_SCAN_INTERVAL_MS, if the admin is idle and no exit
    //      is pending, kick off a WiFi.begin() to poke the STA side into
    //      trying again. The ESP-IDF WiFi stack handles scan/associate
    //      internally — we just need to nudge it periodically.
    //   3. (v0.3.33) Idle-timeout: if no admin activity AND no STA reconnect
    //      for AP_MODE_IDLE_TIMEOUT_MS, hard-restart. AP mode is intended
    //      only for first-boot provisioning or hands-on dev work; a node
    //      that fell to AP mode from a transient failure should not sit
    //      there invisible to the fleet forever. The timer effectively
    //      resets on every admin HTTPS hit (via _lastAdminActivityMs).
    uint32_t lastScanMs   = 0;
    uint32_t apEnteredMs  = millis();
    while (true) {
        uint32_t now = millis();

        if (_apShouldExit && (int32_t)(now - _apExitAtMs) >= 0) {
            LOG_I("AP Portal", "STA reconnected — restarting to resume OPERATIONAL");
            delay(200);            // let any in-flight HTTPS response drain
            ESP.restart();
        }

        // (v0.3.33) Idle-timeout: take the more-recent of "AP entered" and
        // "last admin activity" — that's the timer's reference point.
        // If neither has happened recently enough, restart.
        uint32_t idleRef = (_lastAdminActivityMs > apEnteredMs)
                           ? _lastAdminActivityMs : apEnteredMs;
        if ((now - idleRef) > AP_MODE_IDLE_TIMEOUT_MS) {
            LOG_W("AP Portal", "Idle for %u ms — restarting (AP_MODE_IDLE_TIMEOUT_MS=%u)",
                  (unsigned)(now - idleRef), (unsigned)AP_MODE_IDLE_TIMEOUT_MS);
            delay(200);
            ESP.restart();
        }

#if AP_MODE_STA_ENABLED
        if (staEnabled && !_apShouldExit &&
            apStaScanShouldRun(now, _lastAdminActivityMs, lastScanMs,
                               AP_ADMIN_IDLE_MS, AP_STA_SCAN_INTERVAL_MS)) {
            lastScanMs = now;
            LOG_I("AP Portal", "STA rescan — trying SSID \"%s\"",
                  _apStaBundle.wifi_ssid);
            // WiFi.begin() in APSTA mode kicks an implicit scan+associate
            // sequence. If the router is now reachable we'll get GOT_IP
            // inside AP_STA_RECONNECT_GRACE_MS and _apShouldExit will fire
            // on the next tick. If not, the stack quietly gives up and we
            // try again on the next interval.
            WiFi.begin(_apStaBundle.wifi_ssid, _apStaBundle.wifi_password);
        }
#endif
        delay(100);
    }
}


// =============================================================================
// MODE 2 — Settings server (runs over existing STA Wi-Fi connection)
// Triggered by MQTT cmd/config_mode. Admin browses to the device's LAN IP.
// Does NOT restart after save — MQTT hierarchy updates take effect immediately.
// =============================================================================

// _settingsServerRunning is module-internal — never read from outside ap_portal.h.
// Use settingsServerIsRunning() for any external query.
static bool _settingsServerRunning = false;

// Returns true while the settings HTTPS portal is serving requests.
inline bool settingsServerIsRunning() { return _settingsServerRunning; }


// ── GET /settings — Settings-only form (STA mode) ────────────────────────────
static esp_err_t settingsHandleGet(httpd_req_t* req) {
    char tokenBuf[33];
    generateCsrfTokenSettingsRing(tokenBuf);   // Fresh token into ring slot

    String ip   = WiFi.localIP().toString();
    // gAppConfig.* and DeviceId::get() are HTML-escaped so a malicious value
    // (e.g. a poisoned ota_json_url arriving via ESP-NOW) cannot break out of
    // the value="…" attribute and inject HTML/JS into the admin's browser.
    String html = String("<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Settings</title>"
        "<style>") + PAGE_STYLE + "</style></head><body>"
        "<h2>ESP32 Settings</h2>"
        "<p class='note'>Device ID: " + htmlEscape(DeviceId::get().c_str()) + "<br>"
                         "IP: " + htmlEscape(ip.c_str()) + "</p>"

        "<form method='POST' action='/settings'>"
        "<input type='hidden' name='csrf' value='" + String(tokenBuf) + "'>"

        "<h3>OTA Firmware Updates</h3>"
        "<label>OTA JSON URL *</label>"
        "<input name='ota_json_url' value='" + htmlEscape(gAppConfig.ota_json_url) + "' required "
               "placeholder='https://owner.github.io/repo/ota.json'>"

        "<h3>MQTT Broker</h3>"
        "<p class='note'>Changing these requires a restart to reconnect to the broker.</p>"
        "<label>Broker URL *</label>"
        "<input name='mqtt_broker_url' value='' "
               "placeholder='leave blank to keep current' >"
        "<label>Username</label>"
        "<input name='mqtt_username' placeholder='leave blank to keep current'>"
        "<label>Password</label>"
        "<input name='mqtt_password' type='password' placeholder='leave blank to keep current'>"

        "<h3>MQTT Topic Hierarchy</h3>"
        "<p class='note'>ISA-95: Enterprise/Site/Area/Line/Cell/DeviceType/DeviceId/...</p>"
        "<label>Enterprise</label>"
        "<input name='mq_enterprise' value='" + htmlEscape(gAppConfig.mqtt_enterprise) + "'>"
        "<label>Site</label>"
        "<input name='mq_site'       value='" + htmlEscape(gAppConfig.mqtt_site)       + "'>"
        "<label>Area</label>"
        "<input name='mq_area'       value='" + htmlEscape(gAppConfig.mqtt_area)       + "'>"
        "<label>Line</label>"
        "<input name='mq_line'       value='" + htmlEscape(gAppConfig.mqtt_line)       + "'>"
        "<label>Cell</label>"
        "<input name='mq_cell'       value='" + htmlEscape(gAppConfig.mqtt_cell)       + "'>"
        "<label>Device Type</label>"
        "<input name='mq_devtype'    value='" + htmlEscape(gAppConfig.mqtt_device_type)+ "'>"
        "<label>Node Name <span class='note'>(friendly name e.g. Alpha; A-Z 0-9 _ -, max 15 chars)</span></label>"
        "<input name='node_name'     maxlength='15' pattern='[A-Za-z0-9_-]{0,15}' value='" + htmlEscape(gAppConfig.node_name) + "'>"

        "<button type='submit'>Save Settings</button>"
        "</form>"
        "<button type='button' class='locate' "
          "onclick=\"this.textContent='Flashing...';"
                   "fetch('/locate',{method:'POST'})"
                     ".then(()=>this.textContent='Done \u2014 locate flash complete')"
                     ".catch(()=>this.textContent='Error')\">"
          "Locate This Device"
        "</button>"
        "</body></html>";

    char cookieBuf[72];
    snprintf(cookieBuf, sizeof(cookieBuf),
             "csrf=%s; SameSite=Strict; HttpOnly", tokenBuf);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Set-Cookie", cookieBuf);
    httpd_resp_send(req, html.c_str(), (ssize_t)html.length());
    return ESP_OK;
}


// ── POST /settings — Save OTA URL + MQTT settings, no restart ─────────────────
static esp_err_t settingsHandlePost(httpd_req_t* req) {
    if (!readPostBody(req)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Error: could not read request body.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ── CSRF check ────────────────────────────────────────────────────────────
    // Try every ring slot; on match the slot is cleared so the token is single-use.
    String csrfArg = formArg("csrf");
    if (!verifyAndConsumeCsrfSettings(csrfArg.c_str())) {
        LOG_W("Settings", "POST /settings rejected — CSRF check failed");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
            "Error: CSRF check failed. Reload the settings page and try again.",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    String otaJsonUrl = formArg("ota_json_url");
    if (otaJsonUrl.isEmpty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Error: ota_json_url is required.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // ── Field length validation ───────────────────────────────────────────────
    CredentialBundle _btmp;
    if (otaJsonUrl.length()               > APP_CFG_OTA_JSON_URL_LEN     - 1 ||
        formArg("mq_enterprise").length() > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_site").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_area").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_line").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_cell").length()       > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("mq_devtype").length()    > APP_CFG_MQTT_SEG_LEN         - 1 ||
        formArg("node_name").length()     > APP_CFG_NODE_NAME_LEN        - 1 ||
        formArg("mqtt_broker_url").length() > sizeof(_btmp.mqtt_broker_url) - 1 ||
        formArg("mqtt_username").length() > sizeof(_btmp.mqtt_username)   - 1 ||
        formArg("mqtt_password").length() > sizeof(_btmp.mqtt_password)   - 1) {
        LOG_W("Settings", "POST /settings rejected — field too long");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req,
            "Error: one or more fields exceed the maximum allowed length.",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Build updated AppConfig — overlay changes on top of current values
    AppConfig cfg;
    memcpy(&cfg, &gAppConfig, sizeof(AppConfig));

    otaJsonUrl.toCharArray(cfg.ota_json_url, sizeof(cfg.ota_json_url));

    String mq_ent  = formArg("mq_enterprise");
    String mq_site = formArg("mq_site");
    String mq_area = formArg("mq_area");
    String mq_line = formArg("mq_line");
    String mq_cell = formArg("mq_cell");
    String mq_dev  = formArg("mq_devtype");
    String nodeNm  = formArg("node_name");

    if (mq_ent.length()  > 0) mq_ent.toCharArray(cfg.mqtt_enterprise,   sizeof(cfg.mqtt_enterprise));
    if (mq_site.length() > 0) mq_site.toCharArray(cfg.mqtt_site,        sizeof(cfg.mqtt_site));
    if (mq_area.length() > 0) mq_area.toCharArray(cfg.mqtt_area,        sizeof(cfg.mqtt_area));
    if (mq_line.length() > 0) mq_line.toCharArray(cfg.mqtt_line,        sizeof(cfg.mqtt_line));
    if (mq_cell.length() > 0) mq_cell.toCharArray(cfg.mqtt_cell,        sizeof(cfg.mqtt_cell));
    if (mq_dev.length()  > 0) mq_dev.toCharArray(cfg.mqtt_device_type,  sizeof(cfg.mqtt_device_type));
    // Node name: blank field clears the friendly name back to "" so Node-RED
    // falls back to device_id. Always overwritten when the form is submitted
    // (no "leave blank to keep current" semantics — matches how MQTT segments
    // behave).
    nodeNm.toCharArray(cfg.node_name, sizeof(cfg.node_name));

    // MQTT credentials: blank fields mean "keep the current value"
    String newMurl = formArg("mqtt_broker_url");
    String newMusr = formArg("mqtt_username");
    String newMpwd = formArg("mqtt_password");
    bool mqttChanged = false;

    if (!newMurl.isEmpty() || !newMusr.isEmpty() || !newMpwd.isEmpty()) {
        CredentialBundle b;
        CredentialStore::load(b);
        if (!newMurl.isEmpty()) newMurl.toCharArray(b.mqtt_broker_url, sizeof(b.mqtt_broker_url));
        if (!newMusr.isEmpty()) newMusr.toCharArray(b.mqtt_username,   sizeof(b.mqtt_username));
        if (!newMpwd.isEmpty()) newMpwd.toCharArray(b.mqtt_password,   sizeof(b.mqtt_password));
        b.timestamp = FIRMWARE_BUILD_TIMESTAMP;
        CredentialStore::save(b);
        mqttChanged = true;
    }

    if (!AppConfigStore::save(cfg)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "Error: failed to save settings to NVS.", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Echo the saved values — escape every field since cfg.* mirrors NVS and can
    // carry attacker-controlled strings (ESP-NOW OTA_URL_RESPONSE path) that we
    // don't want interpolated as raw HTML in the admin's browser.
    String msg = String("<div class='ok'>Settings saved.<br>")
               + "OTA JSON URL: "  + htmlEscape(cfg.ota_json_url)    + "<br>"
               + "MQTT hierarchy: " + htmlEscape(cfg.mqtt_enterprise)
               + "/" + htmlEscape(cfg.mqtt_site)
               + "/" + htmlEscape(cfg.mqtt_area)
               + "/" + htmlEscape(cfg.mqtt_line)
               + "/" + htmlEscape(cfg.mqtt_cell)
               + "/" + htmlEscape(cfg.mqtt_device_type);
    if (mqttChanged) msg += "<br>MQTT broker credentials updated — restart required.";
    msg += "</div>";

    String html = String("<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<style>") + PAGE_STYLE + "</style></head><body>"
        "<h2>Settings Saved</h2>" + msg +
        "<br><a href='/settings'>Back to settings</a></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html.c_str(), (ssize_t)html.length());

    LOG_I("Settings", "App config updated via settings portal");
    return ESP_OK;
}


// ── settingsServerStart — start HTTPS settings portal on the LAN interface ──────
// Called from the MQTT onMqttMessage handler when a cmd/config_mode message is
// received. The device stays fully connected to Wi-Fi and MQTT — the HTTPS
// server runs alongside them on the same STA interface, port 443.
//
// Admin browses to https://<device-LAN-IP>/settings from any device on the same
// network. The browser will show an "untrusted certificate" warning (self-signed);
// click Advanced → Proceed to continue.
//
// NOTE: On first call on a device that has never been in AP mode, cert generation
// takes ~10 s. Subsequent calls load the cert from NVS instantly.
void settingsServerStart() {
    if (_settingsServerRunning) return;   // Already running

    bool tlsOk = loadOrGenerateTlsCreds();

    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.httpd.stack_size    = 8192;
    conf.httpd.max_open_sockets = 3;

    if (tlsOk) {
        conf.servercert     = (const uint8_t*)_tlsCertPem;
        conf.servercert_len = strlen(_tlsCertPem) + 1;
        conf.prvtkey_pem    = (const uint8_t*)_tlsKeyPem;
        conf.prvtkey_len    = strlen(_tlsKeyPem) + 1;
        conf.transport_mode = HTTPD_SSL_TRANSPORT_SECURE;
        conf.port_secure    = 443;
    } else {
        LOG_W("Settings", "Falling back to plain HTTP on port 80 (TLS unavailable)");
        conf.transport_mode  = HTTPD_SSL_TRANSPORT_INSECURE;
        conf.port_insecure   = 80;
    }

    if (httpd_ssl_start(&_httpsServer, &conf) != ESP_OK) {
        LOG_E("Settings", "httpd_ssl_start failed — settings portal unavailable");
        return;
    }

    // Register Settings mode URI handlers
    static const httpd_uri_t uriGet    = { "/settings", HTTP_GET,  settingsHandleGet,  nullptr };
    static const httpd_uri_t uriPost   = { "/settings", HTTP_POST, settingsHandlePost, nullptr };
    static const httpd_uri_t uriLocate = { "/locate",   HTTP_POST, apHandleLocate,     nullptr };
    static const httpd_uri_t uriStatus = { "/status",   HTTP_GET,  apHandleStatus,     nullptr };
    httpd_register_uri_handler(_httpsServer, &uriGet);
    httpd_register_uri_handler(_httpsServer, &uriPost);
    httpd_register_uri_handler(_httpsServer, &uriLocate);
    httpd_register_uri_handler(_httpsServer, &uriStatus);

    _settingsServerRunning = true;
    const char* scheme = tlsOk ? "https" : "http";
    LOG_I("Settings", "Portal started — browse to %s://%s/settings",
          scheme, WiFi.localIP().toString().c_str());
}


// ── settingsServerTick — no-op; httpd runs in its own FreeRTOS task ──────────────
// Kept for API compatibility with callers in main.cpp.
// In the previous WebServer implementation, this drove the event loop.
// The ESP-IDF httpd handles connections asynchronously — no polling needed.
bool settingsServerTick() {
    return _settingsServerRunning;
}


// ── settingsServerStop — shut down the settings portal ─────────────────────────
void settingsServerStop() {
    if (!_settingsServerRunning) return;
    httpd_ssl_stop(_httpsServer);
    _httpsServer         = nullptr;
    _settingsServerRunning = false;
    LOG_I("Settings", "Settings portal stopped");
}
