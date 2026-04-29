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
// FIRMWARE_VERSION can be overridden at build time by passing
//   -DFIRMWARE_VERSION_OVERRIDE=\"0.3.03\"
// via build_flags (PlatformIO) or via Arduino CLI's --build-property. In CI
// the git tag (e.g. "v0.3.03" → "0.3.03") is injected so the compiled binary
// always reports the release version. For local builds without the override,
// the literal below is used — suffixed with "-dev" so field-flashed dev
// binaries can be distinguished from CI releases.
#ifdef FIRMWARE_VERSION_OVERRIDE
#define FIRMWARE_VERSION           FIRMWARE_VERSION_OVERRIDE
#else
// (#70 option B, v0.4.20) Local dev builds use 4-component "MAJOR.MINOR.PATCH.DEV"
// instead of "MAJOR.MINOR.PATCH-dev". The 4th component orders strictly older
// than the same-numbered release (0.4.20.0 < 0.4.20), and increments per dev
// build to allow operators to distinguish iterations. CI builds get the tag
// injected via FIRMWARE_VERSION_OVERRIDE = "0.4.20" (no 4th component) so a
// release is always the cleaner 3-component form.
#define FIRMWARE_VERSION           "0.4.30.0"
#endif
#define FIRMWARE_BUILD_TIMESTAMP   1777291898ULL   // 2026-04-27 (v0.4.14)


// -----------------------------------------------------------------------------
// Logging level
// Controls which log messages are compiled into the firmware.
// Set via build_flags in platformio.ini to change per-environment:
//   build_flags = -DLOG_LEVEL=LOG_LEVEL_WARN    (production — errors + warnings only)
//   build_flags = -DLOG_LEVEL=LOG_LEVEL_DEBUG   (verbose development output)
// The default (LOG_LEVEL_INFO) is defined in logging.h and covers normal
// operational events. Change here to override for all builds that include
// this config.h.
// Possible values: LOG_LEVEL_NONE(0), LOG_LEVEL_ERROR(1), LOG_LEVEL_WARN(2),
//                  LOG_LEVEL_INFO(3), LOG_LEVEL_DEBUG(4)
// #define LOG_LEVEL LOG_LEVEL_INFO    // uncomment to pin the level explicitly


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
// WiFi-outage recovery (v0.3.15)
//
// When the router disappears during OPERATIONAL, the firmware no longer gives
// up after 3 attempts + restart — instead it reconnects on an exponential
// backoff schedule indefinitely. The ESP-IDF WiFi stack re-associates
// automatically when the AP returns; the previous 3-strike restart loop was
// actively sabotaging that recovery.
//
// WIFI_BACKOFF_STEPS_MS is the schedule of wait durations between reconnect
// attempts. The last value is used forever.
//
// WIFI_OUTAGE_RESTART_MAX is a SEPARATE counter (NVS key "wifi_outage") from
// DEVICE_RESTART_MAX. Router blips no longer burn through the generic counter;
// only firmware-panic / MQTT-unrecoverable paths count toward DEVICE_RESTART_MAX.
//
// (#98, v0.4.30) Schedule compressed from
//   { 15s, 30s, 60s, 120s, 300s, 600s }   (saturating at 10 min)
// to
//   { 15s, 30s, 60s, 60s, 60s, 60s }      (saturating at 1 min)
// after the 2026-04-29 PM real-world router-power-failure recovery
// found 4/6 fleet devices stuck silent for 16+ minutes post-recovery
// because they hit the 600 s tier during the chaotic recovery window
// and were waiting out 10 min between retries even after the router
// was back up. The original "10 min spacing once we've given up
// hope" tier was tuned for "router truly dead" — but in practice
// router blips dominate that scenario by frequency, and the 1-min
// fixed tier still polls indefinitely with negligible CPU cost
// (one WiFi.reconnect per minute is nothing).
// -----------------------------------------------------------------------------
static const uint32_t WIFI_BACKOFF_STEPS_MS[] = {
    15000, 30000, 60000, 60000, 60000, 60000
};
#define WIFI_BACKOFF_STEPS_COUNT \
    (sizeof(WIFI_BACKOFF_STEPS_MS)/sizeof(WIFI_BACKOFF_STEPS_MS[0]))

