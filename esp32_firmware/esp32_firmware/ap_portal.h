#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include "config.h"
#include "credentials.h"
#include "app_config.h"
#include "device_id.h"

// =============================================================================
// ap_portal.h  —  Configuration portals
//
// TWO PORTAL MODES — same WebServer, different contexts:
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ MODE 1: AP Mode (apPortalStart)                                          │
// │  Entered when no credentials exist or all retries fail.                  │
// │  Device creates a Wi-Fi access point "ESP32-Config-xxxxxxxxxxxx".        │
// │  Admin connects to that AP and browses to 192.168.4.1.                   │
// │                                                                           │
// │  GET  /           Full setup form — Wi-Fi, MQTT, GitHub, rotation key    │
// │  POST /save       Saves all fields, restarts                             │
// │  GET  /status     JSON device info                                        │
// └─────────────────────────────────────────────────────────────────────────┘
//
// ┌─────────────────────────────────────────────────────────────────────────┐
// │ MODE 2: Settings Mode (settingsServerStart / settingsServerStop)         │
// │  Available when already connected to Wi-Fi (OPERATIONAL state).          │
// │  Triggered by an MQTT command on .../cmd/config_mode.                    │
// │  Device starts a temporary HTTP server on its STA IP (same network       │
// │  as the admin's browser — no need to switch Wi-Fi networks).             │
// │                                                                           │
// │  GET  /settings   Settings-only form — GitHub + MQTT hierarchy           │
// │  POST /settings   Saves GitHub + MQTT settings, does NOT restart         │
// │  GET  /status     JSON device info                                        │
// └─────────────────────────────────────────────────────────────────────────┘
//
// SECURITY: Both portals are plain HTTP (no TLS). Security relies on
// physical/network proximity. AP mode requires connecting to the device's
// own Wi-Fi network. Settings mode requires being on the same LAN.
//
// GITHUB CREDENTIALS:
// GitHub owner and repo are entered through these portals — never hardcoded
// in config.h or committed to the repository. The full setup form (Mode 1)
// requires them before it will save. The settings form (Mode 2) can update
// them independently at any time without re-entering Wi-Fi credentials.
// =============================================================================

static WebServer _apServer(80);   // Shared server instance for both modes


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

// Shared CSS and page header — used by both forms to keep HTML size down
// Shared CSS injected into both HTML forms.
// Kept as a const char[] (not String) to stay in flash rather than RAM.
static const char PAGE_STYLE[] =
    "body{font-family:Arial,sans-serif;max-width:520px;margin:40px auto;padding:0 16px}"
    "input{width:100%;box-sizing:border-box;padding:8px;margin:4px 0 12px;"
           "border:1px solid #ccc;border-radius:4px}"
    "button{background:#2E4057;color:#fff;padding:10px 20px;border:none;"
            "border-radius:4px;cursor:pointer;width:100%;margin-top:8px}"
    "h2{color:#2E4057}h3{color:#048A81;margin-top:20px;margin-bottom:4px}"
    ".note{font-size:12px;color:#666;margin:-8px 0 12px}"
    ".ok{background:#e8f5e9;padding:12px;border-radius:4px;color:#2e7d32}";


// ── GET /status — shared by both modes ─────────────────────────────────────────
// Returns a JSON snapshot of device identity, firmware version, LAN IP,
// and the active GitHub owner/repo. Useful for Node-RED dashboards and
// for confirming which physical device you are connected to.
static void apHandleStatus() {
    String json = String("{\"device_id\":\"")  + DeviceId::get()   + "\""
                + ",\"mac\":\""                + DeviceId::getMac() + "\""
                + ",\"firmware_version\":\"" FIRMWARE_VERSION "\""
                + ",\"firmware_ts\":"          + String((uint32_t)FIRMWARE_BUILD_TIMESTAMP)
                + ",\"ip\":\""                 + WiFi.localIP().toString() + "\""
                + ",\"github_owner\":\""       + gAppConfig.github_owner + "\""
                + ",\"github_repo\":\""        + gAppConfig.github_repo  + "\""
                + "}";
    _apServer.send(200, "application/json", json);
}


// =============================================================================
// MODE 1 — AP Mode portal (full setup form)
// =============================================================================

