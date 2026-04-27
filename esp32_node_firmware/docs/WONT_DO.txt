ESP32 Node Firmware — Won't Do
================================
Decisions recorded here are intentional choices NOT to implement a previously
audited improvement. Keep this file so future audits don't re-flag these items
as missing work.

────────────────────────────────────────────────────────────────────────────────

1. AP password hardcoded as "password"  (was audit item #2 in DEFERRED)
   File:    include/config.h line 127
   Decided: 2026-04-21

   Rationale: AP mode is only entered during first provisioning or after all
   other credential sources have failed. Physical proximity is already required
   (the device broadcasts its own Wi-Fi SSID). The admin workflow documents the
   default password, and changing it per-device would force operators to check
   MAC-derived strings on the device's serial console or a sticker — neither
   available during field deployments. The trade-off of a known default
   password is accepted.

   If the threat model changes (e.g. deployment in a hostile RF environment
   or on a contested wireless spectrum), revisit.

────────────────────────────────────────────────────────────────────────────────

2. Node-RED Dashboard httpNodeAuth  (was audit item #19 in DEFERRED)
   File:    ~/.node-red/settings.js  line 125 (commented-out template)
   Decided: 2026-04-21

   Rationale: Node-RED runs on a developer laptop on a private home LAN.
   The dashboard is not accessible from outside the local network, making
   mandatory authentication an unnecessary friction during development.
   The settings.js template is already in place — if the threat model
   changes (laptop used on a shared/public network), the operator can
   enable it in under two minutes:
     node -e "console.log(require('bcryptjs').hashSync('PASS', 8))"
     → uncomment line 125, substitute the hash, restart Node-RED.

   Do not auto-enable: the password choice is operator-specific and
   cannot be committed to the repo without exposing credentials.

────────────────────────────────────────────────────────────────────────────────

3. OTA HTTPS certificate pinning  (was SUGGESTED_IMPROVEMENTS #6)
   File:    include/ota.h (ESP32-OTA-Pull + HTTPClient stack)
   Decided: 2026-04-21 (review), confirmed park 2026-04-27

   Rationale: HTTPS encryption-in-transit is in place; what's missing is
   pinning the GitHub root CA. For the current internal-IoT threat model
   (private LAN, GitHub Pages as the OTA origin) the residual risk is a
   compromised GitHub root or a MITM on the LAN egress — both of which
   would already imply much larger problems. Adding pinning would also
   create a CA-rotation chore (GitHub has rotated before), which without
   automation becomes a tail risk in itself.

   If the OTA origin moves off github.com, OR a CA-rotation automation
   is built first, revisit.

────────────────────────────────────────────────────────────────────────────────

4. MQTT-over-TLS to the broker  (was SUGGESTED_IMPROVEMENTS #7)
   File:    include/mqtt_client.h + include/broker_discovery.h
   Decided: 2026-04-21 (review), confirmed park 2026-04-27

   Rationale: Plaintext MQTT CONNECT carries the broker username/password
   on the wire. The credential-rotation payload is already AES-128-GCM
   encrypted at the application layer, so the high-value secrets are
   protected. The fleet runs on a private LAN with the broker on a
   trusted host. Switching to AsyncMqttClient::setSecure(true) + pinned
   CA requires broker TLS config + Node-RED reconfig + per-device CA
   bundle management — not free, and the threat (LAN-segment passive
   eavesdropper) is small in this deployment.

   include/config.h has a one-line comment next to BROKER_DISCOVERY_ENABLED
   documenting this decision so it isn't quietly forgotten.

   If the deployment moves off a trusted wire segment (e.g. shared
   office VLAN, untrusted Wi-Fi, MQTT-over-WAN), revisit.

────────────────────────────────────────────────────────────────────────────────

5. UID-clone / "Chinese backdoor" MIFARE support  (was SUGGESTED_IMPROVEMENTS #13)
   Decided: 2026-04-27

   Rationale: Standard MIFARE Classic 1K refuses sector-0 writes — the
   UID is factory-burned. Clone silicon ("Gen1a/Gen2/CUID/Magic Cards")
   accepts special command sequences (via libraries like MFRC522Hack)
   to overwrite block 0 — useful for cloning UIDs onto blank cards or
   tag repair, but dangerous: one wrong write to the access-bits portion
   of the sector trailer bricks the card permanently. The library calls
   only work on specific clone silicon variants; on real-world MIFARE
   tags they fail silently or partially.

   No clear use case in the current deployment. If a tag-replacement
   workflow appears (e.g. operator wants a new blank to inherit a
   retired card's UID without re-enrolling everything downstream),
   revisit.

────────────────────────────────────────────────────────────────────────────────

6. NFC phone / card emulation  (was SUGGESTED_IMPROVEMENTS #18)
   Decided: 2026-04-27

   Rationale: ESP32 (Xtensa) cannot emulate an ISO 14443A tag — the
   hardware does not support card emulation mode on its NFC peripheral.
   ESP32-S3 with Host Card Emulation could, but that's a different chip
   from the one this firmware targets and would require parallel
   driver/protocol work for a feature with no defined deployment use.

   Out of scope for this firmware on this hardware. Would only revisit
   if the fleet migrates to ESP32-S3-class hardware AND a phone-tap
   identification workflow becomes a deployment requirement.