#define WIFI_OUTAGE_RESTART_MAX    10      // Max reboots caused specifically by WiFi
                                           // outage before falling to AP mode.
                                           // Kept high because (A) below recovers
                                           // transparently — reaching this limit is
                                           // rare; it's a safety net, not a design path.

// How many consecutive backoff cycles the OPERATIONAL recovery loop must see
// an AUTH_EXPIRE / HANDSHAKE_TIMEOUT disconnect reason before treating the
// credentials as truly wrong and falling to AP mode. Two cycles avoids false
// positives from rare transient auth failures (e.g. CPU-starved WPA handshake).
// After this many consecutive failed WiFi.reconnect() attempts, switch to the
// heavier WiFi.disconnect(true)+WiFi.begin() path. WiFi.reconnect() is sufficient
// for brief blips but silently fails after BEACON_TIMEOUT / complete AP power loss.
// Index 3 = after 15 s + 30 s + 60 s have failed (~105 s elapsed). Subsequent
// attempts (120 s, 300 s, 600 s spacing) use disconnect+begin which is identical
// to the bootstrap path and is proven to work after a full router power cycle.
#define WIFI_FULL_RECONNECT_AFTER_IDX  3

#define WIFI_AUTH_FAIL_CYCLES       2

// NVS key holding the wifi-outage restart counter.
#define NVS_KEY_WIFI_OUTAGE         "wifi_outage"

// -----------------------------------------------------------------------------
// AP-mode background STA scan (v0.3.15 — fixes "stuck in AP after router
// returns")
//
// While in AP mode, the radio runs in APSTA. Every AP_STA_SCAN_INTERVAL_MS
// the device scans for its configured SSID; on a hit it fires WiFi.begin()
// and, on GOT_IP, restarts into OPERATIONAL after a short grace period.
// Scans are skipped while an admin HTTPS session is active (prevents
// interrupting form entry).
//
// Feature-flagged so the entire behavior can be reverted by setting
// AP_MODE_STA_ENABLED to 0 if a router is found that mis-behaves under APSTA.
// -----------------------------------------------------------------------------
#define AP_MODE_STA_ENABLED         1       // 1 = scan for SSID in AP mode; 0 = old behavior
#define AP_STA_SCAN_INTERVAL_MS     30000   // How often to scan for the configured SSID
#define AP_STA_RECONNECT_GRACE_MS   5000    // Settle time between GOT_IP and ESP.restart()
#define AP_ADMIN_IDLE_MS            60000   // Skip scan if admin HTTPS handler ran within
                                            // this window

// ── AP-mode idle timeout (v0.3.33) ──────────────────────────────────────────
// AP mode is intended only for first-boot provisioning or hands-on dev work.
// If no admin activity (HTTPS handler hit) and no STA reconnect happens for
// AP_MODE_IDLE_TIMEOUT_MS, the device hard-restarts. This prevents a node
// that fell to AP mode from a transient failure from sitting there forever
// invisible to the fleet. A real operator session resets the timer on every
// HTTPS request.
#define AP_MODE_IDLE_TIMEOUT_MS    300000   // 5 minutes — short enough that a
                                            // misclassified failure is not a
                                            // long outage; long enough that
                                            // an operator can actually fill
                                            // out the form.


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
// (v0.4.24, #76 sub-C) Time-based escalation is the primary trigger; the
// count-based threshold is now a defensive backstop bumped 10 → 30 to avoid
// firing prematurely on fast-retry storms (mathieucarbou AsyncTCP retries
// in <1 s when broker is up but the connect handshake fails). The unrecoverable
// timeout fires after 10 min of continuous disconnection, which catches the
// genuine "broker is gone" case without being fooled by reconnect cadence.
#define MQTT_RESTART_THRESHOLD        30   // Hard-restart the device after this many
                                           // consecutive failures (Tier 2 backstop).
