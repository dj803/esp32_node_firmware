#pragma once

// =============================================================================
// config.h  —  ESP32 Credential Bootstrap Firmware
//
// PURPOSE: Every deployment-specific value lives here. No magic numbers
// anywhere else in the codebase. Change this file before flashing each
// production deployment.
// =============================================================================


// -----------------------------------------------------------------------------
// Firmware identity
// -----------------------------------------------------------------------------
// FIRMWARE_VERSION must match the GitHub release tag exactly, minus the
// leading 'v'. E.g. tag "v1.2.3" → "1.2.3". The OTA system compares this
// string to the latest GitHub release to decide whether to update.
//
// FIRMWARE_BUILD_TIMESTAMP is a Unix epoch value (seconds since 1970-01-01).
// It is embedded in every credential bundle created via AP mode, allowing
// newer builds to win timestamp comparisons against older ones.
// The GitHub Actions workflow overwrites both values automatically when
// building a release — you only need to update them manually for local builds.
//
// Quick way to get the current Unix time:
//   Linux/Mac:  date +%s
//   Windows:    PowerShell: [int][double]::Parse((Get-Date -UFormat %s))
// -----------------------------------------------------------------------------
#define FIRMWARE_VERSION           "0.3.01"
#define FIRMWARE_BUILD_TIMESTAMP   1745229600ULL   // 2026-04-21 10:40:00 UTC


// -----------------------------------------------------------------------------
// Bootstrap / retry tuning
// These values control how aggressively the device tries to get credentials
// before giving up and falling back to AP mode.
// -----------------------------------------------------------------------------
#define BOOTSTRAP_TIMEOUT_MS       10000   // How long (ms) to wait for a sibling's
                                           // encrypted credential response per attempt.
                                           // Increase if siblings are slow to respond.

#define BOOTSTRAP_MAX_ATTEMPTS     3       // Number of ESP-NOW credential requests to
                                           // broadcast before giving up. Each attempt
                                           // waits BOOTSTRAP_TIMEOUT_MS.

#define WIFI_CONNECT_TIMEOUT_MS    15000   // How long (ms) to wait for the Wi-Fi
                                           // router to grant an IP address per attempt.

#define WIFI_MAX_ATTEMPTS          3       // Wi-Fi association attempts per boot cycle
                                           // before declaring connection failure.

#define DEVICE_RESTART_MAX         3       // How many full device restarts are allowed
                                           // before the firmware stops restarting and
                                           // falls through to AP mode instead.
                                           // Prevents an infinite restart loop when
                                           // credentials are wrong or the router is down.


// -----------------------------------------------------------------------------
// Runtime intervals
// These are used during normal OPERATIONAL state, not during boot.
// -----------------------------------------------------------------------------
#define HEARTBEAT_INTERVAL_MS      60000   // How often (ms) the device publishes a
                                           // heartbeat message to its MQTT status topic.
                                           // Node-RED uses this to detect offline nodes.

#define MQTT_RECONNECT_MAX_MS      60000   // Maximum delay (ms) between MQTT reconnect
                                           // attempts. The delay doubles after each
                                           // failure (exponential back-off) up to this cap.

#define MQTT_REDISCOVERY_THRESHOLD     5   // Re-run broker discovery after this many
                                           // consecutive MQTT failures (Tier 1 self-heal).
#define MQTT_RESTART_THRESHOLD        10   // Hard-restart the device after this many
                                           // consecutive failures (Tier 2 self-heal).
#define MQTT_HUNG_TIMEOUT_MS       12000   // If connect() produces no callback (success or
                                           // failure) within this window, the async client
                                           // has hung — restart the device.

#define OTA_CHECK_INTERVAL_MS      3600000 // How often (ms) the device polls GitHub
                                           // Releases for a newer firmware version.
                                           // Default: 1 hour (3 600 000 ms).
                                           // You can also trigger an immediate check
                                           // via MQTT: publish to cmd/ota_check.


