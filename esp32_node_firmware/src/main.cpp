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
//   └─────────────────────────────────────────────────────────────────────┘
//
//   AP_MODE  (entered from any state on unrecoverable failure):
//   │  Hosts a Wi-Fi access point + HTTP portal for manual configuration.  │
//   │  NEVER returns — calls ESP.restart() after credentials are saved.    │
//
// BOOT SEQUENCE (inside setup()):
//   1. Serial.begin()
//   2. WiFi.mode(WIFI_STA) + DeviceId::init()   — generate/load persistent UUID
//   3. AppConfigStore::load()                   — load GitHub + MQTT topic settings
//   4. State machine: BOOT → BOOTSTRAP/WIFI_CONNECT → OPERATIONAL or AP_MODE
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

#include "config.h"
#include "logging.h"
#include "led.h"
#include "credentials.h"
#include "crypto.h"
#include "espnow_bootstrap.h"
#include "espnow_responder.h"
#include "ap_portal.h"
#include "ws2812.h"        // WS2812B strip — before mqtt_client.h and rfid.h
#include "mqtt_client.h"   // MUST come before ota.h — defines mqttPublishStatus()
#include "ota.h"
#include "espnow_ranging.h"  // MUST come after mqtt_client.h — uses mqttPublish/mqttConnected
#include "rfid.h"          // MUST come after mqtt_client.h AND ws2812.h
#ifdef BLE_ENABLED
#include "ble.h"           // MUST come last — uses mqttPublish from mqtt_client.h
#endif


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


// ── WiFiEvent ─────────────────────────────────────────────────────────────────
// Arduino Wi-Fi event handler — called asynchronously by the Wi-Fi stack.
// We only care about two events: got IP (connected) and disconnected.
void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            // DHCP assigned an IP address — we are fully connected
            LOG_I("WiFi", "Connected - IP: %s", WiFi.localIP().toString().c_str());
            wifiConnected = true;
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            // Lost the connection — could be a temporary blip or a permanent drop
            if (wifiConnected) {
                // Only log when transitioning from connected → disconnected.
                // Suppresses repeated events fired by the WiFi stack during
                // failed association attempts (where we were never connected).
                LOG_I("WiFi", "Disconnected");
            }
            wifiConnected = false;
            break;

        default:
            break;   // Ignore all other events (SCAN_DONE, AP events, etc.)
    }
}


// ── connectWifi ───────────────────────────────────────────────────────────────
// Starts a Wi-Fi connection attempt and blocks until either an IP is assigned
// or WIFI_CONNECT_TIMEOUT_MS elapses.
// Returns true if connected, false if timed out.
static bool connectWifi(const CredentialBundle& b) {
    WiFi.disconnect(true);         // Ensure any previous connection is fully torn down
    WiFi.mode(WIFI_STA);           // Station mode (client, not access point)
    WiFi.onEvent(WiFiEvent);       // Register the event handler for connect/disconnect events
    WiFi.begin(b.wifi_ssid, b.wifi_password);  // Start the association process

    LOG_I("WiFi", "Connecting to SSID: %s", b.wifi_ssid);

    // Poll wifiConnected (set by WiFiEvent) every 100 ms until connected or timeout
    uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
    while (!wifiConnected && millis() < deadline) {
        delay(100);   // Yield to the Wi-Fi task while waiting
    }
    return wifiConnected;
}