#define MQTT_UNRECOVERABLE_TIMEOUT_MS 600000 // (#76 sub-C) Primary Tier 2 trigger:
                                           // restart if MQTT has been continuously
                                           // disconnected for this long (10 min default).
                                           // More intuitive than count-based and
                                           // immune to backoff-cadence weirdness.
// (v0.4.24, #76 sub-D) Restart-loop cool-off: if the most recent N
// consecutive entries in the RestartHistory ring buffer all share a single
// recovery-failure cause (currently "mqtt_unrecoverable"), the next boot
// enters AP mode instead of repeating the doomed cycle. Operator can fix
// the underlying issue (broker config, network) via the AP web portal.
// Streak is broken by RestartHistory::push("operational") which fires after
// MQTT_LOOP_HEALTHY_UPTIME_MS of stable connectivity.
#define MQTT_RESTART_LOOP_THRESHOLD    3   // ≥ this many consecutive mqtt_unrecoverable → AP_MODE
#define MQTT_LOOP_HEALTHY_UPTIME_MS  300000 // 5 min stable MQTT → push "operational" to break streak
// (v0.4.14, 2026-04-27) Bumped 12000 → 90000.
// 12 s was tuned for the "TCP connected, MQTT CONNACK never arrived" hang —
// but the same path ALSO fires on the much more common "broker down, lwIP
// SYN retrying" case (lwIP TCP connect timeout is ~75 s by default). So any
// broker outage longer than 12 s caused EVERY device to ESP.restart()
// simultaneously, producing the v0.4.10 #51 + 2026-04-27 10:42 + 14:04
// cascade patterns (see CHAOS_TESTING.md M2). 90 s gives lwIP SYN room to
// fire its onError naturally before we declare hung.
//
// Better long-term: mqttIsHung() should distinguish "TCP up, CONNACK pending"
// from "TCP not yet up". See SUGGESTED_IMPROVEMENTS #65 (restart-policy
// hardening).
// (v0.4.15 patch 2, 2026-04-27) 90 s wasn't enough — M3 180 s blip still hit
// it at 113 s and the force-disconnect itself triggered the AsyncTCP tcp_arg
// race. Bump to 300 s (5 min) so this watchdog ONLY fires for genuinely stuck
// states, not normal broker outages. lwIP TCP errors out the connection on
// its own timeline; we shouldn't second-guess it during typical retries.
#define MQTT_HUNG_TIMEOUT_MS      300000   // If connect() produces no callback (success or
                                           // failure) within this window, the async client
                                           // has hung — force-disconnect (via mqttForceDisconnect()).

#define OTA_CHECK_INTERVAL_MS      3600000 // How often (ms) the device polls GitHub
                                           // Releases for a newer firmware version.
                                           // Default: 1 hour (3 600 000 ms).
                                           // You can also trigger an immediate check
                                           // via MQTT: publish to cmd/ota_check.

// (#97, v0.4.29) Cascade-recovery OTA quiet window. After any
// WiFi/MQTT disconnect, otaCheckNow() returns early for this many ms.
// Mirrors the v0.4.28 mqttPublish cascade-window guard
// (CASCADE_QUIET_MS), but the OTA window is much longer because heap
// state post-cascade is unknown — we want the device to stabilise on
// steady-state heap and TCP reconnect before pulling 1.6 MB of new
// firmware over HTTPS. 5 min (300 s) covers the longest observed
// AP-recovery storm (12.6 min #96 outage) by an order of magnitude
// for typical 30-90 s blips, and prevents the v0.4.26 → v0.4.27
// AP-cycle #2 issue where every device pulled OTA mid-recovery.
#define OTA_CASCADE_QUIET_MS         300000  // 5 min post-disconnect lockout for OTA fetch

