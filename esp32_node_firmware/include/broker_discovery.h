#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Preferences.h>
// Note: ESPmDNS.h is NOT included here.
// The Arduino wrapper MDNSResponder has an unstable API across ESP32 core
// versions (2.x vs 3.x). Instead we call the ESP-IDF mdns layer directly,
// which has a stable C API available in all ESP32 Arduino core versions.
#include "mdns.h"         // ESP-IDF mdns C API (always available in Arduino ESP32)
#include "config.h"
#include "device_id.h"   // DeviceId::get() used as mDNS hostname
#include "nvs_utils.h"   // NvsPutIfChanged — compare-before-write wrappers

// =============================================================================
// broker_discovery.h  —  Automatic MQTT broker discovery
//
// Tries to find a running Mosquitto (or any MQTT broker) on the local network
// so the device does not need a hardcoded broker IP address.
//
// THREE-STEP FALLBACK CHAIN (called from discoverBroker()):
//
//   Step 1 — mDNS / DNS-SD  (fast ~1–3 s, requires broker-side config)
//   ──────────────────────────────────────────────────────────────────────
//   Uses the ESP-IDF mdns_query_ptr() C API to query for "_mqtt._tcp"
//   services. This API is stable across all ESP32 Arduino core versions
//   (2.x and 3.x) because it sits below the Arduino MDNSResponder wrapper.
//
//   Mosquitto does not advertise itself by default. One-time setup on the
//   broker machine:
//
//   Linux (Avahi):
//     Create /etc/avahi/services/mqtt.service:
//       <service-group>
//         <n>Mosquitto MQTT</n>
//         <service><type>_mqtt._tcp</type><port>1883</port></service>
//       </service-group>
//     sudo systemctl restart avahi-daemon
//
//   Windows (Python, run alongside Mosquitto):
//     pip install zeroconf
//     python advertise_mqtt.py   (see scripts/ folder in this repo)
//
//   If multiple brokers respond, the one with the fastest TCP connect
//   time is preferred.
//
//   Step 2 — TCP port scan  (slow ~5–50 s, zero broker-side config)
//   ──────────────────────────────────────────────────────────────────────
//   Probes every host on the local /24 subnet for open port 1883.
//   Skips own IP and gateway. Stops after PORTSCAN_MAX_RESULTS hits.
//   Picks the candidate with the lowest last-octet value if multiple found.
//
//   Step 3 — Stored URL fallback  (instant, always succeeds)
//   ──────────────────────────────────────────────────────────────────────
//   Parses the mqtt_broker_url from the NVS credential bundle.
//   The final safety net when neither automatic method works.
//
// Set BROKER_DISCOVERY_ENABLED 0 in config.h to skip Steps 1 and 2.
// The NVS credential bundle is never modified by discovery.
// =============================================================================


// ── Result types ───────────────────────────────────────────────────────────────
enum class DiscoveryMethod {
    NONE      = 0,   // No broker found (should not happen — Step 3 always succeeds)
    MDNS      = 1,   // Found via mDNS _mqtt._tcp advertisement
    PORTSCAN  = 2,   // Found via TCP port scan of the local /24 subnet
    STORED    = 3,   // Using the URL stored in the NVS credential bundle
    CACHED    = 4,   // Re-connected to a previously-used broker address
    SIBLING   = 5    // Received from an operational sibling via ESP-NOW
};

struct BrokerResult {
    char            host[64]  = {0};   // IP address as dotted-decimal string
    uint16_t        port      = 1883;  // TCP port number
    DiscoveryMethod method    = DiscoveryMethod::NONE;

    bool found() const { return host[0] != '\0'; }
};


// ── Broker cache: persists last 2 successfully-used broker addresses ──────────
// NVS namespace BROKER_CACHE_NVS_NAMESPACE ("esp32disc").
// Keys: "b0h"/"b0p" = most recent, "b1h"/"b1p" = second most recent.
// Slot 0 = most recent connection, Slot 1 = previous.

struct BrokerCache {
    char     host[2][64];
    uint16_t port[2];
    int      count;   // number of valid entries (0, 1, or 2)
};

static BrokerCache loadBrokerCache() {
    BrokerCache c;
    memset(&c, 0, sizeof(c));
    Preferences prefs;
    prefs.begin(BROKER_CACHE_NVS_NAMESPACE, true);   // read-only
    prefs.getBytes("b0h", c.host[0], sizeof(c.host[0]));
    c.port[0] = prefs.getUShort("b0p", 0);
    prefs.getBytes("b1h", c.host[1], sizeof(c.host[1]));
    c.port[1] = prefs.getUShort("b1p", 0);
    prefs.end();
    c.count = (c.host[0][0] != '\0' ? 1 : 0) + (c.host[1][0] != '\0' ? 1 : 0);
    return c;
}

