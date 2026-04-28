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

────────────────────────────────────────────────────────────────────────────────

7. Node-RED admin API adminAuth  (was SUGGESTED_IMPROVEMENTS #68)
   File:    ~/.node-red/settings.js
   Decided: 2026-04-28

   Rationale: same threat-model logic as item 2 above — Node-RED is on a
   private LAN and the admin API at http://127.0.0.1:1880/flows is not
   reachable from outside. Adding adminAuth would require committing or
   side-channelling a password and would block the unauthenticated
   `Node-RED-Deployment-Type: nodes` flow-push pattern that CLAUDE.md
   relies on. Operator confirmed 2026-04-28: no admin password for now.

   If the threat model changes (laptop used on a shared/public network,
   or Node-RED ever exposed to the internet), enable adminAuth the same
   way as httpNodeAuth (item 2).

────────────────────────────────────────────────────────────────────────────────

8. Bootloader rollback safety net  (was SUGGESTED_IMPROVEMENTS #25)
   Setting: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
   File:    platformio.ini (custom_sdkconfig block, currently commented out)
   Decided: 2026-04-28 — parked at operator request after re-confirming
            the pioarduino blocker. Easy to revisit when upstream fixes.

   What it would do: freshly-OTA'd partitions enter PENDING_VERIFY state;
   bootloader auto-reverts after N consecutive boots if the app doesn't
   call esp_ota_mark_app_valid_cancel_rollback(). Closes the gap where
   a new firmware hard-crashes during early boot (before app_main runs)
   — Phase 2's NVS-flag rollback can't catch that.

   Why parked — the pioarduino blocker (tested 2026-04-23):
   Adding `custom_sdkconfig = CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
   triggers a ~22-min full pioarduino framework rebuild. The rebuild
   loses arduino-esp32's `__wrap_log_printf` symbol (used by
   NetworkClientSecure on our HTTPS OTA path). The wrap directive
   `-Wl,--wrap=log_printf` is normally injected by pioarduino's build
   script and doesn't survive `custom_sdkconfig`. Adding the flag
   manually doesn't help because the wrap *target* itself (Arduino's
   log_printf body) is missing from the rebuilt framework, not just
   the directive.

   Alternative paths considered:
     a) File a pioarduino issue and wait for upstream fix.
     b) Stub `__wrap_log_printf` ourselves — workaround, may break
        Arduino HAL log-level filtering.
     c) Switch to platform-espressif32 (official) — but it ships
        arduino-esp32 2.x, our code targets 3.x.
     d) Build a custom bootloader via raw ESP-IDF + flash-only-bootloader
        in CI — significant pipeline rework.

   Phase 2's runtime NVS-flag rollback covers ~95% of the safety-net
   value (catches "valid app boots but is broken"); the remaining gap
   (early-boot crash before NVS access) is rare and the existing
   bootloader does fall back to the previous valid app on a partition-
   validation failure.

   Revisit if/when:
     - pioarduino's custom_sdkconfig flow preserves the log_printf
       wrap (path (a) lands).
     - The fleet starts pushing builds without serial access (where
       a brick is unrecoverable without operator on-site).
     - OR an early-boot-crash incident actually fires in production.

────────────────────────────────────────────────────────────────────────────────

9. Recovery partition app  (was SUGGESTED_IMPROVEMENTS #26)
   File:    partitions.csv (would need a new "factory" or "recovery" row)
   Decided: 2026-04-28 — parked at operator request pending 8 MB flash
            module migration.

   What it would do: a small factory-partition app (WiFi + esp_https_ota
   only) that the bootloader falls back to when both ota_0 AND ota_1 are
   corrupted. Final-resort un-brick path that doesn't need serial access.

   Why parked — the flash-budget blocker:
   Math on a standard 4 MB ESP32-WROOM:
     nvs (20 K) + otadata (8 K) + recovery (~256 K min) +
     ota_0 (1.7 M) + ota_1 (1.7 M) = 3.69 M
     leaving 320 K for spiffs/coredump.

   Our main app is 1.83 M (97.6% of the 1.875 M ota slot). Shrinking
   to 1.7 M needs ~150 K of code reduction. Removing ESP32-OTA-Pull
   (still used for Pass-1 manifest fetch) saves ~30 K. Removing RFID or
   BLE saves more but is a feature regression. Net: doesn't fit on
   4 MB without compromising shipped features.

   Alternative paths considered:
     a) Move to 8 MB flash hardware (ESP32-WROOM-32E-N8 or ESP32-WROVER-IE).
        Recommended path — gives ~4 MB headroom and unlocks #12 (NTAG424
        DNA) and #25 (bootloader rollback) at the same time.
     b) Slim main app: drop ESP32-OTA-Pull, replace with custom manifest
        fetch (~50 lines using existing HTTPClient + ArduinoJson).
        Saves ~30 K. Combined with disabling unused FastLED palettes /
        NimBLE features, *might* reach 1.7 M but tight, and locks in
        ongoing flash-discipline work for every future feature.
     c) Skip recovery partition; rely on the existing bootloader fallback
        chain (otadata invalid → previous slot) plus Phase 2's runtime
        rollback. This is the current behaviour.

   Revisit if/when:
     - The fleet migrates to 8 MB flash modules (unblocks (a) and pairs
       naturally with the relay+Hall hardware refresh).
     - A device gets bricked in production where serial recovery isn't
       feasible (forces (b) or (a)).
     - OR fleet grows past ~10 devices, where the per-device unbrick
       cost (truck-roll + serial pin access) starts to dominate.