#define OTA_PROGRESS_TIMEOUT_MS      30000 // Per-chunk deadline during the OTA download:
                                           // if no progress callback fires for this long,
                                           // a one-shot FreeRTOS timer calls ESP.restart()
                                           // to recover cleanly rather than waiting for
                                           // the TWDT. Picked at 30 s to tolerate normal
                                           // CDN slow-starts but catch a true stall like
                                           // the v0.3.28 Charlie freeze (see SUGGESTED
                                           // _IMPROVEMENTS #24).

// ── Pre-flight gate (v0.3.33) ────────────────────────────────────────────────
// Checked AFTER NimBLE deinit, BEFORE the blocking download. If either limit
// is breached we publish ota_preflight failure + abort cleanly. Restart-and-
// retry on next interval is preferable to a torn-down failure mid-flash.
//
// Numbers are conservative defaults validated against arduino-esp32 3.1.1 +
// mbedTLS + AsyncTCP heap usage. Tighten with telemetry once we have it.
#define OTA_PREFLIGHT_HEAP_FREE_MIN  80000  // bytes — total free heap (MALLOC_CAP_8BIT)
#define OTA_PREFLIGHT_HEAP_BLOCK_MIN 32000  // bytes — largest contiguous free block

// (#32, v0.4.25) Per-subsystem heap-headroom gates at boot. Mirrors the
// OTA preflight gate above but applied at subsystem-init time. Prevents
// a subsystem from booting into a degraded heap state where its first
// allocation would crash. Subsystems that skip the gate stay DISABLED
// for the rest of this boot — operator can power-cycle to retry on a
// fresher heap. The numbers below are conservative defaults; tighten
// once telemetry exists to validate per-subsystem real-world need.
//
// BLE init: NimBLE controller buffers + GATT tables + advertising state.
// Heaviest single-shot subsystem allocation (~50 KB observed). Only
// meaningful on variants that compile in BLE_ENABLED — esp32dev
// production has BLE disabled at compile time so the gate is a no-op.
#define BLE_INIT_HEAP_FREE_MIN     60000
#define BLE_INIT_HEAP_BLOCK_MIN    32000

// TLS keygen (RSA-2048, first boot only): mbedTLS scratch + intermediate
// big-num buffers + DER serialiser. Once cached in NVS the subsequent
// boots load the PEM and skip keygen entirely, so the gate matters only
// on a freshly-flashed device or after AP portal NVS is wiped.
#define TLS_KEYGEN_HEAP_FREE_MIN   50000
#define TLS_KEYGEN_HEAP_BLOCK_MIN  30000

// MQTT init: AsyncMqttClient + AsyncTCP socket + initial CONNECT framing.
// Modest (~12 KB observed) but the gate guards against post-restart
// boots into a fragmented heap (a hung mqtt_unrecoverable cycle could
// land here with the heap not yet recovered). Threshold sized to leave
// margin for the publish heap-guard's own MQTT_PUBLISH_HEAP_MIN (8192).
#define MQTT_INIT_HEAP_FREE_MIN    24000
#define MQTT_INIT_HEAP_BLOCK_MIN   12000

// ── Hardcoded fallback OTA manifest URLs (v0.3.33) ───────────────────────────
// Tried in order if gAppConfig.ota_json_url (NVS) returns a non-2xx HTTP code.
// Last-resort safety net so a misconfigured portal entry, a Pages outage, or
// a dead CDN doesn't permanently strand the device. Sibling-URL fallback
// runs AFTER this list is exhausted (so a healthy fleet recovers without
// touching the network at all if any one device has a working URL).
//
// Edit these to match the deployed CDN paths. The first entry SHOULD match
// the OTA_JSON_URL default below; the second is the GitHub release-asset URL
// (works even if Pages is down, but version-specific so requires updating
// the manifest schema slightly — currently best-effort).
#define OTA_FALLBACK_URL_COUNT       2
static const char* const OTA_FALLBACK_URLS[OTA_FALLBACK_URL_COUNT] = {
    "https://dj803.github.io/esp32_node_firmware/ota.json",
    "https://raw.githubusercontent.com/dj803/esp32_node_firmware/gh-pages/ota.json"
};