// =============================================================================
// setup() — runs once at boot
// Drives the state machine from BOOT through to OPERATIONAL (or AP_MODE).
// setup() only returns if currentState == OPERATIONAL at the end.
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);   // Give the serial monitor time to connect before the first print
    ledInit();                          // Start LED timer — must be early so all states can blink
    ledSetPattern(LedPattern::BOOT);    // Solid ON during initialisation
    ws2812Init();                       // WS2812B strip: FastLED setup, LEDs off, create event queue
    ws2812TaskStart();                  // Spawn strip task on Core 1 — boot animations from here
    Serial.println();
    LOG_I("BOOT", "ESP32 Credential Bootstrap Firmware v" FIRMWARE_VERSION);

    // Initialise the persistent device UUID (stored in NVS namespace "esp32id").
    // Must run before any MQTT topic building or status publishing.
    // On first boot, generates and saves a new UUID.
    // On subsequent boots (including after OTA updates), loads the existing UUID.
    WiFi.mode(WIFI_STA);   // MAC address readable only after Wi-Fi mode is set
    DeviceId::init();

    // Load GitHub owner/repo and MQTT ISA-95 topic segments from NVS into gAppConfig.
    // Any field not yet saved in NVS falls back to the config.h compile-time default.
    // Must run BEFORE mqttBegin() (reads gAppConfig for topic building) and
    // BEFORE otaCheckNow() (reads gAppConfig.ota_json_url).
    AppConfigStore::load();


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: BOOT
    // Check whether credentials were flagged stale by a previous boot cycle
    // (written by loop() before a sustained-failure restart). If so, force a
    // sibling re-verify even if admin credentials are present — they may no
    // longer be valid after a network change.
    //
    // Otherwise, if admin credentials exist, skip bootstrap for a fast re-boot.
    // If no credentials of any kind exist, go to BOOTSTRAP_REQUEST to ask siblings.
    // ─────────────────────────────────────────────────────────────────────────
    {
        bool credStale = CredentialStore::isCredStale();
        if (credStale) {
            // Clear the flag now — this boot is the re-verify attempt.
            // If sibling bootstrap fails, the device falls back to its stored
            // credentials or AP mode as normal; the flag will not re-trigger.
            CredentialStore::setCredStale(false);
            LOG_W("BOOT", "Credentials flagged stale - forcing sibling re-verify");
            currentState = State::BOOTSTRAP_REQUEST;
        } else if (CredentialStore::hasPrimary()) {
            // Admin credentials are in NVS and not stale — load them
            LOG_I("BOOT", "Admin credentials found - skipping bootstrap");
            if (CredentialStore::load(activeBundle)) {
                currentState = State::WIFI_CONNECT;   // Go straight to Wi-Fi
            } else {
                // NVS said admin credentials exist but they couldn't be loaded — corrupted?
                LOG_E("BOOT", "Failed to load admin credentials - entering AP mode");
                currentState = State::AP_MODE;
            }
        } else {
            // No admin credentials — need to ask a sibling node for them
            currentState = State::BOOTSTRAP_REQUEST;
        }
    }


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: BOOTSTRAP_REQUEST
    // Broadcast an ESP-NOW credential request and wait for a sibling to respond.
    // Each attempt waits up to BOOTSTRAP_TIMEOUT_MS before trying again.
    // If no sibling responds after BOOTSTRAP_MAX_ATTEMPTS, fall back to
    // any existing NVS bundle, or if there is none, go to AP_MODE.
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::BOOTSTRAP_REQUEST) {
        LOG_I("BOOT", "Starting ESP-NOW bootstrap");
        ledSetPattern(LedPattern::WIFI_CONNECTING);   // 500/500 blink — searching for sibling
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "bootstrap", sizeof(e.animName)); ws2812PostEvent(e); }
        WiFi.mode(WIFI_STA);   // ESP-NOW requires STA mode even before AP association

        bool gotBundle = false;   // True once we have valid credentials to use

        // Timestamp safety cap — reject bundles from rogue nodes advertising a
        // far-future timestamp that would permanently win all comparisons.
        const uint64_t BOOTSTRAP_MAX_TS = FIRMWARE_BUILD_TIMESTAMP + SIBLING_TS_MAX_FUTURE_S;

        for (int attempt = 1;
             attempt <= BOOTSTRAP_MAX_ATTEMPTS && !gotBundle;
             attempt++)
        {
            LOG_I("BOOT", "Bootstrap attempt %d of %d", attempt, BOOTSTRAP_MAX_ATTEMPTS);

            CredentialBundle received;
#ifdef SIBLING_PRIMARY_SELECTION
            bool gotResp = espnowBootstrapWithPrimarySelection(received);
#else
            bool gotResp = espnowBootstrap(received);
#endif
            if (gotResp) {
                // Reject bundles with an unreasonably far-future timestamp
                if (received.timestamp > BOOTSTRAP_MAX_TS) {
                    LOG_W("BOOT", "Received bundle timestamp %llu exceeds cap %llu - discarding",
                          received.timestamp, BOOTSTRAP_MAX_TS);
                    // Treat as if no response arrived for this attempt
                } else {
                // A sibling responded — compare with whatever is already in NVS
                // and keep whichever bundle has the newer timestamp.
                CredentialBundle stored;
                bool hasStored = CredentialStore::load(stored);

                // Use the received bundle if:
                //   - We have nothing stored yet, OR
                //   - The received bundle is strictly newer.
                // When timestamps are equal, keep the stored bundle (conservative default).
                bool useReceived = !hasStored
                    || received.timestamp > stored.timestamp
                    || (received.timestamp == stored.timestamp
                        && memcmp(WiFi.macAddress().c_str(),
                                  "00:00:00:00:00:00", 6) < 0);  // always false — stored wins on tie

                if (useReceived) {
                    activeBundle = received;
                    CredentialStore::save(activeBundle);   // Persist for next boot
                    LOG_I("BOOT", "New bundle adopted and saved");
                } else {
                    activeBundle = stored;
                    LOG_I("BOOT", "Kept existing NVS bundle (newer timestamp)");
                }
                gotBundle = true;   // We have credentials — exit the retry loop
                } // end timestamp cap else

            } else {
                // No response this attempt
                if (attempt < BOOTSTRAP_MAX_ATTEMPTS) {
                    LOG_I("BOOT", "No response - retrying in 2 s");
                    // TODO(v0.3.07): remove when bootstrap becomes non-blocking state machine
                    delay(2000);
                }
            }
        }

        if (!gotBundle) {
            // All bootstrap attempts failed — fall back to anything in NVS
            if (CredentialStore::load(activeBundle)) {
                LOG_I("BOOT", "No sibling found - using stored NVS bundle");
                currentState = State::WIFI_CONNECT;
            } else {
                // Nothing in NVS either — no choice but to enter AP mode
                LOG_W("BOOT", "No credentials available - entering AP mode");
                currentState = State::AP_MODE;
            }
        } else {
            currentState = State::WIFI_CONNECT;
        }
    }


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: WIFI_CONNECT
    // Try to associate with the Wi-Fi router using the credentials in activeBundle.
    // If all attempts fail, increment the restart counter and reboot.
    // If the restart counter reaches DEVICE_RESTART_MAX, enter AP_MODE instead
    // (prevents an infinite restart loop if credentials are wrong).
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::WIFI_CONNECT) {
        ledSetPattern(LedPattern::WIFI_CONNECTING);   // 500/500 blink — associating with router
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "wifi", sizeof(e.animName)); ws2812PostEvent(e); }
        bool connected = false;

        for (int attempt = 1;
             attempt <= WIFI_MAX_ATTEMPTS && !connected;
             attempt++)
        {
            LOG_I("WiFi", "Association attempt %d of %d", attempt, WIFI_MAX_ATTEMPTS);
            connected = connectWifi(activeBundle);
            if (!connected && attempt < WIFI_MAX_ATTEMPTS) {
                // TODO(v0.3.07): replace with non-blocking retry when boot path is refactored
                delay(2000);
            }
        }

        if (!connected) {
            // All Wi-Fi attempts failed. Before incrementing the restart counter,
            // ask siblings for fresh credentials — they may have a newer bundle
            // (e.g. after an admin rotated credentials on the network).
            // WiFi is NOT connected here so it is safe to change the channel.
            LOG_W("WiFi", "All attempts failed - trying sibling credential re-verify");

            // Timestamp safety cap (same rule as BOOTSTRAP_REQUEST above)
            const uint64_t REVERIFY_MAX_TS = FIRMWARE_BUILD_TIMESTAMP + SIBLING_TS_MAX_FUTURE_S;

            for (int sibAttempt = 0;
                 sibAttempt < SIBLING_REVERIFY_ATTEMPTS && !connected;
                 sibAttempt++)
            {
                CredentialBundle fresh;
#ifdef SIBLING_PRIMARY_SELECTION
                bool gotFresh = espnowBootstrapWithPrimarySelection(fresh);
#else
                bool gotFresh = espnowBootstrap(fresh);
#endif
                if (gotFresh) {
                    if (fresh.timestamp > REVERIFY_MAX_TS) {
                        LOG_W("WiFi", "Sibling bundle timestamp %llu exceeds cap - discarding",
                              fresh.timestamp);
                    } else if (fresh.timestamp > activeBundle.timestamp) {
                        // Strictly newer credentials received — adopt and retry WiFi
                        activeBundle = fresh;
                        CredentialStore::save(activeBundle);
                        LOG_I("WiFi", "Fresh credentials from sibling - retrying WiFi");

                        for (int ra = 1; ra <= WIFI_MAX_ATTEMPTS && !connected; ra++) {
                            LOG_I("WiFi", "Re-verify attempt %d of %d", ra, WIFI_MAX_ATTEMPTS);
                            connected = connectWifi(activeBundle);
                            if (!connected && ra < WIFI_MAX_ATTEMPTS) {
                                // TODO(v0.3.07): replace with non-blocking retry
                                delay(2000);
                            }
                        }
                        if (connected) {
                            CredentialStore::clearRestartCount();
                            ledSetPattern(LedPattern::WIFI_CONNECTED);   // slow blink — waiting for MQTT
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
                // All attempts exhausted including sibling re-verify
                uint8_t restarts = CredentialStore::incrementRestartCount();
                LOG_W("WiFi", "Failed - restart count now %d / %d", restarts, DEVICE_RESTART_MAX);

                if (restarts < DEVICE_RESTART_MAX) {
                    // Still within the restart budget — reboot and try again
                    LOG_W("WiFi", "Restarting device...");
                    ledSetPattern(LedPattern::ERROR);   // 3× flash — visible during 1s delay
                    delay(1000);    // Brief pause so the serial message is transmitted
                    ESP.restart();  // Full hardware restart
                } else {
                    // Restart budget exhausted — credentials may be wrong or router is down.
                    // Enter AP_MODE so the admin can update credentials.
                    LOG_W("WiFi", "Restart limit reached - entering AP mode");
                    currentState = State::AP_MODE;
                }
            }
        } else {
            // Successfully connected — reset the restart counter
            CredentialStore::clearRestartCount();
            ledSetPattern(LedPattern::WIFI_CONNECTED);   // slow blink — waiting for MQTT
            currentState = State::OPERATIONAL;
        }
    }


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: AP_MODE
    // Host a Wi-Fi access point and HTTP configuration portal.
    // This call NEVER returns — apPortalStart() runs its own blocking loop
    // and calls ESP.restart() after the admin saves credentials.
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::AP_MODE) {
        ledSetPattern(LedPattern::AP_MODE);   // rapid 100/100 blink — config portal active
        { LedEvent e{}; e.type = LedEventType::BOOT_STATE;
          strlcpy(e.animName, "ap_mode", sizeof(e.animName)); ws2812PostEvent(e); }
        apPortalStart();
        // Execution does not reach here
    }


    // ─────────────────────────────────────────────────────────────────────────
    // STATE: OPERATIONAL
    // We are connected to Wi-Fi. Start all application services:
    //   1. ESP-NOW responder — other nodes can now bootstrap from us
    //   2. MQTT — connect to the broker and begin messaging
    //
    // After setup() returns, loop() takes over for ongoing tasks.
    // ─────────────────────────────────────────────────────────────────────────
    if (currentState == State::OPERATIONAL) {
        LOG_I("BOOT", "Wi-Fi connected - starting services");

        // Register our credentials with the ESP-NOW responder so that any
        // new node that asks us for credentials gets the active bundle
        espnowResponderSetBundle(activeBundle);
        espnowResponderStart();   // Start listening for incoming ESP-NOW requests

        // Initialise health flags for sibling health advertisements.
        // WiFi is confirmed connected (we are here). GitHub is assumed reachable
        // until the first OTA check proves otherwise (~1 hour). MQTT starts at 0
        // and is set by onMqttConnect once the broker responds.
        responderSetHealthFlag(0, true);   // bit 0: WiFi connected
        responderSetHealthFlag(2, true);   // bit 2: GitHub assumed reachable

        // Discover the MQTT broker using mDNS, then port scan, then stored URL.
        // discoverBroker() returns the best address found by any method.
        // The result is passed directly to mqttBegin() — the NVS bundle is
        // not modified, so discovery only affects this session.
        BrokerResult broker = discoverBroker(activeBundle.mqtt_broker_url);

        // Persist the discovered address so next boot can skip mDNS/port scan
        if (broker.found()) {
            saveBrokerToCache(broker.host, broker.port);
            // Share with ESP-NOW responder so siblings can ask us for the broker
            espnowResponderSetBroker(broker.host, broker.port);
        }

        // Strip returns to idle blue breathing — OPERATIONAL state reached
        { LedEvent e{}; e.type = LedEventType::RESET; ws2812PostEvent(e); }

        // Connect to the MQTT broker — non-blocking; result arrives via callbacks
        mqttBegin(activeBundle, broker);

#ifdef RFID_ENABLED
        // Initialise the MFRC522 reader over SPI. Called here (after Wi-Fi connects and
        // ESP-NOW channel scanning is complete) so SPI is free from radio contention.
        rfidInit();
#endif

#ifdef BLE_ENABLED
        // Initialise BLE scanner. Called after Wi-Fi is up — BLE and WiFi share the
        // 2.4 GHz radio and ESP-IDF handles the coexistence automatically.
        bleInit();
#endif
    }
}


