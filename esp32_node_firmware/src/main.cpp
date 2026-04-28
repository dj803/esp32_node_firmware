// =============================================================================
// main.cpp  —  ESP32 Credential Bootstrap Firmware  (PlatformIO entry point)
// Spec : v1.0  |  Framework: Arduino core (ESP32)
//
// Previously esp32_firmware.ino under Arduino IDE layout. Moved to src/main.cpp
// for the PlatformIO migration in v0.3.03. The .cpp extension means Arduino.h
// is NOT auto-prepended by the compiler — the explicit #include below is
// required, whereas it was implicit under the .ino convention.
//
// STATE MACHINE OVERVIEW:
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ BOOT                                                                 │
//   │  Has admin credentials in NVS?                                       │
//   │  ├─ YES → WIFI_CONNECT (skip bootstrap — admin already configured)   │
//   │  └─ NO  → BOOTSTRAP_REQUEST                                          │
//   └─────────────────────────────────────────────────────────────────────┘
//            │
//            ▼
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ BOOTSTRAP_REQUEST                                                    │
//   │  Broadcast ESP-NOW credential request (up to BOOTSTRAP_MAX_ATTEMPTS) │
//   │  ├─ Got bundle from sibling → save to NVS → WIFI_CONNECT             │
//   │  ├─ No response + old NVS bundle exists → use it → WIFI_CONNECT      │
//   │  └─ No response + no NVS bundle → AP_MODE                            │
//   └─────────────────────────────────────────────────────────────────────┘
//            │
//            ▼
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ WIFI_CONNECT                                                         │
//   │  Try to associate with Wi-Fi (up to WIFI_MAX_ATTEMPTS)               │
//   │  ├─ Connected → OPERATIONAL                                          │
//   │  ├─ Failed + restart count < DEVICE_RESTART_MAX → restart            │
//   │  └─ Failed + restart limit reached → AP_MODE                         │
//   └─────────────────────────────────────────────────────────────────────┘
//            │
//            ▼
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ OPERATIONAL                                                          │
//   │  • Connected to MQTT broker                                          │
//   │  • Serving credentials to siblings via ESP-NOW                       │
//   │  • Sending heartbeats and checking for OTA updates                   │
//   │  • Listening for credential rotation and OTA trigger commands        │
//   │  • WiFi loss → exponential backoff reconnect, never restarts (0.3.15)│
//   └─────────────────────────────────────────────────────────────────────┘
//
//   AP_MODE  (entered from any state on unrecoverable failure):
//   │  Hosts a Wi-Fi access point + HTTP portal for manual configuration.  │
//   │  Since v0.3.15 the radio also runs STA: every 30 s the firmware      │
//   │  scans for the configured SSID; when the router returns the device   │
//   │  auto-restarts into OPERATIONAL. Admin-idle gated so form entry is   │
//   │  never interrupted. Still exits via ESP.restart() after Save.        │
//
// BOOT SEQUENCE:
//   setup():  Serial, LED, ws2812, WiFi.mode(), WiFiEvent, DeviceId, AppConfig,
//             BOOT state check → BOOTSTRAP_REQUEST: spawn async task, return
//                              → WIFI_CONNECT: WiFi.begin(), return
//                              → AP_MODE: apPortalStart() (never returns)
//   loop():   Polls bootstrap task (BOOTSTRAP_REQUEST), polls wifiConnected
//             (WIFI_CONNECT), then services startup + ongoing tasks (OPERATIONAL)
//
// INCLUDE ORDER:
//   Most cross-module forward declarations are now in *_fwd.h headers, so
//   accidental reordering produces a clear compile error instead of a link
//   failure. A few ordering constraints remain:
//
//   ws2812.h  BEFORE  mqtt_client.h  — ws2812_fwd.h forward-declares LedEvent
//       as an incomplete type; mqtt_client.h uses it by reference which is fine,
//       but rfid.h creates LedEvent values and needs the full definition first.
//   mqtt_client.h  BEFORE  ota.h    — ota.h calls mqttPublishStatus() which is
//       a static defined (not just declared) in mqtt_client.h.
//   espnow_ranging.h  AFTER  mqtt_client.h  — calls mqttPublishJson() (static).
//   rfid.h  AFTER  mqtt_client.h AND ws2812.h  — calls both.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_log.h>   // (v0.4.02) esp_log_level_set — silence Preferences spam