// ── App validation deadline (Phase 2 — used in v0.3.34) ──────────────────────
// After an OTA boot, the new firmware must observe Wi-Fi + MQTT + 1 heartbeat
// within this window before calling esp_ota_mark_app_valid_cancel_rollback().
// If the deadline expires the bootloader will roll back on the next reset.
#define OTA_VALIDATION_DEADLINE_MS   300000  // 5 minutes


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

#define NODE_NAME                  ""                   // Friendly per-device name
                                                        // (e.g. "Alpha", "Bravo"). Empty
                                                        // until set via cmd/espnow/name or
                                                        // the settings portal. Visible in
                                                        // every status / espnow publish so
                                                        // Node-RED can label by name.


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

// (#34, v0.4.24) Captive portal DNS hijack. When set to 1, the AP portal
// runs a UDP DNS server on port 53 that resolves every query to the AP's
// own IP. iOS / Android probe well-known captive-detection URLs on
// connect; with the hijack in place those URLs resolve to us, the OS
// flags the network as captive, and the user sees a "Sign in to network"
// sheet instead of having to manually browse to 192.168.4.1.
//
// Phase 1 (this gate) ships the DNS hijack only. Phase 2 — a plain HTTP
// listener on port 80 that 302-redirects to https://192.168.4.1/ — is
// needed for full UX (without it the captive sheet shows a broken page
// because the OS probes http://, not https://, and our HTTPS server
// doesn't bind 80). Tracked under #34 in SUGGESTED_IMPROVEMENTS.
//
// Adds ~12 KB of flash (DNSServer + AsyncUDP). Set to 0 if AP mode is
// never expected (operator-only deployments) and the flash budget is
// tight. v0.5.0+ relay variant is closer to the 1.875 MB ota slot
// limit; revisit then.
#define AP_CAPTIVE_DNS_ENABLED     1


// -----------------------------------------------------------------------------
// RFID reader (MFRC522v2 via SPI)
// Define RFID_ENABLED to activate the RFID module. Comment out on nodes
// that do not have an MFRC522 reader attached — no code overhead either way.
// Variant builds (#71) can override via build_flags by setting -DRFID_DISABLED
// — the gate below skips the define so the source-level #ifdef RFID_ENABLED
// blocks compile out at preprocess time.
// -----------------------------------------------------------------------------
#ifndef RFID_DISABLED
#define RFID_ENABLED                    // Comment out to disable on reader-less nodes
#endif

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

// RFID Playground (v0.3.17) — programmatic read/write via MQTT
//   cmd/rfid/program   arms the next card for a multi-block write
//   cmd/rfid/read_block arms the next card for a single-block read
//   cmd/rfid/cancel    clears any armed request
// While armed the firmware enters a transient state (see RfidMode in rfid.h):
// auto-publish on telemetry/rfid is paused until the action completes, times
// out, or is cancelled. RFID_PROGRAM_TIMEOUT_MS is the default timeout — the
// Node-RED request can override it on a per-request basis.
#define RFID_PROGRAM_TIMEOUT_MS 15000   // Default idle timeout after arming (ms).
                                        // If no tag is presented within this window
                                        // the request auto-cancels with status="timeout".
#define RFID_MAX_WRITE_BLOCKS     32    // Upper bound on writes[] in a single
                                        // cmd/rfid/program payload. Bumped from 8
                                        // in v0.4.11 to fit a complete NDEF URI
                                        // image (~30 4-byte pages on NTAG21x for
                                        // a typical URL). The response JSON only
                                        // echoes block indices, so the larger
                                        // array does not blow the stack buffer.


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