// ── GET / — Full setup form (AP mode only) ───────────────────────────────────────
// Serves all configuration fields in one page:
//   Section 1: Wi-Fi SSID + password
//   Section 2: MQTT broker URL, username, password
//   Section 3: GitHub owner + repo (required — never stored in firmware source)
//   Section 4: MQTT ISA-95 topic hierarchy (pre-filled with current gAppConfig values)
//   Section 5: Rotation key (optional AES key for MQTT credential rotation)
// GitHub owner and repo are pre-filled with gAppConfig values (from NVS or config.h defaults)
// so that returning admins see what is currently configured.
static void apHandleRoot() {
    String html = String("<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Setup</title>"
        "<style>") + PAGE_STYLE + "</style></head><body>"
        "<h2>ESP32 Device Setup</h2>"
        "<form method='POST' action='/save'>"

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

        "<h3>GitHub OTA</h3>"
        "<p class='note'>Required for automatic firmware updates. "
            "Never commit these values to your repository.</p>"
        "<label>GitHub Owner / Organisation *</label>"
        "<input name='github_owner' value='" + String(gAppConfig.github_owner) + "' required>"
        "<label>GitHub Repository *</label>"
        "<input name='github_repo' value='" + String(gAppConfig.github_repo) + "' required>"

        "<h3>MQTT Topic Hierarchy</h3>"
        "<p class='note'>ISA-95 path: Enterprise/Site/Area/Line/Cell/DeviceType/DeviceId/...</p>"
        "<label>Enterprise</label>"
        "<input name='mq_enterprise' value='" + String(gAppConfig.mqtt_enterprise) + "'>"
        "<label>Site</label>"
        "<input name='mq_site' value='" + String(gAppConfig.mqtt_site) + "'>"
        "<label>Area</label>"
        "<input name='mq_area' value='" + String(gAppConfig.mqtt_area) + "'>"
        "<label>Line</label>"
        "<input name='mq_line' value='" + String(gAppConfig.mqtt_line) + "'>"
        "<label>Cell</label>"
        "<input name='mq_cell' value='" + String(gAppConfig.mqtt_cell) + "'>"
        "<label>Device Type</label>"
        "<input name='mq_devtype' value='" + String(gAppConfig.mqtt_device_type) + "'>"

        "<h3>Security</h3>"
        "<label>Rotation Key <span class='note'>(32 hex chars = 16 bytes, optional)</span></label>"
        "<input name='rotation_key' maxlength='32' "
               "placeholder='e.g. 0102030405060708090a0b0c0d0e0f10'>"

        "<button type='submit'>Save &amp; Restart</button>"
        "</form></body></html>";
    _apServer.send(200, "text/html", html);
}