// -----------------------------------------------------------------------------
// OTA JSON URL
// Stable URL of the JSON filter file hosted on GitHub Pages (gh-pages branch).
// The CI workflow overwrites this file on every tagged release, pointing to the
// new firmware.bin asset. The URL itself never changes between versions.
// Replace "myorg" and "esp32-firmware" with your actual GitHub owner and repo.
// This default can be overridden at runtime via the AP portal / settings form.
// -----------------------------------------------------------------------------
#define OTA_JSON_URL               "https://myorg.github.io/esp32-firmware/ota.json"


// -----------------------------------------------------------------------------
// MQTT topic hierarchy  (ISA-95 / Unified Namespace pattern)
//
// Full topic format:
//   [Enterprise]/[Site]/[Area]/[Line]/[Cell]/[DeviceType]/[DeviceId]/[Prefix]
//
// DeviceId is NOT defined here — it is built at runtime from the last 3 bytes
// of the device's MAC address (6 uppercase hex chars, e.g. "A3F2C1").
//
// Example full topic:
//   Acme/JohannesburgPlant/Warehouse/Line1/Cell3/ESP32Sensor/A3F2C1/status
//
// These segments are designed for Node-RED's Unified Namespace pattern.
// Change these values to match your site topology.
// -----------------------------------------------------------------------------
#define MQTT_ENTERPRISE            "Enigma"             // Top-level org / company name
#define MQTT_SITE                  "JHBDev"             // Physical site or factory
#define MQTT_AREA                  "Office"             // Zone within the site
#define MQTT_LINE                  "Line"               // Production line or logical group
#define MQTT_CELL                  "Cell"               // Work cell or sub-area
#define MQTT_DEVICE_TYPE           "ESP32NodeBox"       // Class of this device


// -----------------------------------------------------------------------------
// AP mode portal
// When no credentials are available (first boot, or all retries failed),
// the device creates a Wi-Fi access point so an admin can configure it.
// -----------------------------------------------------------------------------
#define AP_SSID_PREFIX             "ESP32-Config-"  // SSID = prefix + last 4 hex of MAC
                                                    // e.g. "ESP32-Config-F2C1"
#define AP_PASSWORD                "password"       // Wi-Fi password for the AP network.
                                                    // Change this for production deployments.
#define AP_USERNAME                "admin"          // Shown as a label on the config page
                                                    // (informational only — no HTTP auth)
#define AP_LOCAL_IP                "192.168.4.1"    // Default IP assigned by ESP32 AP mode


// -----------------------------------------------------------------------------
// RFID reader (MFRC522v2 via SPI)
// Define RFID_ENABLED to activate the RFID module. Comment out on nodes
// that do not have an MFRC522 reader attached — no code overhead either way.
// -----------------------------------------------------------------------------
#define RFID_ENABLED                    // Comment out to disable on reader-less nodes

#define RFID_SS_PIN        5            // SPI Slave Select GPIO
                                        // SCK=18, MISO=19, MOSI=23 (default ESP32 VSPI)
#define RFID_RST_PIN       22           // RST (NRSTPD) GPIO — pulsed LOW→HIGH in rfidInit()
                                        // to guarantee a clean hardware reset on every boot
#define RFID_IRQ_PIN       4            // IRQ GPIO (active LOW, open-drain; uses INPUT_PULLUP)
                                        // Fires when a card sends ATQA in response to REQA
#define RFID_REARM_MS           150     // Re-issue REQA every N ms when no card is present
                                        // REQA times out in ~25 ms; 150 ms gives ~6 checks/sec
#define RFID_POST_READ_QUIET_MS 300     // After a read (success or fail), suppress re-arming
                                        // for this long to let the card fully enter HALT state
                                        // and discard the spurious IRQ it can cause. Must be
                                        // > RFID_REARM_MS so re-arming resumes naturally.
#define RFID_DEBOUNCE_MS        2000    // Min ms between publishes for the same card UID
                                        // Different cards are always processed immediately


// -----------------------------------------------------------------------------
// Status LED
// -----------------------------------------------------------------------------
#define STATUS_LED_PIN             2    // GPIO 2 = onboard LED on most ESP32 dev boards
                                        // (active HIGH — HIGH = on, LOW = off)


