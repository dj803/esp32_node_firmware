# ESP32 Credential Bootstrap Firmware

Firmware for ESP32-WROOM-32 nodes implementing zero-touch credential provisioning,
MQTT connectivity, and automatic OTA updates from GitHub Releases.

See the full technical specification (`ESP32_Credential_Bootstrap_Spec_v0.3.docx`) for details.

---

## File Structure

```
esp32_firmware/
├── esp32_firmware.ino      # Main sketch — state machine
├── config.h                # All compile-time constants — edit before flashing
├── credentials.h           # CredentialBundle struct + NVS storage helpers
├── crypto.h                # ECDH key agreement + AES-128-GCM (mbedTLS)
├── espnow_bootstrap.h      # Bootstrap: request credentials from a sibling
├── espnow_responder.h      # Operational: serve credentials to siblings
├── ap_portal.h             # AP mode HTTP configuration portal
├── mqtt_client.h           # AsyncMqttClient wrapper, topics, credential rotation
└── ota.h                   # GitHub Releases OTA check and HTTPUpdate flash

.github/
└── workflows/
    └── build.yml           # Arduino CLI build + release workflow
```

---

## Quick Start

### 1. Edit config.h

Before flashing, set your deployment values in `config.h`:

```cpp
#define MQTT_ENTERPRISE    "YourCompany"
#define MQTT_SITE          "YourSite"
#define MQTT_AREA          "YourArea"
#define MQTT_LINE          "Line1"
#define MQTT_CELL          "Cell1"
#define MQTT_DEVICE_TYPE   "ESP32Sensor"

#define OTA_JSON_URL       "https://your-github-username.github.io/your-repo-name/ota.json"
```

### 2. Install Required Libraries

In Arduino IDE → Library Manager:
- **ArduinoJson** (v7)

Install manually (download ZIP and add via Sketch → Include Library → Add .ZIP):
- **AsyncTCP**: https://github.com/me-no-dev/AsyncTCP
- **AsyncMqttClient**: https://github.com/marvinroger/async-mqtt-client
- **ESP32-OTA-Pull**: https://github.com/mikalhart/ESP32-OTA-Pull

### 3. Board Settings

- Board: **ESP32 Dev Module**
- Partition Scheme: **Default 4MB with spiffs** (required for OTA)
- Upload Speed: 921600

### 4. Flash the First Node

Flash via Arduino IDE (Upload button). On first boot the node has no credentials
and will enter **AP mode** automatically.

Connect your phone to `ESP32-Config-XXXX` (password: `password`) and navigate
to `http://192.168.4.1` to enter Wi-Fi and MQTT credentials.

### 5. Bootstrap Subsequent Nodes

Flash additional nodes and power them on. They will automatically request
credentials from the first provisioned node via ESP-NOW and connect without
any manual configuration.

---

## OTA Updates

1. Push a tag to GitHub matching `v*.*.*`:
   ```bash
   git tag v1.1.0
   git push origin v1.1.0
   ```
2. GitHub Actions compiles the sketch and attaches `firmware.bin` to the release.
3. All operational nodes detect the new version within `OTA_CHECK_INTERVAL_MS`
   (default 1 hour) and update automatically.
4. To trigger an immediate update, publish any message to:
   ```
   [Enterprise]/[Site]/[Area]/[Line]/[Cell]/[DeviceType]/[DeviceId]/cmd/ota_check
   ```

---

## MQTT Topics

All topics follow the ISA-95 / Unified Namespace pattern:

```
[Enterprise]/[Site]/[Area]/[Line]/[Cell]/[DeviceType]/[DeviceId]/[Prefix]
```

| Prefix           | Direction     | Purpose                          |
|------------------|---------------|----------------------------------|
| `status`         | Device → MQTT | Boot announcement, heartbeat, OTA events |
| `telemetry`      | Device → MQTT | Application sensor data          |
| `cmd`            | MQTT → Device | General commands                 |
| `cmd/cred_rotate`| MQTT → Device | Credential rotation bundle       |
| `cmd/ota_check`  | MQTT → Device | Trigger immediate OTA check      |
| `cmd/led`        | MQTT → Device | WS2812B strip control — colour / brightness / animation / count / off / reset (see [docs/led_control.md](docs/led_control.md)) |
| `cmd/locate`     | MQTT → Device | Flash status LED for 4 s (ignored payload) — physical locate |
| `status/led`     | Device → MQTT | Retained LED strip state — state, r/g/b, brightness, count, uptime |
| `cmd/rfid/program` | MQTT → Device | Arm next scan for a raw-hex multi-block write (see [docs/rfid_tag_profiles.md](docs/rfid_tag_profiles.md)) |
| `cmd/rfid/read_block` | MQTT → Device | Arm next scan for a single-block read |
| `cmd/rfid/cancel` | MQTT → Device | Cancel any pending arm           |
| `telemetry/rfid` | Device → MQTT | Scan event — uid, profile, card_type, authorized |
| `response`       | Device → MQTT | Command acknowledgements (incl. RFID program/read outcomes) |

---

## Security Notes

- Credentials are exchanged over ESP-NOW using ECDH (Curve25519) + AES-128-GCM.
- The AP portal uses a fixed default password — change `AP_PASSWORD` in `config.h`
  for production deployments.
- OTA updates are authenticated via HTTPS (ISRG Root X1 CA). Firmware signature
  verification is a planned v2 feature.
- Plaintext credentials are never logged to Serial at any log level.