// =============================================================================
// loop() — runs repeatedly after setup() returns
// Only active when currentState == OPERATIONAL. All other states either never
// reach loop() (AP_MODE blocks in setup) or the device has restarted.
// =============================================================================
void loop() {
    // Guard: if setup() didn't reach OPERATIONAL for any reason, do nothing
    if (currentState != State::OPERATIONAL) return;

    // ── Wi-Fi health check ────────────────────────────────────────────────────
    // If the Wi-Fi connection dropped (wifiConnected set to false by WiFiEvent),
    // attempt reconnection without blocking the rest of the loop.
    //
    // State is tracked across loop() iterations via static variables:
    //   _wifiRecoveryAttempts — how many reconnects have been tried this outage
    //   _wifiRetryAt          — millis() timestamp before which we do not retry
    //
    // Note: connectWifi() still blocks for up to WIFI_CONNECT_TIMEOUT_MS per
    // call. Full non-blocking WiFi recovery (state machine driven from loop())
    // is deferred to v0.3.07 along with the ESP-NOW channel scan refactor.
    if (!wifiConnected) {
        static int      _wifiRecoveryAttempts = 0;
        static uint32_t _wifiRetryAt          = 0;

        if (_wifiRecoveryAttempts == 0) {
            // First check after losing connection — log and start the recovery sequence
            LOG_W("Loop", "Wi-Fi lost - attempting reconnect");
            ledSetPattern(LedPattern::WIFI_CONNECTING);   // 500/500 — reconnecting
        }

        if (millis() < _wifiRetryAt) {
            // Still in the inter-attempt pause — return so other loop tasks can run
            return;
        }

        connectWifi(activeBundle);
        _wifiRecoveryAttempts++;

        if (wifiConnected) {
            // Reconnected successfully — clear recovery state
            _wifiRecoveryAttempts = 0;
            _wifiRetryAt          = 0;
            // AsyncMqttClient will automatically reconnect to the broker
            // via its own reconnect timer — no explicit call needed here.
        } else if (_wifiRecoveryAttempts >= WIFI_MAX_ATTEMPTS) {
            // All attempts exhausted — flag credentials as stale so next boot
            // forces a sibling re-verify before falling back to AP mode.
            LOG_W("Loop", "Could not reconnect - flagging credentials stale and restarting");
            ledSetPattern(LedPattern::ERROR);   // 3× flash before restart
            CredentialStore::setCredStale(true);
            CredentialStore::incrementRestartCount();   // Track this failure
            ESP.restart();
        } else {
            // Schedule the next attempt with a 3-second pause.
            // Return to loop() so MQTT heartbeat and OTA tasks can still run.
            _wifiRetryAt = millis() + 3000;
            return;
        }
    }

    // ── MQTT heartbeat ────────────────────────────────────────────────────────
    // Publishes a heartbeat status message every HEARTBEAT_INTERVAL_MS.
    // mqttHeartbeat() checks the elapsed time internally and returns immediately
    // if the interval has not elapsed yet — safe to call every loop tick.
    mqttHeartbeat();

    // ── MQTT self-heal ────────────────────────────────────────────────────────
    // Hung watchdog: if connect() was called but no callback (success or failure)
    // arrived within MQTT_HUNG_TIMEOUT_MS the AsyncMqttClient TCP layer has
    // silently stalled — restart immediately.
    // Tier 2: hard restart once MQTT_RESTART_THRESHOLD consecutive failures hit.
    // Tier 1: re-run broker discovery at MQTT_REDISCOVERY_THRESHOLD failures.
    if (mqttIsHung()) {
        LOG_W("Loop", "MQTT hung (no callback) - restarting device");
        ledSetPattern(LedPattern::ERROR);
        CredentialStore::incrementRestartCount();
        ESP.restart();
    } else if (mqttFailCount() >= MQTT_RESTART_THRESHOLD) {
        // Sustained MQTT failure — credentials may be wrong (wrong broker URL,
        // bad username/password). Flag as stale so next boot asks siblings.
        LOG_W("Loop", "MQTT unrecoverable - flagging credentials stale and restarting");
        ledSetPattern(LedPattern::ERROR);
        CredentialStore::setCredStale(true);
        CredentialStore::incrementRestartCount();
        ESP.restart();
    } else if (mqttNeedsRediscovery()) {
        mqttClearRediscoveryFlag();
        ledSetPattern(LedPattern::WIFI_CONNECTING);   // re-running discovery
        LOG_W("Loop", "MQTT stuck - re-running broker discovery");
        BrokerResult broker = discoverBroker(activeBundle.mqtt_broker_url);
        if (broker.found()) saveBrokerToCache(broker.host, broker.port);
        mqttReinit(broker);
    }

    // ── OTA version check ─────────────────────────────────────────────────────
    // Checks GitHub for a newer firmware release every OTA_CHECK_INTERVAL_MS,
    // or immediately if an MQTT ota_check command was received.
    // otaLoop() returns quickly if the interval hasn't elapsed.
    otaLoop();

    // ── ESP-NOW ranging ───────────────────────────────────────────────────────
    // Broadcasts a periodic ranging beacon and publishes peer RSSI/distance to
    // MQTT topic .../espnow every ESPNOW_MQTT_PUBLISH_MS.
    espnowRangingLoop();

#ifdef RFID_ENABLED
    rfidLoop();             // Poll for RFID card scans; publishes UID to .../telemetry
#endif

#ifdef BLE_ENABLED
    bleLoop();              // BLE scan trigger, tracked beacon MQTT publish (2 s), serial print (10 s)
#endif

    settingsServerTick();   // Handle HTTP requests on the settings portal (no-op
                            // when portal is not active — returns false immediately)

    delay(100);   // Yield for 100 ms — prevents watchdog timeout and keeps CPU free
                  // for the Wi-Fi and MQTT background tasks running on core 0
}