// ── POST /save — Full setup, save all fields, restart ─────────────────────────
static void apHandleSave() {
    String ssid = _apServer.arg("wifi_ssid");
    String murl = _apServer.arg("mqtt_broker_url");
    String ghOwner = _apServer.arg("github_owner");
    String ghRepo  = _apServer.arg("github_repo");

    // All four fields are required — reject early with a clear message.
    // GitHub fields are required here (not optional) to prevent the device
    // being saved with placeholder values that would cause OTA to fail.
    if (ssid.isEmpty() || murl.isEmpty() || ghOwner.isEmpty() || ghRepo.isEmpty()) {
        _apServer.send(400, "text/plain",
            "Error: wifi_ssid, mqtt_broker_url, github_owner and github_repo are required.");
        return;
    }

    // ── Save credentials ──────────────────────────────────────────────────────
    CredentialBundle b;
    ssid.toCharArray(b.wifi_ssid,       sizeof(b.wifi_ssid));
    _apServer.arg("wifi_password") .toCharArray(b.wifi_password,   sizeof(b.wifi_password));
    murl.toCharArray(b.mqtt_broker_url, sizeof(b.mqtt_broker_url));
    _apServer.arg("mqtt_username") .toCharArray(b.mqtt_username,   sizeof(b.mqtt_username));
    _apServer.arg("mqtt_password") .toCharArray(b.mqtt_password,   sizeof(b.mqtt_password));

    String rotHex = _apServer.arg("rotation_key");
    if (rotHex.length() == 32) {
        for (int i = 0; i < 16; i++) {
            char hex[3] = { rotHex[i*2], rotHex[i*2+1], 0 };
            b.rotation_key[i] = (uint8_t)strtol(hex, nullptr, 16);
        }
    }
    b.timestamp = FIRMWARE_BUILD_TIMESTAMP;
    b.source    = CredSource::ADMIN;

    if (!CredentialStore::save(b)) {
        _apServer.send(500, "text/plain", "Error: failed to save credentials to NVS.");
        return;
    }

    // ── Save app config (GitHub + MQTT hierarchy) ─────────────────────────────
    AppConfig cfg;
    ghOwner.toCharArray(cfg.github_owner,    sizeof(cfg.github_owner));
    ghRepo.toCharArray(cfg.github_repo,      sizeof(cfg.github_repo));
    _apServer.arg("mq_enterprise").toCharArray(cfg.mqtt_enterprise,  sizeof(cfg.mqtt_enterprise));
    _apServer.arg("mq_site")      .toCharArray(cfg.mqtt_site,        sizeof(cfg.mqtt_site));
    _apServer.arg("mq_area")      .toCharArray(cfg.mqtt_area,        sizeof(cfg.mqtt_area));
    _apServer.arg("mq_line")      .toCharArray(cfg.mqtt_line,        sizeof(cfg.mqtt_line));
    _apServer.arg("mq_cell")      .toCharArray(cfg.mqtt_cell,        sizeof(cfg.mqtt_cell));
    _apServer.arg("mq_devtype")   .toCharArray(cfg.mqtt_device_type, sizeof(cfg.mqtt_device_type));

    // Fill any blank MQTT fields with current defaults so nothing breaks
    if (strlen(cfg.mqtt_enterprise) == 0) strncpy(cfg.mqtt_enterprise, MQTT_ENTERPRISE, sizeof(cfg.mqtt_enterprise)-1);
    if (strlen(cfg.mqtt_site)       == 0) strncpy(cfg.mqtt_site,       MQTT_SITE,       sizeof(cfg.mqtt_site)-1);
    if (strlen(cfg.mqtt_area)       == 0) strncpy(cfg.mqtt_area,       MQTT_AREA,       sizeof(cfg.mqtt_area)-1);
    if (strlen(cfg.mqtt_line)       == 0) strncpy(cfg.mqtt_line,       MQTT_LINE,       sizeof(cfg.mqtt_line)-1);
    if (strlen(cfg.mqtt_cell)       == 0) strncpy(cfg.mqtt_cell,       MQTT_CELL,       sizeof(cfg.mqtt_cell)-1);
    if (strlen(cfg.mqtt_device_type)== 0) strncpy(cfg.mqtt_device_type,MQTT_DEVICE_TYPE,sizeof(cfg.mqtt_device_type)-1);

    if (!AppConfigStore::save(cfg)) {
        _apServer.send(500, "text/plain", "Error: failed to save app config to NVS.");
        return;
    }

    _apServer.send(200, "text/plain", "All settings saved. Restarting...");
    Serial.println("[AP Portal] Settings saved — restarting in 2 s");
    delay(2000);
    ESP.restart();
}


// ── apPortalStart — AP mode, blocks forever ────────────────────────────────────
void apPortalStart() {
    String deviceId = getDeviceId();
    String uuidSuffix = deviceId.length() >= 12
                        ? deviceId.substring(deviceId.length() - 12)
                        : deviceId;
    String ssid = String(AP_SSID_PREFIX) + uuidSuffix;

    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid.c_str(), AP_PASSWORD);
    Serial.printf("[AP Portal] SSID: %s  IP: %s\n", ssid.c_str(), AP_LOCAL_IP);

    _apServer.on("/",       HTTP_GET,  apHandleRoot);
    _apServer.on("/save",   HTTP_POST, apHandleSave);
    _apServer.on("/status", HTTP_GET,  apHandleStatus);
    _apServer.begin();

    Serial.println("[AP Portal] Waiting for admin — browse to 192.168.4.1");
    while (true) {
        _apServer.handleClient();
        delay(10);
    }
}


// =============================================================================
// MODE 2 — Settings server (runs over existing STA Wi-Fi connection)
// Triggered by MQTT cmd/config_mode. Admin browses to the device's LAN IP.
// Does NOT restart after save — MQTT hierarchy updates take effect immediately;
// GitHub settings take effect on next OTA check.
// =============================================================================