// Call after any successful broker connection to persist the address.
// New entry goes into slot 0; old slot 0 shifts to slot 1.
// No-op if the address is already in slot 0.
static void saveBrokerToCache(const char* host, uint16_t port) {
    BrokerCache c = loadBrokerCache();
    if (strcmp(c.host[0], host) == 0 && c.port[0] == port) return;  // already current
    Preferences prefs;
    prefs.begin(BROKER_CACHE_NVS_NAMESPACE, false);  // read-write
    // Shift: slot 0 → slot 1. NvsPutIfChanged skips writes when the value
    // already matches — e.g. if slot 1 already held the previous slot 0,
    // no redundant write is issued.
    NvsPutIfChanged(prefs, "b1h", c.host[0], sizeof(c.host[0]));
    NvsPutIfChanged(prefs, "b1p", (uint16_t)c.port[0]);
    // Write new entry as slot 0
    NvsPutIfChanged(prefs, "b0h", host, strnlen(host, 63) + 1);
    NvsPutIfChanged(prefs, "b0p", (uint16_t)port);
    prefs.end();
    Serial.printf("[Discovery] Cache: saved %s:%d as slot 0\n", host, port);
}


// ── Internal: TCP connect probe ────────────────────────────────────────────────
// Returns round-trip time in ms, or 0xFFFFFFFF on timeout/failure.
// Used to rank multiple mDNS candidates by response latency.
static uint32_t tcpPingMs(const char* host, uint16_t port, uint32_t timeoutMs) {
    WiFiClient client;
    uint32_t t0 = millis();
    if (client.connect(host, port, (int32_t)timeoutMs)) {
        uint32_t elapsed = millis() - t0;
        client.stop();
        return elapsed;
    }
    return 0xFFFFFFFF;
}


// ── Step 0: Try last 2 cached broker addresses ────────────────────────────────
// Loads up to 2 previously-used broker addresses from NVS and prints both to
// serial regardless of connectivity. Attempts TCP connection to each in order
// (most recent first). Returns immediately on the first reachable broker.
// Falls through (empty result) if neither cached address responds.
static BrokerResult tryCachedBrokers() {
    BrokerResult result;
    BrokerCache c = loadBrokerCache();

    if (c.count == 0) {
        Serial.println("[Discovery] Step 0: no cached brokers");
        return result;
    }

    // Always print all cached entries so they are visible in the serial log
    Serial.printf("[Discovery] Step 0: %d cached broker(s) known:\n", c.count);
    for (int i = 0; i < 2; i++) {
        if (c.host[i][0] != '\0') {
            Serial.printf("[Discovery]   [%d] %s:%d\n", i, c.host[i], c.port[i]);
        }
    }

    // Try each cached broker in order (slot 0 = most recent)
    for (int i = 0; i < 2; i++) {
        if (c.host[i][0] == '\0' || c.port[i] == 0) continue;
        Serial.printf("[Discovery] Step 0: probing %s:%d...\n", c.host[i], c.port[i]);
        uint32_t t = tcpPingMs(c.host[i], c.port[i], 1000);
        if (t != 0xFFFFFFFF) {
            strncpy(result.host, c.host[i], sizeof(result.host) - 1);
            result.port   = c.port[i];
            result.method = DiscoveryMethod::CACHED;
            Serial.printf("[Discovery] Step 0: connected %s:%d (%u ms)\n",
                          result.host, result.port, t);
            return result;
        }
        Serial.printf("[Discovery] Step 0: %s:%d unreachable\n", c.host[i], c.port[i]);
    }
    return result;   // empty — caller falls through to Steps 1–3
}