// -----------------------------------------------------------------------------
// DESIGN NOTE — MQTT traffic is PLAINTEXT on the LAN.
// MQTT CONNECT (incl. username/password) and every subsequent PUBLISH travel
// unencrypted on the wire. Accepted trade-off for a private LAN deployment:
// a passive sniffer on the same segment can read broker credentials and
// telemetry, but cannot forge rotation payloads — those are AES-128-GCM
// encrypted at the application layer. Revisit if the deployment ever moves
// off a trusted segment. Migration path documented in
// docs/SUGGESTED_IMPROVEMENTS.txt §7 (AsyncMqttClient::setSecure + pinned CA).
// -----------------------------------------------------------------------------

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
// #define BLE_ENABLED   // (#51 diagnostic 2026-04-26) BLE temporarily disabled — proof-of-concept only, not actively used. Frees ~30-50 KB flash + ~6-8 KB heap, eliminates NimBLE/WiFi/ESP-NOW coexistence as a variable. Re-enable when needed; see SUGGESTED_IMPROVEMENTS #51.
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
// NVS namespace registry (v0.3.08)
//
// Single authoritative list of every NVS namespace in the firmware.
// The #define constants above are the actual strings — kept for backward
// compatibility so all existing call-sites compile unchanged.
//
// Add new namespaces here first (new enum value + nvsNsName() case), then add
// the corresponding #define above.
//
// Note: ESP-IDF NVS namespace names are limited to 15 characters.
// -----------------------------------------------------------------------------
enum class NvsNs : uint8_t {
    CREDENTIALS  = 0,   // NVS_NAMESPACE              ("esp32cred")
    DEVICE_ID    = 1,   // DEVICE_ID_NVS_NAMESPACE    ("esp32id")
    BROKER_CACHE = 2,   // BROKER_CACHE_NVS_NAMESPACE ("esp32disc")
    LED          = 3,   // LED_STRIP_NVS_NAMESPACE    ("esp32led")
    RFID         = 4,   // RFID_NVS_NAMESPACE         ("esp32rfid")
    BLE          = 5,   // BLE_NVS_NAMESPACE          ("esp32ble")
};

inline const char* nvsNsName(NvsNs ns) {
    switch (ns) {
        case NvsNs::CREDENTIALS:  return NVS_NAMESPACE;
        case NvsNs::DEVICE_ID:    return DEVICE_ID_NVS_NAMESPACE;
        case NvsNs::BROKER_CACHE: return BROKER_CACHE_NVS_NAMESPACE;
        case NvsNs::LED:          return LED_STRIP_NVS_NAMESPACE;
        case NvsNs::RFID:         return RFID_NVS_NAMESPACE;
        case NvsNs::BLE:          return BLE_NVS_NAMESPACE;
        default:                  return "unknown";
    }
}


// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Relay (BDD 2CH 5V — JQC-3FF-S-Z) — v0.5.0 hardware integration
// Two-channel active-LOW relay driver. Default-DISABLED: production fleet
// devices that don't have the board attached should NOT drive these pins.
// Enable per device via build_flags `-DRELAY_ENABLED` (e.g. an
// `[env:esp32dev_relay]` PIO env), OR uncomment the define here for a
// device that physically has the relay wired. See docs/PLAN_RELAY_HALL_v0.5.0.md.
// -----------------------------------------------------------------------------
// #define RELAY_ENABLED
#define RELAY_CH1_PIN         25      // Output-capable, no strapping conflict
#define RELAY_CH2_PIN         26      // Output-capable, no strapping conflict
#define RELAY_ACTIVE_LOW       1      // 1 = LOW energizes (JQC-3FF-S-Z); 0 = active-HIGH boards
#define RELAY_NVS_NAMESPACE   "esp32relay"