static bool _settingsServerRunning = false;

// ── GET /settings — Settings-only form (STA mode) ───────────────────────────────
// Served over the existing Wi-Fi STA connection (device LAN IP, port 80).
// Shows a form pre-filled with the current gAppConfig values so the admin
// can see what is set and only change what needs updating.
// Does NOT show Wi-Fi credentials — those are only changed in AP mode or
// via MQTT credential rotation.
static void settingsHandleGet() {
    String ip = WiFi.localIP().toString();
    String html = String("<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ESP32 Settings</title>"
        "<style>") + PAGE_STYLE + "</style></head><body>"
        "<h2>ESP32 Settings</h2>"
        "<p class='note'>Device ID: " + DeviceId::get() + "<br>"
                         "IP: " + ip + "</p>"

        "<form method='POST' action='/settings'>"

        "<h3>GitHub OTA</h3>"
        "<p class='note'>Never commit these values to your repository.</p>"
        "<label>GitHub Owner / Organisation *</label>"
        "<input name='github_owner' value='" + String(gAppConfig.github_owner) + "' required>"
        "<label>GitHub Repository *</label>"
        "<input name='github_repo'  value='" + String(gAppConfig.github_repo)  + "' required>"

        "<h3>MQTT Broker</h3>"
        "<p class='note'>Changing these requires a restart to reconnect to the broker.</p>"
        "<label>Broker URL *</label>"
        "<input name='mqtt_broker_url' value='" + String("") + "' "
               "placeholder='leave blank to keep current' >"
        "<label>Username</label>"
        "<input name='mqtt_username' placeholder='leave blank to keep current'>"
        "<label>Password</label>"
        "<input name='mqtt_password' type='password' placeholder='leave blank to keep current'>"

        "<h3>MQTT Topic Hierarchy</h3>"
        "<p class='note'>ISA-95: Enterprise/Site/Area/Line/Cell/DeviceType/DeviceId/...</p>"
        "<label>Enterprise</label>"
        "<input name='mq_enterprise' value='" + String(gAppConfig.mqtt_enterprise) + "'>"
        "<label>Site</label>"
        "<input name='mq_site'       value='" + String(gAppConfig.mqtt_site)       + "'>"
        "<label>Area</label>"
        "<input name='mq_area'       value='" + String(gAppConfig.mqtt_area)       + "'>"
        "<label>Line</label>"
        "<input name='mq_line'       value='" + String(gAppConfig.mqtt_line)       + "'>"
        "<label>Cell</label>"
        "<input name='mq_cell'       value='" + String(gAppConfig.mqtt_cell)       + "'>"
        "<label>Device Type</label>"
        "<input name='mq_devtype'    value='" + String(gAppConfig.mqtt_device_type)+ "'>"

        "<button type='submit'>Save Settings</button>"
        "</form></body></html>";
    _apServer.send(200, "text/html", html);
}