// -----------------------------------------------------------------------------
// NVS (Non-Volatile Storage) namespace
// All persistent data (credentials, restart counter) is stored under this
// namespace using the Arduino Preferences library. Changing this value will
// cause the device to lose any previously stored credentials.
// -----------------------------------------------------------------------------
#define NVS_NAMESPACE              "esp32cred"   // Credential storage namespace
#define DEVICE_ID_NVS_NAMESPACE    "esp32id"     // Device UUID namespace (separate from credentials
                                                 // so credential wipes do not destroy device identity)
#define BROKER_CACHE_NVS_NAMESPACE "esp32disc"   // Last 2 known broker addresses (host + port)


// -----------------------------------------------------------------------------
// MQTT Broker Discovery
// The firmware tries three methods in order before giving up:
//   1. mDNS  — looks for a service advertised as _mqtt._tcp on the LAN
//              (requires Avahi on Linux or Bonjour on Windows on the broker host)
//   2. Port scan — probes each host on the local /24 subnet for open port 1883
//              (works with stock Mosquitto, no broker-side config required)
//   3. Stored URL — uses the mqtt_broker_url from the NVS credential bundle
//              (manually entered via the AP portal — always the final fallback)
// Set BROKER_DISCOVERY_ENABLED to 0 to skip steps 1 and 2 and always use the
// stored URL (useful if discovery causes problems on your network).
// -----------------------------------------------------------------------------
#define BROKER_DISCOVERY_ENABLED    1       // 1 = try mDNS + port scan; 0 = stored URL only

#define MDNS_DISCOVERY_TIMEOUT_MS   3000    // How long to wait for mDNS responses
#define MDNS_SERVICE_TYPE           "mqtt"  // _mqtt._tcp service type (do not change)
#define MDNS_SERVICE_PROTO          "tcp"   // Protocol component of _mqtt._tcp (do not change)

#define PORTSCAN_PORT               1883    // TCP port to probe during subnet scan
#define PORTSCAN_TIMEOUT_MS         75      // Per-host TCP connect timeout (ms)
                                            // 75 ms × 254 hosts = ~19 s worst case
                                            // Hosts that respond immediately are much faster
                                            // Increase to 200 ms on slow/congested networks
#define PORTSCAN_MAX_RESULTS        4       // Stop scanning after this many open ports found
                                            // (avoids waiting for the full subnet if broker found early)


// -----------------------------------------------------------------------------
// ESP-NOW protocol constants
// Changing ESPNOW_PROTOCOL_VERSION will make this firmware incompatible with
// nodes running an older version — increment it whenever the message layout
// changes so that mismatched nodes reject each other's packets gracefully.
//
// Channel note: ESP-NOW and Wi-Fi share the same radio. In OPERATIONAL mode
// the radio is locked to the router's channel; all ESP-NOW responses use
// peer.channel=0 (= current Wi-Fi channel automatically). During BOOTSTRAP
// (before Wi-Fi connects) the firmware scans channels 1–13 to find a sibling
// automatically — no fixed channel constant is required. The last found channel
// is cached in NVS so subsequent boots skip straight to the right channel.
// -----------------------------------------------------------------------------
#define ESPNOW_PROTOCOL_VERSION    1      // Increment on any breaking wire format change
#define ESPNOW_MSG_CREDENTIAL_REQ  0x01   // Message type byte: "I need credentials"
#define ESPNOW_MSG_CREDENTIAL_RESP 0x02   // Message type byte: "Here are your credentials"

// Per-channel dwell time (ms) used during ESP-NOW bootstrap channel scanning.
// The new device cycles through Wi-Fi channels 1–13, broadcasting a credential
// request on each and waiting this long for a sibling reply before moving on.
// The sibling is always on the router's channel — this scan finds it automatically
// without requiring the router channel to be known or fixed in advance.
// Total worst-case scan time: ESPNOW_CHANNEL_DWELL_MS × 13 per bootstrap attempt.
// Increase if siblings respond slowly; 500 ms is sufficient for all normal loads.
#define ESPNOW_CHANNEL_DWELL_MS   500


// Note: ISRG Root X1 certificate removed. OTA now uses ESP32-OTA-Pull which
// connects via HTTPClient without explicit cert pinning. HTTPS traffic is
// encrypted in transit but the server certificate is not verified against a
// pinned root CA. Acceptable for internal IoT deployments; revisit if the
// threat model requires full certificate chain validation.