// ── Step 0.5: Ask siblings via ESP-NOW ────────────────────────────────────────
// Broadcasts a BROKER_REQ on the current Wi-Fi channel. Any OPERATIONAL sibling
// that already has a connected broker will reply with its address within
// BROKER_ESPNOW_TIMEOUT_MS. This is faster than mDNS and requires no broker-side
// config — ideal for networks without mDNS or when the port scan would be slow.
// Declared in espnow_responder.h (included before broker_discovery.h).
static BrokerResult tryEspNowBroker() {
    BrokerResult result;
    char     host[64] = {0};
    uint16_t port     = 0;
    if (!espnowGetSiblingBroker(host, sizeof(host), &port)) return result;
    if (host[0] == '\0' || port == 0) return result;

    // Verify the address is actually reachable before returning it
    Serial.printf("[Discovery] Step 0.5: sibling gave %s:%d — probing...\n", host, port);
    uint32_t t = tcpPingMs(host, port, 1000);
    if (t == 0xFFFFFFFF) {
        Serial.printf("[Discovery] Step 0.5: %s:%d unreachable — discarding\n", host, port);
        return result;
    }

    strncpy(result.host, host, sizeof(result.host) - 1);
    result.port   = port;
    result.method = DiscoveryMethod::SIBLING;
    Serial.printf("[Discovery] Step 0.5: using sibling broker %s:%d (%u ms)\n",
                  result.host, result.port, t);
    return result;
}


// ── Step 1: mDNS discovery via ESP-IDF mdns C API ────────────────────────────
// Uses mdns_query_ptr() from the ESP-IDF mdns layer — the correct function
// for "find all instances of a service type" (DNS-SD PTR query).
// Works across all ESP32 Arduino core versions (avoids the unstable wrapper).
//
// The ESP-IDF mdns_result_t is a linked list:
//   result->addr->addr   — IP address (esp_ip4_addr_t)
//   result->port         — uint16_t port
//   result->next         — pointer to next result (or NULL)
static BrokerResult tryMdnsDiscovery() {
    BrokerResult result;
    Serial.println("[Discovery] Step 1: mDNS query for _mqtt._tcp...");

    // Initialise the ESP-IDF mdns service. Safe to call multiple times —
    // returns ESP_ERR_INVALID_STATE if already initialised, which we ignore.
    esp_err_t initErr = mdns_init();
    if (initErr != ESP_OK && initErr != ESP_ERR_INVALID_STATE) {
        Serial.printf("[Discovery] mDNS: init failed (%d)\n", initErr);
        return result;
    }

    // Set a hostname so this device is identifiable on the network.
    // Uses the UUID for uniqueness.
    mdns_hostname_set(DeviceId::get().c_str());

    // mdns_query_ptr() sends a DNS-SD PTR query for all instances of a
    // service type — the correct function for "find any _mqtt._tcp broker".
    // It fills a linked list of mdns_result_t, each containing hostname,
    // port, and the address list for one discovered service instance.
    // Note: service and proto are passed WITHOUT leading underscores —
    // ESP-IDF adds them internally ("mqtt" -> "_mqtt", "tcp" -> "_tcp").
    mdns_result_t* results = NULL;
    esp_err_t err = mdns_query_ptr(
        "mqtt",                    // Service type (ESP-IDF prepends "_")
        "tcp",                     // Protocol  (ESP-IDF prepends "_")
        MDNS_DISCOVERY_TIMEOUT_MS, // How long to wait for responses (ms)
        10,                        // Maximum number of results to collect
        &results);

    if (err != ESP_OK || results == NULL) {
        Serial.println("[Discovery] mDNS: no _mqtt._tcp services found");
        mdns_query_results_free(results);
        return result;   // Empty — caller tries Step 2
    }

    // Count results and log them
    int count = 0;
    for (mdns_result_t* r = results; r != NULL; r = r->next) count++;
    Serial.printf("[Discovery] mDNS: %d service(s) found\n", count);

    // Find the best result using TCP connect latency as the ranking metric.
    // Lower latency = broker is closer on the network.
    char      bestHost[64]  = {0};
    uint16_t  bestPort      = 0;
    uint32_t  bestTimeMs    = 0xFFFFFFFF;

    for (mdns_result_t* r = results; r != NULL; r = r->next) {
        // Extract IP address from the result's address list
        if (r->addr == NULL) continue;   // Skip results with no IP

        char ipStr[40];
        // addr->addr is esp_ip_addr_t; for IPv4 we use the .u_addr.ip4 field
        esp_ip4_addr_t ip4 = r->addr->addr.u_addr.ip4;
        snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&ip4));

        uint16_t port = r->port;

        Serial.printf("[Discovery] mDNS candidate: %s:%d\n", ipStr, port);

        uint32_t t = tcpPingMs(ipStr, port, 500);
        Serial.printf("[Discovery] mDNS TCP probe %s: %s ms\n",
                      ipStr, t == 0xFFFFFFFF ? "timeout" : String(t).c_str());

        if (t < bestTimeMs) {
            bestTimeMs = t;
            strncpy(bestHost, ipStr, sizeof(bestHost) - 1);
            bestPort = port;
        }
    }

    mdns_query_results_free(results);   // Always free the result list

    if (bestHost[0] == '\0') {
        // All candidates timed out on TCP probe
        Serial.println("[Discovery] mDNS: all candidates failed TCP probe");
        return result;
    }

    strncpy(result.host, bestHost, sizeof(result.host) - 1);
    result.port   = bestPort;
    result.method = DiscoveryMethod::MDNS;
    Serial.printf("[Discovery] mDNS: selected %s:%d (%u ms)\n",
                  result.host, result.port, bestTimeMs);
    return result;
}