#include "config.h"
#include "logging.h"
#include "led.h"
#include "credentials.h"
#include "crypto.h"
#include "espnow_bootstrap.h"
#include "espnow_responder.h"
#include "wifi_recovery.h"   // pure helpers — backoff index, auth-fail classifier
#include "ap_portal.h"
#include "ws2812.h"        // WS2812B strip — before mqtt_client.h and rfid.h
#include "relay.h"         // (v0.5.0) BDD 2CH relay — before mqtt_client.h (publishers go in mqtt_client.h)
#include "hall.h"          // (v0.5.0) BMT 49E Hall — before mqtt_client.h
#include "mqtt_client.h"   // MUST come before ota.h — defines mqttPublishStatus()
#include "ota.h"
#include "ota_validation.h"  // Phase 2 / v0.3.34 — post-OTA self-test + rollback
#include "coredump_publish.h"  // v0.4.17 / #65 — publish ESP-IDF core-dump summary on boot
#include "espnow_ranging.h"  // MUST come after mqtt_client.h — uses mqttPublish/mqttConnected
#include "rfid.h"          // MUST come after mqtt_client.h AND ws2812.h
#ifdef BLE_ENABLED
#include "ble.h"           // MUST come last — uses mqttPublish from mqtt_client.h
#endif

// (v0.4.02) Compile-time guards on third-party library APIs. Pure
// static_assert + #error block; no runtime cost. Catches a silent
// signature drift (e.g. accidental NimBLE / AsyncMqttClient version bump
// in platformio.ini) at build time rather than in the field.
#include "lib_api_assert.h"

// (v0.4.03) Heap-phase logging — one-line macro callable from setup()
// + loop() at major subsystem-init boundaries. Output format:
//   [I][Heap] <phase>: free=12345 largest=6789
// Tag matches the existing OTA path's "Heap at trigger" / "Heap after
// BLE deinit" lines so field log analysis can grep one tag for all
// memory-phase events.
#define LOG_HEAP(phase)                                                      \
    LOG_I("Heap", "%s: free=%u largest=%u",                                  \
          (phase),                                                            \
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),                \
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT))


// ── State machine definition ──────────────────────────────────────────────────
// The firmware progresses through these states once during setup().
// After reaching OPERATIONAL, the loop() function handles ongoing work.
enum class State {
    BOOT,               // Entry point — check NVS for existing admin credentials
    BOOTSTRAP_REQUEST,  // Ask siblings for credentials via ESP-NOW
    WIFI_CONNECT,       // Attempt to associate with the Wi-Fi router
    OPERATIONAL,        // Normal running state — MQTT connected, serving siblings
    AP_MODE             // Fallback — host a config portal for manual setup
};

static State           currentState = State::BOOT;  // Start at BOOT on every power-up
static CredentialBundle activeBundle;               // The credential bundle currently in use


// ── Wi-Fi connection state ────────────────────────────────────────────────────
// Updated by the WiFiEvent callback (which runs in a separate FreeRTOS task),
// so it is read from the main task in connectWifi() and loop().
static bool wifiConnected = false;

// Last STA_DISCONNECTED reason code seen by the WiFiEvent callback. Read by
// the OPERATIONAL recovery loop to distinguish "router offline" (keep
// backing off forever) from "auth failed" (fall to AP mode after
// WIFI_AUTH_FAIL_CYCLES consecutive cycles). See wifi_recovery.h for the
// classifier — added v0.3.15.
static volatile uint8_t wifiLastDisconnectReason = 0;


// ── WiFiEvent ─────────────────────────────────────────────────────────────────
// Arduino Wi-Fi event handler — called asynchronously by the Wi-Fi stack.
// We only care about two events: got IP (connected) and disconnected.
//
// The two-argument signature (v0.3.15) gives us the reason code on
// STA_DISCONNECTED so the recovery loop can tell "router power-cycled"
// from "password is wrong."
void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            // DHCP assigned an IP address — we are fully connected
            LOG_I("WiFi", "Connected - IP: %s", WiFi.localIP().toString().c_str());
            wifiConnected = true;
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // Lost the connection — could be a temporary blip or a permanent drop.
            // Capture the reason code before we zero wifiConnected so the
            // OPERATIONAL recovery path can discriminate transient vs. terminal.
            wifiLastDisconnectReason = info.wifi_sta_disconnected.reason;
            if (wifiConnected) {
                // Only log when transitioning from connected → disconnected.
                // Suppresses repeated events fired by the WiFi stack during
                // failed association attempts (where we were never connected).
                LOG_I("WiFi", "Disconnected (reason=%u)",
                      (unsigned)wifiLastDisconnectReason);
            }
            wifiConnected = false;
            break;

        default:
            break;   // Ignore all other events (SCAN_DONE, AP events, etc.)
    }
}


