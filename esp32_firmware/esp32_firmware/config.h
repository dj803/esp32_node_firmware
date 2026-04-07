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
#define FIRMWARE_VERSION           "0.0.7"
#define FIRMWARE_BUILD_TIMESTAMP   1775574236ULL   // 2026-04-07 13:43:56 UTC


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
// GitHub OTA source
// The device fetches firmware.bin from the latest release of this repository.
// Replace with your actual GitHub account and repo name.
// -----------------------------------------------------------------------------
#define GITHUB_OTA_OWNER           "myorg"           // GitHub username or organisation
#define GITHUB_OTA_REPO            "esp32-firmware"  // Repository name (not the full URL)


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
// -----------------------------------------------------------------------------
#define ESPNOW_PROTOCOL_VERSION    1      // Increment on any breaking wire format change
#define ESPNOW_MSG_CREDENTIAL_REQ  0x01   // Message type byte: "I need credentials"
#define ESPNOW_MSG_CREDENTIAL_RESP 0x02   // Message type byte: "Here are your credentials"


// -----------------------------------------------------------------------------
// ISRG Root X1 certificate
// Used to authenticate HTTPS connections to api.github.com during OTA checks
// and to raw.githubusercontent.com during binary downloads.
// This certificate expires 2035-06-04. Update it before that date.
//
// Latest certificate:  https://letsencrypt.org/certificates/
// PROGMEM stores it in flash instead of RAM to save heap space.
// -----------------------------------------------------------------------------
static const char GITHUB_ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoBggIBAK3oJHP0FDfzm54rVygc
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
)EOF";