// -----------------------------------------------------------------------------
// Sibling credential verification
// Added to handle stale or incomplete credentials at boot and after sustained
// connection failures. When a node cannot connect with its stored credentials,
// it asks healthy siblings for a fresh bundle before falling back to AP mode.
// -----------------------------------------------------------------------------

// How many rounds of sibling bootstrap to attempt from the WIFI_CONNECT failure
// path before giving up and incrementing the restart counter.
// Set to 1 — one sibling check per restart cycle is enough; excessive retries
// here would delay the restart loop and AP mode fallback unnecessarily.
#define SIBLING_REVERIFY_ATTEMPTS   1

// Window (ms) during which HEALTH_RESP packets are collected during phase 1 of
// the optional two-phase primary selection bootstrap. Longer = more siblings
// heard; shorter = faster startup. 2000 ms is a good balance for most networks.
#define SIBLING_HEALTH_WAIT_MS      2000

// NVS key (within NVS_NAMESPACE) that stores the stale-credentials flag.
// Written as UChar: 0 = not stale, 1 = stale.
// Written by loop() before an unrecoverable restart; read and cleared at BOOT.
#define NVS_KEY_CRED_STALE          "cred_stale"

// Timestamp safety cap — reject received credential bundles whose timestamp
// exceeds FIRMWARE_BUILD_TIMESTAMP by more than this many seconds (~1 year).
// Guards against a rogue node advertising a far-future timestamp that would
// permanently "win" all subsequent timestamp comparisons.
#define SIBLING_TS_MAX_FUTURE_S     31536000ULL

// Message type bytes for the optional two-phase primary selection bootstrap.
// These extend the existing ESPNOW_MSG_CREDENTIAL_REQ / CREDENTIAL_RESP pair.
#define ESPNOW_MSG_HEALTH_QUERY     0x03   // Broadcast: "who is healthy enough to serve creds?"
#define ESPNOW_MSG_HEALTH_RESP      0x04   // Unicast reply: MAC + fw_version + health flags
#define ESPNOW_MSG_OTA_URL_REQ      0x05   // Broadcast/unicast: "what is your OTA JSON URL?"
#define ESPNOW_MSG_OTA_URL_RESP     0x06   // Unicast reply: sender MAC + URL string
#define ESPNOW_MSG_BROKER_REQ       0x07   // Broadcast: "what MQTT broker are you using?"
#define ESPNOW_MSG_BROKER_RESP      0x08   // Unicast reply: host string + port
#define ESPNOW_MSG_RANGING_BEACON   0x09   // Periodic broadcast for RSSI-based peer ranging

// How long (ms) to wait for a sibling's broker address response.
// Tried before mDNS and port scan — fast path when a sibling is on the same LAN.
#define BROKER_ESPNOW_TIMEOUT_MS    2000

// How long (ms) to wait for an OTA URL response from a sibling.
// Short timeout — this is a best-effort enrichment, not a critical boot path.
#define OTA_URL_REQUEST_TIMEOUT_MS  3000

// Define to enable two-phase primary selection bootstrap.
// When defined, bootstrapping nodes first collect health advertisements from
// all siblings and request credentials from the best one (highest firmware
// version + most health flags). Falls back to plain broadcast if no health
// responses arrive — safe for mixed v1/v2 fleets.
// Comment out to compile with simple broadcast bootstrap only.
// #define SIBLING_PRIMARY_SELECTION


// -----------------------------------------------------------------------------
// WS2812B LED Strip
// Addressable RGB strip driven by FastLED on a dedicated FreeRTOS task (Core 1).
// GPIO5 is reserved for RFID SPI SS — use GPIO27 or another safe alternative.
// -----------------------------------------------------------------------------
#define LED_STRIP_PIN             27      // GPIO27 — avoids GPIO5 (RFID SPI SS)
                                          // Alternatives: GPIO25, GPIO26
#define LED_MAX_NUM_LEDS          64      // Compile-time buffer size (pre-allocated,
                                          // never heap). Set to the largest strip you
                                          // will ever connect to this firmware build.