// ── connectWifi ───────────────────────────────────────────────────────────────
// Starts a Wi-Fi connection attempt and blocks until either an IP is assigned
// or WIFI_CONNECT_TIMEOUT_MS elapses.  Used only on the rare sibling re-verify
// path during WIFI_CONNECT (SIBLING_REVERIFY_ATTEMPTS == 1); the normal
// boot-time Wi-Fi association is non-blocking and driven from loop().
// Returns true if connected, false if timed out.
static bool connectWifi(const CredentialBundle& b) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(b.wifi_ssid, b.wifi_password);
    LOG_I("WiFi", "Connecting to SSID: %s", b.wifi_ssid);
    uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
    while (!wifiConnected && millis() < deadline) delay(100);
    return wifiConnected;
}


// ── Boot-time WiFi state (shared between setup and loop) ─────────────────────
// These track the non-blocking WIFI_CONNECT phase that loop() drives.
static int      _bootWifiAttempts = 0;
static uint32_t _bootWifiDeadline = 0;
static bool     _bootWifiBegan    = false;


// =============================================================================
// setup() — runs once at boot
//
// Performs hardware initialisation and determines the initial boot state,
// then returns as quickly as possible so loop() can drive the state machine.
//
//   • If BOOTSTRAP_REQUEST: spawns the async ESP-NOW task and returns.
//   • If WIFI_CONNECT:      calls WiFi.begin() (non-blocking) and returns.
//   • If AP_MODE:           calls apPortalStart() which never returns.
//
// All subsequent state transitions (BOOTSTRAP→WIFI_CONNECT→OPERATIONAL) happen
// inside loop().  One-time OPERATIONAL setup (broker discovery, mqttBegin,
// peripheral init) also runs in loop() on the first OPERATIONAL iteration.
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);   // Give the serial monitor time to attach before the first print
    ledInit();
    ledSetPattern(LedPattern::BOOT);
    ws2812Init();
    ws2812TaskStart();
    Serial.println();
    LOG_I("BOOT", "ESP32 Credential Bootstrap Firmware v" FIRMWARE_VERSION);

    // (v0.4.03) Preferences NOT_FOUND log spam is now suppressed structurally
    // via prefsTryBegin() in include/prefs_quiet.h — every read-only NVS
    // open pre-checks namespace existence via nvs_open(), so the Arduino
    // log_e() inside Preferences::begin() is never reached when the
    // namespace is genuinely missing. The v0.4.02 esp_log_level_set
    // tombstone has been removed.

    LOG_HEAP("after-serial");

    // MAC address is readable only after Wi-Fi mode is set.
    // Register WiFiEvent early so it is active before any WiFi.begin() call.
    WiFi.mode(WIFI_STA);
    WiFi.onEvent(WiFiEvent);
    DeviceId::init();

    // Load GitHub owner/repo and MQTT ISA-95 topic segments from NVS.
    // Falls back to config.h compile-time defaults for any field not yet saved.
    // Must run BEFORE mqttBegin() (topic building) and otaCheckNow() (OTA URL).
    AppConfigStore::load();

    // (v0.3.34) Detect post-OTA boot. If the running partition is in
    // PENDING_VERIFY state, set a deadline; otaValidationConfirmHealth() must
    // be called before it expires or the bootloader will roll us back.
    otaValidationCheckBoot();

    // (#76 sub-D, v0.4.24) Restart-loop cool-off. If the device has just
    // ESP.restart()'d ≥ MQTT_RESTART_LOOP_THRESHOLD times in a row with
    // reason=mqtt_unrecoverable, the broker is unreachable in a way that
    // restarting won't fix (bad URL, bad creds, broker offline indefinitely).
    // Enter AP mode so the operator can inspect via web UI without a serial
    // cable. Streak is broken automatically by mqttMarkHealthyIfDue() once
    // a successful boot reaches MQTT_LOOP_HEALTHY_UPTIME_MS of connectivity.
    {
        uint8_t loopCount = RestartHistory::countTrailingCause("mqtt_unrecoverable");
        if (loopCount >= MQTT_RESTART_LOOP_THRESHOLD) {
            LOG_W("BOOT", "Restart-loop detected: %u consecutive mqtt_unrecoverable — entering AP mode",
                  (unsigned)loopCount);
            // Try to load creds so AP portal can run APSTA background scan
            // (if creds are present they'll let the portal auto-recover when
            // the broker comes back). If creds are absent, AP portal still
            // starts in pure-AP mode for first-touch provisioning.
            CredentialStore::load(activeBundle);
            currentState = State::AP_MODE;
            return;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // STATE: BOOT
    // Determine the initial state based on NVS credential freshness.
    // ─────────────────────────────────────────────────────────────────────────
    {
        bool credStale = CredentialStore::isCredStale();
        if (credStale) {
            CredentialStore::setCredStale(false);
            LOG_W("BOOT", "Credentials flagged stale — forcing sibling re-verify");
            currentState = State::BOOTSTRAP_REQUEST;
        } else if (CredentialStore::hasPrimary()) {
            LOG_I("BOOT", "Admin credentials found — skipping bootstrap");
            if (CredentialStore::load(activeBundle)) {
                currentState = State::WIFI_CONNECT;
            } else {
                LOG_E("BOOT", "Failed to load admin credentials — entering AP mode");
                currentState = State::AP_MODE;
            }
        } else {
            currentState = State::BOOTSTRAP_REQUEST;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // BOOTSTRAP_REQUEST: spawn the async task — loop() will poll for completion
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::BOOTSTRAP_REQUEST) {
        LOG_I("BOOT", "Starting async ESP-NOW bootstrap");
        ledSetPattern(LedPattern::WIFI_CONNECTING);
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "bootstrap", sizeof(e.animName)); ws2812PostEvent(e); }
        espnowBootstrapBegin();   // FreeRTOS task — returns immediately
        return;                   // loop() drives the rest
    }

    // ─────────────────────────────────────────────────────────────────────────
    // WIFI_CONNECT: call WiFi.begin() non-blocking — loop() will poll
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::WIFI_CONNECT) {
        ledSetPattern(LedPattern::WIFI_CONNECTING);
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "wifi", sizeof(e.animName)); ws2812PostEvent(e); }
        LOG_I("WiFi", "Association attempt 1 of %d — SSID: %s",
              WIFI_MAX_ATTEMPTS, activeBundle.wifi_ssid);
        WiFi.disconnect(true);
        WiFi.begin(activeBundle.wifi_ssid, activeBundle.wifi_password);
        _bootWifiAttempts = 1;
        _bootWifiDeadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
        _bootWifiBegan    = true;
        return;   // loop() drives the rest
    }

    // ─────────────────────────────────────────────────────────────────────────
    // AP_MODE: host the config portal — never returns
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::AP_MODE) {
        ledSetPattern(LedPattern::AP_MODE);
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "ap_mode", sizeof(e.animName)); ws2812PostEvent(e); }
        // Pass activeBundle so apPortalStart can run the APSTA background scan
        // if creds are loaded. On truly-first-boot (no NVS bundle) this branch
        // is not reached — the BOOTSTRAP_REQUEST path enters AP_MODE via the
        // null-bundle fall-through below. (v0.3.15)
        apPortalStart(&activeBundle);   // never returns
    }
}