// ── POST /settings — Save GitHub + MQTT settings, no restart ──────────────────
static void settingsHandlePost() {
    String ghOwner = _apServer.arg("github_owner");
    String ghRepo  = _apServer.arg("github_repo");

    if (ghOwner.isEmpty() || ghRepo.isEmpty()) {
        _apServer.send(400, "text/plain",
            "Error: github_owner and github_repo are required.");
        return;
    }

    // Build updated AppConfig — start from current values then overlay changes
    AppConfig cfg;
    memcpy(&cfg, &gAppConfig, sizeof(AppConfig));

    ghOwner.toCharArray(cfg.github_owner, sizeof(cfg.github_owner));
    ghRepo.toCharArray(cfg.github_repo,   sizeof(cfg.github_repo));

    String mq_ent  = _apServer.arg("mq_enterprise");
    String mq_site = _apServer.arg("mq_site");
    String mq_area = _apServer.arg("mq_area");
    String mq_line = _apServer.arg("mq_line");
    String mq_cell = _apServer.arg("mq_cell");
    String mq_dev  = _apServer.arg("mq_devtype");

    if (mq_ent.length()  > 0) mq_ent.toCharArray(cfg.mqtt_enterprise,   sizeof(cfg.mqtt_enterprise));
    if (mq_site.length() > 0) mq_site.toCharArray(cfg.mqtt_site,        sizeof(cfg.mqtt_site));
    if (mq_area.length() > 0) mq_area.toCharArray(cfg.mqtt_area,        sizeof(cfg.mqtt_area));
    if (mq_line.length() > 0) mq_line.toCharArray(cfg.mqtt_line,        sizeof(cfg.mqtt_line));
    if (mq_cell.length() > 0) mq_cell.toCharArray(cfg.mqtt_cell,        sizeof(cfg.mqtt_cell));
    if (mq_dev.length()  > 0) mq_dev.toCharArray(cfg.mqtt_device_type,  sizeof(cfg.mqtt_device_type));

    // MQTT broker URL / username / password are optional on this form.
    // Blank fields mean "keep the current value" — the admin does not need to
    // re-enter the password just to change the topic hierarchy or GitHub repo.
    // If any of the three MQTT fields are non-blank, load the current bundle,
    // overlay the new values, and save it back to NVS.
    String newMurl = _apServer.arg("mqtt_broker_url");
    String newMusr = _apServer.arg("mqtt_username");
    String newMpwd = _apServer.arg("mqtt_password");
    bool mqttChanged = false;

    if (!newMurl.isEmpty() || !newMusr.isEmpty() || !newMpwd.isEmpty()) {
        // Load current cred bundle, overlay new values, save back
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
        _apServer.send(500, "text/plain", "Error: failed to save settings to NVS.");
        return;
    }

    String msg = String("<div class='ok'>Settings saved.<br>")
               + "GitHub: " + cfg.github_owner + "/" + cfg.github_repo + "<br>"
               + "MQTT hierarchy: " + cfg.mqtt_enterprise + "/" + cfg.mqtt_site
               + "/" + cfg.mqtt_area + "/" + cfg.mqtt_line + "/" + cfg.mqtt_cell
               + "/" + cfg.mqtt_device_type;
    if (mqttChanged) msg += "<br>MQTT broker credentials updated — restart required to reconnect.";
    msg += "</div>";

    // Serve a confirmation page with the saved values
    String html = String("<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Saved</title>"
        "<style>") + PAGE_STYLE + "</style></head><body>"
        "<h2>Settings Saved</h2>" + msg +
        "<br><a href='/settings'>Back to settings</a></body></html>";
    _apServer.send(200, "text/html", html);

    Serial.println("[Settings] App config updated via settings portal");
}


// ── settingsServerStart — start HTTP settings portal on the LAN interface ────────
// Called from the MQTT onMqttMessage handler when a cmd/config_mode message
// is received on the device's command topic.
//
// The device stays fully connected to Wi-Fi and MQTT — the settings server
// runs alongside them on the same STA interface, port 80.
//
// The admin browses to http://<device-LAN-IP>/settings from any device on
// the same network. No need to disconnect from the LAN or join the AP SSID.
// The device publishes a "config_mode_active" status message to MQTT containing
// the clickable settings_url so Node-RED can surface a link in a dashboard.
void settingsServerStart() {
    if (_settingsServerRunning) return;   // Already running

    _apServer.on("/settings", HTTP_GET,  settingsHandleGet);
    _apServer.on("/settings", HTTP_POST, settingsHandlePost);
    _apServer.on("/status",   HTTP_GET,  apHandleStatus);
    _apServer.begin();
    _settingsServerRunning = true;

    Serial.printf("[Settings] Settings portal started — browse to http://%s/settings\n",
                  WiFi.localIP().toString().c_str());
}


// ── settingsServerTick — drive the settings server HTTP event loop ────────────────
// Must be called on every loop() iteration.
// When the settings portal is active, calls WebServer::handleClient() to
// process any pending HTTP connections. Returns true while active, false when
// the portal has not been started (or after settingsServerStop() is called).
// The call is a no-op when inactive, so it is cheap to call unconditionally.
bool settingsServerTick() {
    if (!_settingsServerRunning) return false;
    _apServer.handleClient();
    return true;
}


// ── settingsServerStop — shut down the settings portal ─────────────────────────
// Stops the WebServer and clears the running flag. Call this if you want to
// free port 80 after configuration is complete. Currently not called
// automatically — the portal stays active until the device restarts.
void settingsServerStop() {
    if (!_settingsServerRunning) return;
    _apServer.stop();
    _settingsServerRunning = false;
    Serial.println("[Settings] Settings portal stopped");
}