#define LED_DEFAULT_COUNT          8      // Active LEDs at boot. Adjustable at
                                          // runtime via MQTT cmd/led count command.
#define LED_MAX_BRIGHTNESS        80      // 0-255 soft brightness cap.
                                          // FRD recommended range: 30-120.
                                          // FastLED.setMaxPowerInVoltsAndMilliamps
                                          // provides a secondary hard current cap.
#define LED_REFRESH_MS            30      // Strip update interval (~33 fps).
                                          // FRD spec: 20-50 ms.
#define LED_RFID_OVERRIDE_MS    2000      // Duration (ms) of RFID-triggered
                                          // animation before returning to previous state.
#define LED_TASK_STACK          4096      // FreeRTOS stack size for WS2812 task (bytes)
#define LED_TASK_PRIORITY          1      // Task priority — below Wi-Fi/MQTT (priority 5)
#define LED_EVENT_QUEUE_DEPTH     10      // Max queued LED events (drops oldest if full)
#define LED_STRIP_NVS_NAMESPACE "esp32led"  // NVS namespace: persists activeLedCount
                                            // and activeBrightness across reboots


// -----------------------------------------------------------------------------
// RFID Whitelist
// UIDs are stored in compact uppercase hex format without separators ("AABBCCDD").
// Colon-separated formats are normalised on ingestion.
// -----------------------------------------------------------------------------
#define RFID_MAX_WHITELIST        20      // Maximum number of stored known UIDs
#define RFID_UID_STR_LEN          15      // Max UID string length: 7 bytes × 2 hex
                                          // chars = 14 chars + null terminator = 15
#define RFID_NVS_NAMESPACE    "esp32rfid" // NVS namespace for whitelist persistence


// -----------------------------------------------------------------------------
// BLE Beacon Scanner
// Passive BLE central scanner — detects NRF51822 beacons and estimates distance
// via RSSI + log-distance path loss model. No BLE peripheral/advertising.
// Controlled entirely via MQTT; Node-RED provides the UI.
// -----------------------------------------------------------------------------
#define BLE_ENABLED
#define BLE_SCAN_DURATION_S        5      // Seconds per on-demand full scan (cmd/ble/scan)
#define BLE_TRACK_SCAN_DURATION_S  2      // Seconds per repeated tracking scan
#define BLE_MAX_BEACONS           32      // Max beacon entries in discovered list
#define BLE_MAX_TRACKED            8      // Max simultaneously tracked beacons
#define BLE_DEFAULT_TX_POWER     -59      // dBm at 1 m when iBeacon data is absent
#define BLE_PATH_LOSS_N          2.0f     // Path-loss exponent (2.0 = free space)
#define BLE_MQTT_PUBLISH_MS    2000UL     // Publish tracked beacon RSSI to MQTT every 2 s
#define BLE_SERIAL_PRINT_MS   10000UL     // Serial print tracked beacon every 10 s
#define BLE_NVS_NAMESPACE     "esp32ble"  // NVS namespace — persists tracked MACs
#define BLE_NVS_KEY_TRACKED   "tracked_mac"  // CSV of up to BLE_MAX_TRACKED MACs


// -----------------------------------------------------------------------------
// ESP-NOW Ranging
// Passive RSSI-based distance estimation for nearby ESP32 sibling nodes.
// Every node periodically broadcasts ESPNOW_MSG_RANGING_BEACON; the receiver
// uses recvInfo->rx_power to estimate distance with the same log-distance
// formula used by the BLE scanner. Results are published to MQTT topic "espnow".
// -----------------------------------------------------------------------------
#define ESPNOW_TX_POWER_DBM        -59      // Assumed Tx power at 1 m (dBm); calibrate per antenna
#define ESPNOW_PATH_LOSS_N         2.5f     // Path-loss exponent (slightly higher than BLE free-space)
#define ESPNOW_MAX_TRACKED          8       // Max simultaneously tracked peers
#define ESPNOW_MQTT_PUBLISH_MS   2000UL    // Publish peer data to MQTT every 2 s
#define ESPNOW_STALE_MS         15000UL    // Drop peer from table after 15 s of silence
#define ESPNOW_BEACON_INTERVAL_MS 3000UL   // How often this node broadcasts its ranging beacon