// =============================================================================
// loop() — runs repeatedly after setup() returns
//
// Drives the full boot state machine through to OPERATIONAL, then handles
// ongoing work (Wi-Fi recovery, MQTT, OTA, ranging, RFID, BLE).
//
//   BOOTSTRAP_REQUEST  — polls the async ESP-NOW task; on completion
//                        processes the bundle and transitions to WIFI_CONNECT
//                        or AP_MODE.
//   WIFI_CONNECT       — polls wifiConnected (set by WiFiEvent); retries up
//                        to WIFI_MAX_ATTEMPTS; synchronous sibling re-verify
//                        on total failure; falls through to AP_MODE or restart.
//   AP_MODE            — calls apPortalStart() which never returns.
//   OPERATIONAL        — one-time service startup on first entry, then the
//                        regular heartbeat / OTA / RFID / BLE tasks.
// =============================================================================
void loop() {

    // ─────────────────────────────────────────────────────────────────────────
    // STATE: BOOTSTRAP_REQUEST — poll the FreeRTOS task started in setup()
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::BOOTSTRAP_REQUEST) {
        if (!espnowBootstrapIsDone()) return;   // task still running

        // Task completed — process result
        const uint64_t BOOTSTRAP_MAX_TS = FIRMWARE_BUILD_TIMESTAMP + SIBLING_TS_MAX_FUTURE_S;
        bool gotBundle = false;

        CredentialBundle received;
        if (espnowBootstrapGetResult(received)) {
            if (received.timestamp > BOOTSTRAP_MAX_TS) {
                LOG_W("BOOT", "Bootstrap bundle timestamp %llu exceeds cap — discarding",
                      received.timestamp);
            } else {
                // Compare with stored bundle and keep the newer one
                CredentialBundle stored;
                bool hasStored    = CredentialStore::load(stored);
                bool useReceived  = !hasStored || received.timestamp > stored.timestamp;

                if (useReceived) {
                    activeBundle = received;
                    CredentialStore::save(activeBundle);
                    LOG_I("BOOT", "New bundle adopted and saved");
                } else {
                    activeBundle = stored;
                    LOG_I("BOOT", "Kept existing NVS bundle (newer timestamp)");
                }
                gotBundle = true;
            }
        }

        if (!gotBundle) {
            // All bootstrap attempts failed — fall back to NVS bundle if any
            if (CredentialStore::load(activeBundle)) {
                LOG_I("BOOT", "No sibling — using stored NVS bundle");
                gotBundle = true;
            }
        }

        if (!gotBundle) {
            LOG_W("BOOT", "No credentials available — entering AP mode");
            currentState = State::AP_MODE;
            return;
        }

        // Transition to WIFI_CONNECT — fire WiFi.begin() and come back next tick
        currentState = State::WIFI_CONNECT;
        ledSetPattern(LedPattern::WIFI_CONNECTING);
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "wifi", sizeof(e.animName)); ws2812PostEvent(e); }
        LOG_I("WiFi", "Association attempt 1 of %d — SSID: %s",
              WIFI_MAX_ATTEMPTS, activeBundle.wifi_ssid);
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);
        WiFi.begin(activeBundle.wifi_ssid, activeBundle.wifi_password);
        _bootWifiAttempts = 1;
        _bootWifiDeadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
        _bootWifiBegan    = true;
        return;
    }


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: WIFI_CONNECT — non-blocking WiFi association
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::WIFI_CONNECT) {
        // First entry when coming straight from setup() (BOOT→WIFI_CONNECT path)
        if (!_bootWifiBegan) {
            LOG_I("WiFi", "Association attempt 1 of %d — SSID: %s",
                  WIFI_MAX_ATTEMPTS, activeBundle.wifi_ssid);
            WiFi.disconnect(true);
            WiFi.begin(activeBundle.wifi_ssid, activeBundle.wifi_password);
            _bootWifiAttempts = 1;
            _bootWifiDeadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
            _bootWifiBegan    = true;
        }

        if (wifiConnected) {
            // Connected — reset restart counter and proceed to OPERATIONAL
            CredentialStore::clearRestartCount();
            ledSetPattern(LedPattern::WIFI_CONNECTED);
            currentState   = State::OPERATIONAL;
            _bootWifiBegan = false;
            return;
        }

        if (millis() < _bootWifiDeadline) return;   // still waiting for this attempt

        // Attempt timed out
        LOG_W("WiFi", "Attempt %d of %d timed out", _bootWifiAttempts, WIFI_MAX_ATTEMPTS);

        if (_bootWifiAttempts < WIFI_MAX_ATTEMPTS) {
            // Retry immediately
            _bootWifiAttempts++;
            LOG_I("WiFi", "Association attempt %d of %d", _bootWifiAttempts, WIFI_MAX_ATTEMPTS);
            WiFi.disconnect(true);
            WiFi.begin(activeBundle.wifi_ssid, activeBundle.wifi_password);
            _bootWifiDeadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
            return;
        }

        // All Wi-Fi attempts exhausted — try synchronous sibling re-verify
        // (SIBLING_REVERIFY_ATTEMPTS == 1 so this path is rare and brief)
        LOG_W("WiFi", "All attempts failed — trying sibling credential re-verify");
        _bootWifiBegan = false;

        const uint64_t REVERIFY_MAX_TS = FIRMWARE_BUILD_TIMESTAMP + SIBLING_TS_MAX_FUTURE_S;
        bool connected = false;

        for (int sib = 0; sib < SIBLING_REVERIFY_ATTEMPTS && !connected; sib++) {
            CredentialBundle fresh;
#ifdef SIBLING_PRIMARY_SELECTION
            bool gotFresh = espnowBootstrapWithPrimarySelection(fresh);
#else
            bool gotFresh = espnowBootstrap(fresh);
#endif
            if (gotFresh) {
                if (fresh.timestamp > REVERIFY_MAX_TS) {
                    LOG_W("WiFi", "Sibling bundle timestamp %llu exceeds cap — discarding",
                          fresh.timestamp);
                } else if (fresh.timestamp > activeBundle.timestamp) {
                    activeBundle = fresh;
                    CredentialStore::save(activeBundle);
                    LOG_I("WiFi", "Fresh credentials from sibling — retrying WiFi");
                    for (int ra = 1; ra <= WIFI_MAX_ATTEMPTS && !connected; ra++) {
                        LOG_I("WiFi", "Re-verify WiFi attempt %d of %d", ra, WIFI_MAX_ATTEMPTS);
                        connected = connectWifi(activeBundle);
                    }
                    if (connected) {
                        CredentialStore::clearRestartCount();
                        ledSetPattern(LedPattern::WIFI_CONNECTED);
                        currentState = State::OPERATIONAL;
                    } else {
                        LOG_W("WiFi", "Still failed with fresh sibling credentials");
                    }
                } else {
                    LOG_I("WiFi", "Sibling bundle not newer (ts=%llu <= stored=%llu)",
                          fresh.timestamp, activeBundle.timestamp);
                }
            } else {
                LOG_W("WiFi", "No sibling responded to re-verify request");
            }
        }

        if (!connected) {
            // Route through the wifi-outage counter (v0.3.15) — router blips
            // no longer exhaust DEVICE_RESTART_MAX. AP mode is still reachable
            // via WIFI_OUTAGE_RESTART_MAX, but from there the new background
            // STA scan in ap_portal.h recovers automatically when the router
            // returns, so the user no longer has to manually reset.
            uint8_t restarts = CredentialStore::incrementRestartCount(RestartReason::WIFI_OUTAGE);
            LOG_W("WiFi", "Failed — wifi-outage restart count %d / %d",
                  restarts, WIFI_OUTAGE_RESTART_MAX);
            if (restarts < WIFI_OUTAGE_RESTART_MAX) {
                LOG_W("WiFi", "Restarting device...");
                ledSetPattern(LedPattern::ERROR);
                delay(1000);
                ESP.restart();
            } else {
                LOG_W("WiFi", "Wifi-outage restart limit reached — entering AP mode "
                              "(background STA scan will retry every %d ms)",
                      AP_STA_SCAN_INTERVAL_MS);
                currentState = State::AP_MODE;
            }
        }
        return;
    }


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: AP_MODE — host the config portal (never returns)
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::AP_MODE) {
        ledSetPattern(LedPattern::AP_MODE);
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "ap_mode", sizeof(e.animName)); ws2812PostEvent(e); }
        // Pass activeBundle so apPortalStart can run the APSTA background scan
        // if creds are loaded. On truly-first-boot (no NVS bundle) this branch
        // is not reached — the BOOTSTRAP_REQUEST path enters AP_MODE via the
        // null-bundle fall-through below. (v0.3.15)
        apPortalStart(&activeBundle);   // never returns — calls ESP.restart() after credentials saved or router returns
        return;
    }


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: OPERATIONAL
    // ─────────────────────────────────────────────────────────────────────────

    // ── One-time service startup ──────────────────────────────────────────────
    // Runs on the very first OPERATIONAL loop() tick. Doing this here (rather
    // than in setup()) means discoverBroker() blocks in loop() context where all
    // FreeRTOS tasks (LED strip, Wi-Fi stack) continue to run unimpeded.
    {
        static bool _servicesStarted = false;
        if (!_servicesStarted) {
            _servicesStarted = true;
            LOG_I("BOOT", "Wi-Fi connected — starting services");
            LOG_HEAP("after-wifi");

            espnowResponderSetBundle(activeBundle);
            espnowResponderStart();

            // WiFi is confirmed connected. GitHub reachability is assumed until
            // the first OTA check proves otherwise (~1 hour from boot).
            responderSetHealthFlag(0, true);   // bit 0: WiFi connected
            responderSetHealthFlag(2, true);   // bit 2: GitHub assumed reachable

            BrokerResult broker = discoverBroker(activeBundle.mqtt_broker_url);
            if (broker.found()) {
                saveBrokerToCache(broker.host, broker.port);
                espnowResponderSetBroker(broker.host, broker.port);
            }

            { LedEvent e{}; e.type = LedEventType::RESET; ws2812PostEvent(e); }

            mqttBegin(activeBundle, broker);
            LOG_HEAP("after-mqtt");

#ifdef RFID_ENABLED
            rfidInit();   // SPI free now that ESP-NOW channel scan is complete
#endif
#ifdef BLE_ENABLED
            bleInit();    // BLE + WiFi share 2.4 GHz; ESP-IDF handles coexistence
            LOG_HEAP("after-ble");
#endif
#ifdef RELAY_ENABLED
            relayInit();   // (v0.5.0) BDD 2CH relay — drives HIGH before OUTPUT to prevent boot-click
            LOG_HEAP("after-relay");
#endif
#ifdef HALL_ENABLED
            hallInit();    // (v0.5.0) BMT 49E Hall — ADC1 single-shot, periodic publish
            LOG_HEAP("after-hall");
#endif
            return;   // Give other tasks a cycle before entering the regular loop
        }
    }

    // ── Wi-Fi health check ────────────────────────────────────────────────────
    // Exponential-backoff reconnect. Never restarts for WiFi loss alone — the
    // ESP-IDF WiFi stack re-associates automatically when the router returns.
    // The old 3-strike-then-restart loop was actively sabotaging that recovery:
    // a ~5-min router outage would cascade through DEVICE_RESTART_MAX (=3)
    // reboots and pin the whole fleet into AP mode within ~2.5 min. (v0.3.15)
    //
    // Auth-fail fast path: if the STA_DISCONNECTED reason code looks like a
    // bad password (classifier in wifi_recovery.h) across WIFI_AUTH_FAIL_CYCLES
    // consecutive backoff cycles, flag creds stale and fall to AP mode — where
    // the background STA scan will still retry if the user corrects the
    // password via admin provisioning.
    {
        static bool     _wifiWasConnected   = true;   // true = assume connected at startup
        static uint8_t  _wifiBackoffIdx     = 0;
        static uint32_t _wifiNextAttemptMs  = 0;
        static uint32_t _wifiOutageStartMs  = 0;
        static uint8_t  _wifiAuthFailCycles = 0;

        if (wifiConnected && !_wifiWasConnected) {
            // Just re-established connection — reset recovery state and log
            // the outage duration so operators can see how long the gap was.
            uint32_t outageMs = millis() - _wifiOutageStartMs;
            _wifiWasConnected   = true;
            _wifiBackoffIdx     = 0;
            _wifiNextAttemptMs  = 0;
            _wifiAuthFailCycles = 0;
            CredentialStore::clearWifiOutageCount();   // outage survived → clear budget
            LOG_I("Loop", "Wi-Fi reconnected after %u ms outage", (unsigned)outageMs);
            ledSetPattern(LedPattern::WIFI_CONNECTED);
        }

        if (!wifiConnected) {
            if (_wifiWasConnected) {
                _wifiWasConnected   = false;
                _wifiBackoffIdx     = 0;
                _wifiNextAttemptMs  = 0;
                _wifiOutageStartMs  = millis();
                _wifiAuthFailCycles = 0;
                LOG_W("Loop", "Wi-Fi lost — entering backoff-retry (never restarts for WiFi loss)");
                ledSetPattern(LedPattern::WIFI_CONNECTING);
            }

            if (millis() < _wifiNextAttemptMs) return;   // still backing off

            // Fire an attempt. Schedule the next one at the current backoff
            // step, then advance the index (saturating at the last slot).
            uint32_t waitMs = WIFI_BACKOFF_STEPS_MS[_wifiBackoffIdx];
            LOG_I("Loop", "WiFi reconnect attempt (next backoff step %u ms)",
                  (unsigned)waitMs);
            if (_wifiBackoffIdx >= WIFI_FULL_RECONNECT_AFTER_IDX) {
                // WiFi.reconnect() has now failed WIFI_FULL_RECONNECT_AFTER_IDX
                // times without success. Switch to the heavy path used by the
                // bootstrap phase (lines ~300-301): disconnect+begin forces a full
                // re-association negotiation. WiFi.reconnect() silently fails after
                // BEACON_TIMEOUT (complete router power loss + return) — this is
                // the fix for devices that stay powered while the router cycles.
                LOG_W("Loop", "WiFi.reconnect() failed %u times — switching to "
                              "disconnect+begin (BEACON_TIMEOUT workaround)",
                      (unsigned)_wifiBackoffIdx);
                WiFi.disconnect(true);
                delay(100);
                WiFi.begin(activeBundle.wifi_ssid, activeBundle.wifi_password);
            } else {
                WiFi.reconnect();   // sufficient for brief blips (< ~105 s)
            }
            _wifiNextAttemptMs = millis() + waitMs;
            _wifiBackoffIdx    = wifiBackoffAdvance(_wifiBackoffIdx,
                                                    (uint8_t)WIFI_BACKOFF_STEPS_COUNT);

            // Auth-fail fast path — but only once we've seen at least one
            // failed attempt (reason was captured on the disconnect event).
            if (wifiReasonIsAuthFail(wifiLastDisconnectReason)) {
                if (++_wifiAuthFailCycles >= WIFI_AUTH_FAIL_CYCLES) {
                    LOG_W("Loop", "WiFi disconnect reason %u suggests bad credentials "
                                  "across %u cycles — flagging stale and restarting",
                          (unsigned)wifiLastDisconnectReason,
                          (unsigned)_wifiAuthFailCycles);
                    ledSetPattern(LedPattern::ERROR);
                    CredentialStore::setCredStale(true);
                    CredentialStore::incrementRestartCount(RestartReason::CRED_BAD);
                    // (v0.3.36) Intentional final-drain delay: gives the
                    // log line above + the NVS write of the cred-stale flag
                    // time to flush before the reboot wipes RAM. Only ever
                    // executes on the path to ESP.restart(), so the brief
                    // starvation of BLE/ranging is bounded and one-shot.
                    delay(500);
                    ESP.restart();
                }
            } else {
                _wifiAuthFailCycles = 0;   // non-auth reason clears the streak
            }
            return;
        }
    }

    // ── MQTT heartbeat ────────────────────────────────────────────────────────
    mqttHeartbeat();

    // (#76 sub-D) After MQTT_LOOP_HEALTHY_UPTIME_MS of stable connectivity,
    // push an "operational" marker to RestartHistory so any pre-existing
    // mqtt_unrecoverable streak gets broken. Idempotent.
    mqttMarkHealthyIfDue();

    // ── Core-dump publish (#65, v0.4.17) ──────────────────────────────────────
    // Once per boot, after MQTT is up, drain any stored core dump by publishing
    // its summary (PC, exception cause, faulting task, backtrace) to
    // .../diag/coredump retained QoS 1, then erase. coredumpPublishIfAny() is
    // idempotent — second call is a no-op once erased. Cheap to call every loop.
    if (mqttIsConnected()) {
        coredumpPublishIfAny();
    }

    // ── MQTT self-heal ────────────────────────────────────────────────────────
    // (v0.4.15, 2026-04-27) Hung-watchdog NO LONGER calls ESP.restart().
    // ESP.restart() from this branch raced with AsyncTCP's pending error
    // handler and produced the LoadStoreAlignment / StoreProhibited panic in
    // tcp_arg() — the actual cascade signature seen on every long-outage M3
    // run. The reconnect timer in mqtt_client.h retries forever (capped at
    // 60 s backoff); we don't need a hard restart to recover. We force a
    // clean disconnect so the next reconnect starts from a fresh PCB and
    // clear the watchdog so this log doesn't spam every loop tick.
    // See SUGGESTED_IMPROVEMENTS #65.
    if (mqttIsHung()) {
        LOG_W("Loop", "MQTT hung (no callback in %u ms) — force-disconnect, timer retries",
              (unsigned)MQTT_HUNG_TIMEOUT_MS);
        ledSetPattern(LedPattern::ERROR);
        mqttForceDisconnect();   // releases stuck async-client state, clears _mqttConnectStartMs
    } else if (mqttFailCount() >= MQTT_RESTART_THRESHOLD ||
               mqttDisconnectedDurationMs() >= MQTT_UNRECOVERABLE_TIMEOUT_MS) {
        // (#65 Phase 2, v0.4.19) Was unconditional ESP.restart() — now defer
        // 300 ms via mqttScheduleRestart() so the diagnostic JSON publish
        // drains and the dashboard sees WHY this device just decided to
        // self-restart (fail count, last disconnect reason, RSSI).
        // (#76 sub-C, v0.4.24) Trigger now also fires on time — 10 min
        // continuous outage is enough regardless of fail count.
        LOG_W("Loop", "MQTT unrecoverable (fails=%d, outage=%lu ms) — scheduling restart",
              (int)mqttFailCount(),
              (unsigned long)mqttDisconnectedDurationMs());
        ledSetPattern(LedPattern::ERROR);
        CredentialStore::setCredStale(true);
        CredentialStore::incrementRestartCount();
        mqttScheduleRestart("mqtt_unrecoverable", 300);
    } else if (mqttNeedsRediscovery()) {
        mqttClearRediscoveryFlag();
        ledSetPattern(LedPattern::WIFI_CONNECTING);
        LOG_W("Loop", "MQTT stuck — re-running broker discovery");
        BrokerResult broker = discoverBroker(activeBundle.mqtt_broker_url);
        if (broker.found()) saveBrokerToCache(broker.host, broker.port);
        mqttReinit(broker);
    }

    // ── OTA version check ─────────────────────────────────────────────────────
    otaLoop();

    // (v0.3.34) Validate / roll back if we're inside the post-OTA window.
    // No-op outside the window (and on boots where the partition wasn't in
    // PENDING_VERIFY state to begin with).
    otaValidationTick();

    // ── ESP-NOW ranging ───────────────────────────────────────────────────────
    espnowRangingLoop();

#ifdef RFID_ENABLED
    rfidLoop();
#endif
#ifdef BLE_ENABLED
    bleLoop();
#endif
#ifdef HALL_ENABLED
    hallLoop();   // (v0.5.0) periodic ADC1 read + telemetry publish
#endif

    settingsServerTick();

    delay(100);
}