// ── Step 2: TCP port scan ─────────────────────────────────────────────────────
static BrokerResult tryPortScan() {
    BrokerResult result;
    Serial.println("[Discovery] Step 2: TCP port scan (port 1883)...");

    IPAddress myIP      = WiFi.localIP();
    IPAddress myGateway = WiFi.gatewayIP();

    // Build /24 network base from first three octets of own IP
    uint32_t base = ((uint32_t)myIP[0] << 24)
                  | ((uint32_t)myIP[1] << 16)
                  | ((uint32_t)myIP[2] << 8);

    Serial.printf("[Discovery] Scanning %d.%d.%d.1-254...\n",
                  myIP[0], myIP[1], myIP[2]);

    IPAddress candidates[PORTSCAN_MAX_RESULTS];
    int       foundCount = 0;

    WiFiClient client;

    for (int i = 1; i <= 254 && foundCount < PORTSCAN_MAX_RESULTS; i++) {
        IPAddress target(
            (base >> 24) & 0xFF,
            (base >> 16) & 0xFF,
            (base >>  8) & 0xFF,
            (uint8_t)i
        );

        if (target == myIP)      continue;
        if (target == myGateway) continue;

        // Print progress every 10 hosts so the serial monitor shows the
        // scan is alive. Skipped hosts (own IP, gateway) are not counted.
        if (i % 10 == 0) {
            Serial.printf("[Discovery] Port scan: probing .%d–.%d...\n",
                          i, min(i + 9, 254));
        }

        if (client.connect(target, PORTSCAN_PORT, PORTSCAN_TIMEOUT_MS)) {
            client.stop();
            candidates[foundCount++] = target;
            Serial.printf("[Discovery] Port scan: open port at %s\n",
                          target.toString().c_str());
        }
    }

    if (foundCount == 0) {
        Serial.println("[Discovery] Port scan: nothing found");
        return result;
    }

    // Pick the candidate with the lowest last-octet (servers typically have
    // low-numbered IPs on home/office networks, e.g. .1, .2, .10)
    IPAddress chosen = candidates[0];
    for (int i = 1; i < foundCount; i++) {
        if (candidates[i][3] < chosen[3]) chosen = candidates[i];
    }

    chosen.toString().toCharArray(result.host, sizeof(result.host));
    result.port   = PORTSCAN_PORT;
    result.method = DiscoveryMethod::PORTSCAN;
    Serial.printf("[Discovery] Port scan: selected %s:%d (%d candidate(s))\n",
                  result.host, result.port, foundCount);
    return result;
}


// ── discoverBroker — main entry point ─────────────────────────────────────────
// Runs the three-step chain. `storedUrl` is the mqtt_broker_url from the
// active credential bundle, used as the Step 3 fallback.
BrokerResult discoverBroker(const char* storedUrl) {
    BrokerResult result;

    // Step 0: cached addresses from previous successful connections (fast)
    result = tryCachedBrokers();
    if (result.found()) return result;

#if BROKER_DISCOVERY_ENABLED

    // Step 0.5: ask a sibling via ESP-NOW (fast, no broker-side config needed)
    result = tryEspNowBroker();
    if (result.found()) return result;

    result = tryMdnsDiscovery();
    if (result.found()) return result;

    result = tryPortScan();
    if (result.found()) return result;

#else
    Serial.println("[Discovery] Auto-discovery disabled — using stored URL");
#endif

    // Step 3: parse stored URL as fallback
    Serial.printf("[Discovery] Step 3: stored URL: %s\n", storedUrl);
    String url(storedUrl);
    int schemeEnd = url.indexOf("://");
    if (schemeEnd >= 0) url = url.substring(schemeEnd + 3);
    int colon = url.lastIndexOf(':');
    if (colon > 0) {
        url.substring(0, colon).toCharArray(result.host, sizeof(result.host));
        result.port = (uint16_t)url.substring(colon + 1).toInt();
    } else {
        url.toCharArray(result.host, sizeof(result.host));
        result.port = 1883;
    }
    result.method = DiscoveryMethod::STORED;
    return result;
}