// -----------------------------------------------------------------------------
// Hall sensor (BMT 49E module — Honeywell SS49E + LM393) — v0.5.0
// Periodic ADC1 read → mGauss conversion → MQTT telemetry. Default-DISABLED.
// Enable per device via `-DHALL_ENABLED`. See docs/PLAN_RELAY_HALL_v0.5.0.md
// for the 2×10 kΩ divider note (sensor is 5 V powered; ADC max is 3.3 V).
// -----------------------------------------------------------------------------
// #define HALL_ENABLED
#define HALL_AO_PIN                   32      // ADC1_CH4, safe with WiFi
#define HALL_DO_PIN                   33      // Optional digital threshold (LM393 D0)
#define HALL_INTERVAL_MS            1000      // Read + publish every 1 s by default
#define HALL_THRESHOLD_GAUSS          50      // Default threshold for edge-detection
#define HALL_DIVIDER_RATIO          2.0f      // 2×10 kΩ divider (5 V → 2.5 V)
#define HALL_SENSOR_VCC_V           5.0f      // SS49E supply spec
#define HALL_SENSITIVITY_MV_PER_GAUSS  1.4f   // SS49E typical
#define HALL_NVS_NAMESPACE    "esp32hall"

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

// Drift-filter defaults (F4). Overridden at runtime via cmd/espnow/filter and persisted to NVS.
#define ESPNOW_EMA_ALPHA_X100       30      // RSSI EMA smoothing factor × 100 (α = 0.30).
                                            // Higher → reacts faster but noisier.
#define ESPNOW_OUTLIER_DB           15      // Reject RSSI samples deviating > 15 dB from EMA.
                                            // Set 0 to disable outlier filtering.

// Calibration defaults (F3). Not user-tunable compile-time beyond these.
#define ESPNOW_CALIBRATION_SAMPLES  30      // RSSI readings per calibration step (median taken).
                                            // At 3 s beacon interval this is ~90 s per step.
#define ESPNOW_CALIBRATION_TIMEOUT_MS 120000UL  // Abort step if peer silent for 2 min.
#define ESPNOW_CALIB_MAX_POINTS     6       // Max (distance, rssi_median) pairs in the multi-point
                                            // buffer used by 'commit' linreg (v0.4.07 / #39).

// Post-OTA validation window — beacon suppression to free CPU for the
// otaValidationConfirmHealth() critical path during the first 30 s of a
// freshly-OTA'd boot. See espnow_ranging.h::espnowRangingLoop comment block
// and SUGGESTED_IMPROVEMENTS.txt #35 (Charlie v0.4.07 task_wdt analysis).
#define ESPNOW_POST_OTA_QUIET_MS    30000UL


// -----------------------------------------------------------------------------
// MQTT-triggered sleep modes (v0.3.20)
//
// Three commands cover the full ESP32 idle spectrum:
//   cmd/modem_sleep  — Wi-Fi modem power save; CPU keeps running, MQTT stays up
//   cmd/sleep        — light sleep; CPU halted, radio torn down, RAM preserved
//   cmd/deep_sleep   — deep sleep; cold boot on wake, RAM wiped, lowest current
//
// All three accept JSON payload {"seconds":N} with N in [MIN_SLEEP_SECONDS,
// MAX_SLEEP_SECONDS]. Values outside the range are rejected with a warning log
// and no state change. The 86400-second (24 h) cap prevents an operator from
// accidentally parking a device off-network for days.
//
// For cmd/sleep and cmd/deep_sleep, dispatch is deferred by SLEEP_DEFER_MS
// (mirroring the cmd/restart pattern) so the "sleeping"/"deep_sleeping" status
// publish has time to drain through AsyncMqttClient before the radio goes off.
// -----------------------------------------------------------------------------
#define MIN_SLEEP_SECONDS          1        // Shortest allowed duration (rejects 0/negative)
#define MAX_SLEEP_SECONDS       86400        // 24 h ceiling — sanity cap, not a hard limit
#define SLEEP_DEFER_MS             300        // Delay between status publish and radio teardown
