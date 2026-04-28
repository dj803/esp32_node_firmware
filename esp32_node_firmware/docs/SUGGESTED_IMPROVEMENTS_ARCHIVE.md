ESP32 Node Firmware — Suggested Improvements (Infrastructure)
==============================================================
These items came out of the v0.3.03 audit but aren't on the committed
release path. Tackle opportunistically or when an adjacent fix makes one
of them cheap to bundle in.

────────────────────────────────────────────────────────────────────────────────

1. Upgrade GitHub Actions to Node.js 24            (addressed 2026-04-22)
   WHERE:    .github/workflows/build.yml (actions/checkout@v4, setup-python@v5,
             upload-artifact@v4, download-artifact@v4, softprops/action-gh-release@v2)
   DEADLINE: Node.js 20 is deprecated; hard cutoff June 2026. After that
             date, CI builds will fail without this upgrade.
   FIX:      Set FORCE_JAVASCRIPT_ACTIONS_TO_NODE24=true at workflow level
             (chosen approach — one-line workflow `env:` block). Once every
             action above publishes a native Node 24 build, the env var can
             be removed and individual actions bumped explicitly.

2. JTAG debugging setup                                (was audit item 20)
   Requires: ST-Link or ESP-Prog debug probe, JTAG pins wired on the dev board
   Benefit:  Step-through debugging via Cortex Debug in VS Code.
   Effort:   ~2h firmware config + hardware provisioning.

3. Reproducible builds                                 (was audit item 21)
   Problem:  FIRMWARE_BUILD_TIMESTAMP in config.h is a literal epoch value
             that changes per release, making binary output non-deterministic.
   Fix:      Replace with SOURCE_DATE_EPOCH env var in CI, or drop the field
             entirely and rely on the git SHA embedded in FIRMWARE_VERSION.

4. Dependency SBOM + supply-chain scan                 (was audit item 22)
   Add Dependabot (built-in) and/or Trivy against the platformio.ini lib_deps
   list. Alerts on CVEs in AsyncTCP, async-mqtt-client, NimBLE-Arduino, etc.

5. Local PlatformIO validation                 (addressed 2026-04-22)
   No local `pio run` / `pio test` was ever run during the v0.3.03 migration —
   CI was the only signal. Run `pip install platformio` on the dev machine and
   confirm `pio run -e esp32dev` + `pio test -e native` both pass locally, so
   future iterations don't round-trip through CI for trivial syntax errors.
   STATUS:   `pio run -e esp32dev` validated locally (Charlie + Bravo flashed
             from this machine on 2026-04-22). `pio test -e native` requires
             a local C/C++ host toolchain (gcc/g++), which is not installed
             on the current Windows dev box. Install MSYS2 mingw-w64 or
             chocolatey `mingw` to enable host tests; CI runs them on Ubuntu.
   WINDOWS GOTCHA: PlatformIO's click output handler uses CP1252 on Windows
             and crashes with UnicodeEncodeError on the Unicode progress bars
             that esptool emits during `--target upload`. The flash itself
             succeeds (esptool is a subprocess), but pio's output reader dies.
             Workaround: prefix every pio invocation with `set
             PYTHONIOENCODING=utf-8 &&` (cmd.exe) or `$env:PYTHONIOENCODING=
             "utf-8";` (PowerShell), or set it persistently via
             `setx PYTHONIOENCODING utf-8`.

6. OTA HTTPS certificate pinning                       (flagged 2026-04-21 review)
   WHERE:    include/ota.h (ESP32-OTA-Pull + HTTPClient stack)
   Current:  HTTPS encryption in transit but no root-CA verification. Trust
             anchor is implicit in whoever hosts the JSON filter file
             (GitHub Pages / Releases in current deployment).
   Fix:      Embed the GitHub root CA PEM as a build-time constant and pass
             to HTTPClient.setCACert() before calling the OTA-Pull library.
             Needs a CA-rotation story — GitHub has rotated before.
   Decided:  Left as-is for internal-IoT threat model. Revisit if the OTA
             origin moves off github.com or if a CA-rotation automation
             is in place.
   STATUS:   WONT_DO 2026-04-27 — moved to docs/WONT_DO.md entry 3.

7. MQTT-over-TLS to the broker                         (flagged 2026-04-21 review)
   WHERE:    include/mqtt_client.h + include/broker_discovery.h
   Current:  Plaintext CONNECT on LAN. Username/password on the wire.
             Credential-rotation payload is AES-128-GCM encrypted at the
             application layer, so the crown jewels are still safe, but
             the first-boot credentials are not.
   Fix:      Switch to AsyncMqttClient::setSecure(true) + a pinned CA.
             Requires broker TLS config + Node-RED reconfig. Not free.
   Decided:  Private LAN, same rationale as WONT_DO item 1 (AP password).
             Revisit if deployment moves off a trusted wire segment.
   Note:     config.h has a one-line comment next to BROKER_DISCOVERY_ENABLED
             documenting this decision so it isn't quietly forgotten.
   STATUS:   WONT_DO 2026-04-27 — moved to docs/WONT_DO.md entry 4.

────────────────────────────────────────────────────────────────────────────────
v0.3.15 — AP-mode recovery (rejected alternatives)         (flagged 2026-04-21)
────────────────────────────────────────────────────────────────────────────────
Context: v0.3.15 addressed a real incident where a router power-cycle pinned
all ESP32 nodes in AP mode until manually reset. The shipped fix was a
three-part change — indefinite exponential Wi-Fi backoff in OPERATIONAL,
background STA scan + auto-reconnect in AP mode, and a separate wifi-outage
restart counter. Three alternative approaches were considered and rejected
during planning; recording them here so the reasoning isn't lost.

8. Sibling ESP-NOW "come back online" broadcast
   IDEA:     When a node regains Wi-Fi, send an ESP-NOW hint so siblings
             stuck on lost-Wi-Fi can abort their backoff and retry.
   REJECTED: Fatal flaw against the observed incident — all siblings fell to
             AP mode simultaneously. ESP-NOW is deinit'd in AP mode; soft-AP
             locks the radio to the AP channel, not the router's Wi-Fi
             channel. No sibling is listening when any other sibling
             reconnects. Only helps staggered-recovery cases — the rarest
             failure mode. Shipped fix (C + A) already handles the common
             and LAN-wide scenarios without the complexity.
   STATUS:   WONT_DO 2026-04-28 — REJECTED design alternative kept here
             for posterity. Index moved to WONT_DO.

9. Time-limited AP mode → reboot
   IDEA:     Hard timeout on AP mode (e.g. 1 h); reboot unconditionally to
             retry OPERATIONAL.
   REJECTED: (A) — the background STA scan — already exits AP mode
             automatically when the router returns, with zero user
             disruption. A timer-based reboot interrupts admin provisioning
             mid-edit, regenerates the HTTPS cert on every cycle, churns
             ESP-NOW siblings, and papers over rather than fixes the root
             cause. Could be added later as a ~1 h last-ditch safety net
             if field data shows (A) is insufficient.
   STATUS:   WONT_DO 2026-04-28 — REJECTED design alternative kept here
             for posterity. Index moved to WONT_DO.

10. "Has ever successfully connected" NVS flag
    IDEA:    Distinguish first-boot unprovisioned devices from provisioned
             devices that are just having a bad Wi-Fi day, and delay AP
             entry for the latter.
    REJECTED: Subsumed by (C). With indefinite backoff in OPERATIONAL,
             provisioned devices very rarely reach AP mode from a router
             outage at all — the flag is solving a problem the primary fix
             already removed. Adds NVS state + a new code branch for
             negligible benefit.
    STATUS:  WONT_DO 2026-04-28 — REJECTED design alternative kept here
             for posterity. Index moved to WONT_DO.


================================================================================
RFID PLAYGROUND — v0.3.17 follow-ups
================================================================================

Tracked here because they were explicitly listed in the v0.3.17 plan's
"Explicitly NOT in scope" section. Pick these up as individual features once a
concrete use case emerges — the profile layer in include/rfid_types.h keeps
each one small and additive.

11. NTAG21x write support (NTAG213 / NTAG215 / NTAG216)
    Currently the profile string is recognised and can appear in telemetry,
    but the firmware's _rfidExecuteProgram() rejects non-MIFARE-Classic
    profiles with status "write_failed". Wire up MIFARE_Ultralight_Write on
    the 4-byte-page path and expose a page_size=4 entry in the Node-RED
    profile dropdown.

12. NTAG424 DNA secure messaging (AES)
    Separate feature branch — the MFRC522 transparent channel + AES-CCM
    secure-messaging framing is nontrivial. Rough effort: 1 sprint for a
    minimal read/write-page handler, plus key-derivation / session-state
    bookkeeping.

13. UID-clone / "Chinese backdoor" MIFARE support
    No MFRC522Hack, no sector-0 overwrite. Dangerous for non-experts and the
    library calls only work on specific clone silicon. Park here unless a
    clear use case appears.
    STATUS: WONT_DO 2026-04-27 — moved to docs/WONT_DO.md entry 5.

14. Per-sector key rotation from Node-RED (cmd/rfid/set_key)
    v0.3.17 writes data blocks only; sector trailers are hard-refused by the
    trailer_guard. Re-keying tags is a separate security story — needs
    audit + careful UI (one mis-entered access-bit byte bricks a tag).

15. NDEF encoding / decoding helpers
    Ship a small encoder that builds NDEF TLV records (URI, Text, MIME) and
    a decoder for the scan feed so Node-RED can present smartphone-readable
    tags natively. Layers cleanly on top of the ntag21x profile once (11)
    lands.

16. Preset "program this kind of tag" forms
    Operator ID / Machine ID / Free-form label / NDEF URL templates on top
    of the raw hex editor. Ship after a real operational pattern emerges —
    premature templates usually don't fit the actual workflow.

17. LF 125 kHz support (HID, EM4100, T5577)
    Different radio — requires an RDM6300 or similar. Not the MFRC522.
    Would live behind a compile-time LF_ENABLED flag mirroring RFID_ENABLED.

18. NFC phone / card emulation
    ESP32 (Xtensa) cannot emulate ISO 14443A as a tag. ESP32-S3 + host card
    emulation could — out of scope for this firmware.
    STATUS: WONT_DO 2026-04-27 — moved to docs/WONT_DO.md entry 6.

────────────────────────────────────────────────────────────────────────────────
LED CONTROL — v0.3.18 follow-ups
────────────────────────────────────────────────────────────────────────────────

19. Per-LED addressing
    The firmware currently drives the whole strip uniformly. Per-pixel
    control would need a new cmd/led "pixels" schema, a larger event
    payload (or separate topic), and a rework of the Core 1 renderer.
    Ship once a real use case appears (e.g. segment-of-8 progress bar).

20. Scene / preset save-to-device
    v0.3.18 presets live only in the Node-RED ui-template. Adding an NVS
    slot on the device would let the MQTT-less AP portal preview a scene
    too, and survive a Node-RED reinstall. Small feature — ~1 day.

21. Group / broadcast LED commands
    UI targets one device at a time. Support a "send to all readers" or
    multi-select — either client-side fan-out in Node-RED or a broadcast
    MQTT topic the firmware subscribes to (mirrors broadcast/cred_rotate).

22. Scheduled / time-of-day LED automation
    e.g. warm-white at dawn, red-dim at night. Out of scope for v0.3.18 —
    user can wire Node-RED inject nodes themselves if needed.

23. OTA / heartbeat LED override from Node-RED
    Today only LOCATE can overlay the status LED from outside. A generic
    "force pattern X for Y seconds" command would let Node-RED show
    alarm patterns for app-level events (door left open, sensor fault).

────────────────────────────────────────────────────────────────────────────────
OTA — v0.3.28 follow-ups                                   (flagged 2026-04-22)
────────────────────────────────────────────────────────────────────────────────

24. OTA transient freeze — add watchdog-safe progress timeout   (observed v0.3.28)
    OBSERVED:  During the v0.3.28 OTA field rollout, device Charlie froze on
               first attempt (no serial attached, device non-responsive).
               Worked cleanly on a manual reset + retry. Alpha and Bravo
               succeeded first-try. Root cause unknown — no serial log captured.
    CANDIDATES:
      a) Heap fragmentation at the moment of OTA. Charlie had likely been running
         longer / in a different heap state. BLE deinit freed the pool but mbed TLS
         handshake or lwIP buffer allocation may have hit a transient shortage.
      b) TCP stall mid-download. If the GitHub Pages CDN stalled and the progress
         callback stopped firing, the TWDT would eventually fire and reset — but
         the timeout (~3–5 s) may have looked like a freeze before the reset
         actually triggered.
      c) MQTT/async_tcp teardown race unique to Charlie's connection state at the
         time (e.g. mid-reconnect when OTA triggered, causing the _mqttOtaActive
         guard to hit an unexpected branch).
    SUGGESTED FIX:
      - Add a per-chunk deadline in the progress callback: if more than N seconds
        elapse between consecutive callback invocations, call ESP.restart() to
        abort and recover cleanly rather than relying on the TWDT alone.
      - Consider logging heap free/largest-block at OTA trigger time (before BLE
        deinit) so future freezes can be correlated with heap state.
      - If freeze recurs on Charlie specifically, capture serial during next OTA
        attempt to get the exact crash / stall point.
    PRIORITY:  Low — single occurrence, clean recovery on retry. Promote if it
               recurs on any device.
    STATUS:    Addressed 2026-04-22.
               - One-shot FreeRTOS timer `_otaProgressWatchdog` armed before the
                 download (ota.h). Each progress callback resets it; if no
                 callback fires for OTA_PROGRESS_TIMEOUT_MS (config.h, default
                 30 s), the timer task calls ESP.restart() to break the stall.
               - Trigger-time heap snapshot (free + largest block) now logged
                 immediately after "Update available" — before MQTT/BLE
                 teardown — so future freezes can be correlated with heap state
                 at the moment of OTA initiation, not just after teardown.

────────────────────────────────────────────────────────────────────────────────
OTA bulletproofing — v0.3.33 → v0.3.35 shipped, v0.4.0 deferred  (2026-04-23)
────────────────────────────────────────────────────────────────────────────────

Reference: see git log v0.3.32..v0.3.35 for the 3-phase OTA-bulletproofing
rollout that closed most of the gaps from the OTA-failure investigation.
Phases 1+2+3 shipped and are running on Alpha + Bravo + Charlie.

  Phase 1 (v0.3.33): pre-flight heap gate, hardcoded URL fallback chain,
                     boot_reason field in retained status, stage-tagged OTA
                     failure events, AP-mode 5-min idle timeout.
  Phase 2 (v0.3.34): NVS-flag-based post-OTA validation. The OLD firmware
                     writes the incoming version to NVS just before
                     ESP.restart(); the NEW firmware reads it on boot,
                     arms a 5-min validation deadline, and calls
                     esp_ota_mark_app_invalid_rollback_and_reboot() if
                     MQTT doesn't recover in time. Works without bootloader
                     ROLLBACK_ENABLE — manually marks the running partition
                     ABORTED and boots the previous valid OTA slot.
  Phase 3 (v0.3.35): replaced ESP32-OTA-Pull's blocking writer with
                     esp_https_ota for the actual flash. partial_http_download
                     enables HTTP Range support so a stalled download can
                     resume on retry. Custom ArduinoJson manifest re-fetch
                     to extract the .bin URL (ESP32-OTA-Pull doesn't expose
                     it after Pass 1).

────────────────────────────────────────────────────────────────────────────────
Phase 4 — DEFERRED items                                          (2026-04-23)
────────────────────────────────────────────────────────────────────────────────

25. Bootloader rollback safety net (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y)
    GAP:      Phase 2's NVS-flag rollback only fires if our app code runs
              far enough to read NVS and arm the deadline. If a new firmware
              hard-crashes during early boot (before app_main / setup),
              there's no auto-revert — the device boot-loops the bad app.
              The bootloader option closes this last gap by making OTA'd
              partitions enter PENDING_VERIFY state; bootloader auto-reverts
              after N consecutive boots without the app calling
              esp_ota_mark_app_valid_cancel_rollback().
    BLOCKED:  Tested 2026-04-23. Adding `custom_sdkconfig =
              CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` to platformio.ini
              triggers a full pioarduino framework rebuild (~22 min). The
              rebuild produces a framework that's missing arduino-esp32's
              __wrap_log_printf symbol — NetworkClientSecure (used by our
              HTTPS OTA path) calls log_printf which the platform normally
              wraps via -Wl,--wrap=log_printf to redirect through Arduino's
              HAL log macros. The wrap directive is injected by the
              platform's build script and is NOT preserved through the
              custom_sdkconfig rebuild. Adding -Wl,--wrap=log_printf to
              build_flags doesn't help because the wrap target itself
              (Arduino's log_printf implementation) is missing from the
              rebuilt framework, not just the wrap directive.
    REPRO:    1. Place a copy of min_spiffs.csv in esp32_node_firmware/
                 (pioarduino's custom_sdkconfig flow looks for it there).
              2. Uncomment the custom_sdkconfig block in platformio.ini.
              3. Run `pio run -e esp32dev`.
              4. Wait ~22 min for framework rebuild.
              5. Build fails at link step:
                 "undefined reference to __wrap_log_printf" (multiple sites
                 in NetworkClientSecure/src/ssl_client.cpp).
    PATHS:    a) File a pioarduino issue, wait for upstream fix.
              b) Provide our own __wrap_log_printf stub in main.cpp that
                 vfprintf's to Serial — workaround, may break Arduino HAL
                 log level filtering.
              c) Switch to platform-espressif32 (the official platform)
                 instead of pioarduino — but it ships arduino-esp32 2.x,
                 not the 3.x our code targets.
              d) Skip pioarduino's custom_sdkconfig and build a custom
                 bootloader manually via ESP-IDF + flash-only-bootloader
                 step in CI. Significant build pipeline rework.
    PRIORITY: Low — Phase 2 covers ~95% of the safety-net value. The
              remaining gap (early-boot crash) is rare and our existing
              bootloader does fall back to the previous valid app on a
              boot-time partition-validation failure (just doesn't auto-
              detect "valid app boots but is broken").

26. Recovery partition app
    GAP:      If BOTH ota_0 and ota_1 ever end up corrupted (very rare —
              would need a botched flash + power loss), the device is
              bricked. A recovery partition (factory app with WiFi +
              esp_https_ota only) gives the bootloader a known-good
              fallback that can re-flash the OTA slots from scratch.
    BLOCKED:  Doesn't fit on 4 MB flash with our current main app size.
              Math: nvs (20K) + otadata (8K) + recovery (~256K min) +
              ota_0 (1.7M) + ota_1 (1.7M) = 3.69M, leaving 320K for
              spiffs/coredump. But our main app is 1.83M (93% of the
              standard 1.875M ota slot), so shrinking to 1.7M needs
              ~150K of code reduction. Removing ESP32-OTA-Pull (still
              used for Pass 1 manifest fetch) saves ~30K. Removing RFID
              or BLE saves more but is a feature regression.
    PATHS:    a) Move to 8 MB flash hardware. Recommended.
              b) Slim main app: drop ESP32-OTA-Pull, replace with custom
                 manifest fetch (50 lines using existing HTTPClient +
                 ArduinoJson). Saves ~30 K. Combined with disabling
                 unused FastLED palettes / NimBLE features, might reach
                 1.7 M, but tight.
              c) Skip recovery partition, rely on the existing bootloader
                 fallback chain (otadata invalid → previous slot) plus
                 Phase 2's runtime rollback.
    PRIORITY: Low for current 3-device fleet (all serial-accessible).
              Promote when fleet grows past ~10 devices or when nodes
              are deployed without easy serial access.


────────────────────────────────────────────────────────────────────────────────
v0.3.36 → v0.4.01 — cross-cutting hardening Phase C / D follow-ups (2026-04-23)
────────────────────────────────────────────────────────────────────────────────

The v0.3.36 (Phase A concurrency hardening) + v0.4.01 (Phase B observability +
docs) milestones close most of the recurring v0.3.xx root-cause clusters.
These items are the architectural follow-ups that need a v0.4.x cycle:

27. Library-API regression test in CI
    GAP:      v0.3.03 PlatformIO migration only checks binary-size delta
              (was 4 KB, widened to 128 KB after Bluedroid 700 KB miss).
              Will not catch a future NimBLE 1→3 silent ABI break or
              AsyncMqttClient API drift. Lib SHAs are pinned (good) but
              there is no synthetic-API test that would catch a deliberate
              version bump quietly breaking a function signature.
    FIX:      Add a tiny test in test/test_native/ that exercises one
              symbol from each pinned lib (NimBLEDevice::init,
              AsyncMqttClient::setClientId, MFRC522::PCD_Init); CI fails
              to compile if any signature drifted. Bonus: re-pin SHA-pin
              comments with the version number they correspond to so a
              deliberate bump is intentional.
    PRIORITY: Medium — has bitten us once (NimBLE 1→2 in v0.2.10).

28. NVS / static-string lifetime audit + naming convention
    GAP:      Cluster 3 has bitten v0.1.7 (setClientId .c_str() dangling),
              v0.3.00, v0.3.11, v0.3.30 (LWT topic), v0.3.31. Each fix
              was reactive. Pattern: caller passes String::c_str() from
              a temporary; library stores the pointer; use-after-free at
              next call. Will recur the next time a developer adds an
              MQTT setter or httpd handler that takes const char*.
    FIX:      One-time sweep: every callsite that takes String::c_str()
              and passes to a library function — categorise as "library
              copies (safe)" or "library stores pointer (must outlive)".
              Add a naming convention (e.g. gMqttHostStatic, gLwtTopicStatic)
              for any string passed-by-pointer that must have static
              lifetime. Document in a new docs/STRING_LIFETIME.md.
    PRIORITY: Medium — pattern recurs roughly every major feature cycle.
    STATUS:   RESOLVED 2026-04-23 in v0.4.02. docs/STRING_LIFETIME.md
              codifies the convention; module-static `_mqttClientId`,
              `_mqttHost`, `_mqttWillTopic`, `_mqttBundle` are now
              labelled with `// LIFETIME: <api>` annotations in
              include/mqtt_client.h. Compile-time guard at
              include/lib_api_assert.h static_asserts the AsyncMqttClient
              setter signatures so a silent ABI drift fails the build.
              Re-audit 2026-04-28: every `.c_str()` callsite either uses
              a module-static String (safe) or a copy-style API like
              snprintf / publish / strncpy (also safe). No regressions.

29. WDT-heartbeat audit for all blocking I/O
    GAP:      v0.3.27/0.3.32 fixed OTA. Same risk shape lives in: broker
              discovery's AsyncTCP callbacks (v0.3.13 added detach but no
              per-result WDT kick), BLE onResult callback, MFRC522 SPI
              transactions during heavy WiFi.
    FIX:      Audit every blocking-or-async-callback site for "is the
              calling task subscribed to TWDT, and does this op exceed
              the timeout?". Document the per-task TWDT-subscription
              policy (which tasks ARE subscribed; which routinely block
              for long enough to need explicit feeds).
    PRIORITY: Low — no observed field issue outside OTA, but the
              architectural risk shape is identical.

30. AsyncTCP fork swap (marvinroger → mathieucarbou)
    Mathieucarbou's maintained fork has known leak fixes and is what
    production fleets standardise on. Risk: behaviour deltas in
    reconnect timing might surface new bugs. Defer to v0.4.x cycle
    that includes a soak test on Alpha first.
    PRIORITY: Low — current fork works. Promote when next fleet
              reliability incident traces to AsyncTCP.
    STATUS: RESOLVED 2026-04-27 (v0.4.14 cascade-fix). platformio.ini
    line 118 now pins https://github.com/mathieucarbou/AsyncTCP.git
    @v3.3.2. Used during the v0.4.13 → v0.4.20 cascade-fix marathon.
    Fleet-wide M2 + M3 chaos tests pass on the new fork. The use-after-
    free in lwIP raw_pcbs walk under Wi-Fi flap (#78) is independent of
    the fork choice and tracked separately.

31. Pin LED + RFID tasks to Core 1
    App tasks currently default to Core 0 alongside WiFi. Pinning LED
    to Core 1 + RFID to Core 1 frees Core 0 for network. Small change
    but RFID polling timing is sensitive — needs a careful smoke test.
    PRIORITY: Low — would help WiFi/BLE jitter under heavy LED render.

32. Heap-headroom gate at boot for each subsystem
    Mirrors v0.3.33's OTA preflight gate. Before bringing each
    subsystem up (BLE init, MQTT connect, etc.), check
    esp_get_free_internal_heap_size() against a per-subsystem
    threshold (~40 KB headroom for mbedTLS handshakes). Gracefully
    skip and re-try on the next loop.
    PRIORITY: Low — no observed OOM crashes in current fleet, but
              the OTA bulletproofing investigation flagged this as
              the highest-leverage single change for 4 MB classic
              ESP32 running all subsystems concurrently.

33. Versioned MQTT topic prefixes
    Enigma/JHBDev/... is unversioned; schema drift between firmware
    versions silently breaks Node-RED consumers. Future deployments
    should use v1/Enigma/.... Existing fleet would need a Node-RED
    bridge for backwards compat — significant deployment cost for
    marginal gain at 3 devices.
    PRIORITY: Low until fleet > 5 devices.

34. Captive portal DNS responder
    AP portal currently doesn't trigger iOS / Android captive sheets —
    admin must manually browse to 192.168.4.1. Optional UX win.
    PRIORITY: Low.

35. Operational practice: canary OTA pattern
    Even at 3 devices: OTA Alpha first, soak ~1 h, then OTA Bravo +
    Charlie. The v0.3.33-v0.3.35 rollout's Bravo/Charlie panic on
    v0.3.33→v0.3.35 OTA would have been caught earlier with this
    discipline. Not a code change — operational practice.
    STATUS:   Adopted as habit going forward.

    ──── Empirical case for canary, captured 2026-04-25 (v0.4.06 release) ────
    The v0.4.06 release (peer_tracker EMA-reseed-on-step-change) was
    initially deployed by triggering cmd/ota_check on all 3 devices at
    roughly the same moment. Within 10 minutes, all 3 devices were
    showing the broker their LWT (offline) and stayed silent.

    Investigation chain:
       1. Connected Charlie to PC USB / serial monitor at COM5 — Charlie
          eventually self-recovered, booting on v0.4.05 (rollback target).
          So the Phase-2 NVS-flag rollback safety net WORKED on Charlie:
          v0.4.06 firmware booted, ran far enough to start associating
          with the AP, but didn't reach MQTT within the 5-minute
          OTA_VALIDATION_DEADLINE_MS window. Rollback fired automatically.
       2. Followed by a single-device OTA test on Charlie (no other
          devices competing): v0.4.06 booted, WiFi associated on the
          FIRST attempt, MQTT connected, and otaValidationConfirmHealth()
          marked the app valid — total time from reset to validated under
          ~10 seconds.
       3. Conclusion: v0.4.06 firmware itself was fine. The fleet-wide
          rollback was caused by THREE devices simultaneously trying to
          associate with the same AP after their OTA reboots. The AP
          can typically only handle 1-2 simultaneous association
          requests cleanly — the third gets queued or rejected and burns
          its 15s WIFI_CONNECT_TIMEOUT, then retries (exhausting the
          3-attempt budget = 45s), then falls through to sibling
          re-verify (which also fails because siblings are in the same
          state), then enters AP-mode portal. Once in AP-mode, MQTT is
          unreachable until either the 5-min admin-idle timeout fires or
          the operator visits the portal. Either way, the
          OTA_VALIDATION_DEADLINE_MS=300000 (5 min) elapses and rollback
          fires.

    Implication for current architecture (DO NOT extend OTA validation
    timeout):
       The 5-min validation timeout is correct and should NOT be
       lengthened just to mask this. Lengthening it would mean a real
       firmware fault that prevents MQTT-reach takes longer to detect
       and roll back. The fix is to ensure only ONE device is in the
       post-OTA-reboot WiFi-association critical window at a time.

    Recommended OTA discipline (this is now the canonical procedure):
       a. Confirm fleet healthy and all devices on the previous version.
       b. Trigger cmd/ota_check on ONE device (the canary).
       c. Wait for the canary's boot announcement at the new version
          AND its OTA_VALIDATED status event. Typically <2 min total.
       d. Soak the canary for at least 5 min (stability sanity-check).
       e. Trigger the next device. Repeat (c) and (d).
       f. After the last device, soak the whole fleet for 1 h before
          considering the deployment "stable" and moving to the next
          release.

    Future operator-tooling improvements (not blocking, but useful):
       - Node-RED button "OTA fleet (canary)" that runs the procedure
         above automatically: pub cmd/ota_check, wait for boot+validated
         events, repeat for next device.
       - Status tile that shows current firmware version per device so
         the operator can see canary vs. fleet at a glance.
       - Auto-revert: if any device times out during canary OTA, halt
         the rollout and notify the operator instead of proceeding.

    Operator-tooling RFC (added 2026-04-25 after fleet-OTA failure mode):
       The current "Update all" / per-device "Update" buttons on the
       Device Status tab fire cmd/ota_check immediately, with no
       awareness of the canary discipline above. This is unsafe for
       any deployment >2 devices and silently invites the same
       AP-contention rollback we just hit. Required dashboard upgrades:

       1. STAGGER STATE MACHINE (Node-RED side, no firmware change):
          - "Update fleet" button kicks off a state machine that:
            a. Lists devices needing OTA (filter by current
               firmware_version != target).
            b. Picks the first as canary; publishes cmd/ota_check.
            c. Waits for that device's boot announcement at the new
               firmware version AND its OTA_VALIDATED status event.
            d. Waits an additional 5 min stability soak.
            e. Moves to the next device, repeats c+d+e.
            f. If any step times out (e.g. canary doesn't return on the
               new version within 8 min), halts the rollout, surfaces a
               red alert, and asks operator confirmation before continuing.
       2. VISUAL PROGRESS INDICATOR:
          - Per-device row showing: current_version, target_version,
            and current state in { pending / waiting-canary /
            uploading / validating / soaking / done / FAILED }.
          - Per-device coloured progress bar:
              grey  = pending
              blue  = OTA_CHECKING / OTA_DOWNLOADING / OTA_PREFLIGHT
              amber = OTA_VALIDATING (post-reboot, awaiting MQTT)
              green = OTA_VALIDATED + soaked
              red   = OTA_FAILED or rollback fired
          - Overall fleet progress: "Updating 1 of 3 — ESP32-Charlie".
          - "Cancel rollout" button that stops the state machine
            without rolling back already-validated devices.
       3. PER-DEVICE OTA OUTCOME LOG:
          - Append to a flow-context array on each OTA event seen so
            the operator can review what happened recently
            (which device, when, what version, result).

       Effort: ~4 hours of Node-RED state-machine + UI work.
       Priority: HIGH if more than one release is planned; the next
                 fleet OTA without this WILL hit the same rollback
                 cascade.

    ──── Second post-mortem addendum 2026-04-25 (v0.4.07 OTA pass) ────
    During the v0.4.07 release, fleet OTA was attempted with the triangle
    on power banks (Charlie/Delta/Echo) and Bravo on USB-PC for serial
    visibility. Two distinct failure modes were observed; v0.4.07
    firmware itself proved healthy on the USB-PC unit.

    Outcomes:
       - Bravo (USB-PC, OTA from v0.4.06 claim → v0.4.07):
            * Download progressed cleanly to ~70 %.
            * PANIC during esp_partition_write → spi1_end →
              cache_enable → spi_flash_enable_interrupts_caches…
              → _xt_coproc_restorecs (EXCCAUSE 0 = IllegalInstruction).
            * Bootloader rolled back to original v0.4.06 partition.
            * Repeatable on Bravo specifically (same module had the
              v0.4.06 hard-reset documented in the prior addendum).
            * NOT v0.4.07 specific — same call stack would hit any
              version. Bravo-specific module flake or USB-power-related.
            * MITIGATION: reflashed Bravo via serial (`pio run -t
              upload`) — clean boot at v0.4.07, sustained 5+ minutes.

       - Charlie (triangle, power-bank, OTA from v0.4.06):
            * Download + flash succeeded — boot announcement WAS
              published at v0.4.07, so app0 took the new image.
            * task_wdt fired during the 5-min Phase-2 validation
              window before otaValidationConfirmHealth() could mark
              the partition valid.
            * Bootloader rolled back to v0.4.06 partition. Boot
              announcement on v0.4.06 reports boot_reason=task_wdt
              (the task watchdog that caused the rollback).
            * Same failure mode triangle devices hit on v0.4.06 OTA:
              cold boot → WiFi association in triangle position
              consumes too much of the validation budget → MQTT
              connect happens but a background task starves and
              triggers task_wdt before Phase-2 validation runs.

       - Bravo (USB-PC) running v0.4.07: 5 min uptime, no task_wdt,
            heartbeats arrive every 60 s as expected. Confirms
            v0.4.07 firmware itself is HEALTHY in steady state.

    ROOT-CAUSE-DELTA: the OTA failure mode is environment-driven, not
    firmware-driven. v0.4.07 source code is identical to what runs
    cleanly on Bravo. The triangle position (~5 m from AP through a
    wall) compounds:
       a. Slower WiFi association → eats into the 5-min Phase-2 budget
       b. Lower-quality MQTT keepalive → background task can starve
       c. Power-bank power: borderline brownout margins under boot
          peak current (radio + flash + ESP-NOW init simultaneously)

    OPTIONS for the triangle fleet (deferred — calibration UX work
    can proceed using Bravo as the v0.4.07 calibration node):

       A) Serial-flash each triangle device one at a time. Disruptive
          (triangle dismantles temporarily) but reliable.
       B) Move each triangle device to PC USB for OTA, return after
          validation. Same disruption as A, no benefit over A.
       C) Extend OTA_VALIDATION_DEADLINE_MS from 5 min → 10 min for
          triangle devices specifically. Risks masking real faults
          (the original 5 min was tuned for the failure-detection
          window) — use a per-device override only as last resort.
       D) Add a Node-RED / firmware coordination: pause beacon
          publishing during the first 60 s of post-OTA boot so
          the new image has more headroom for WiFi+MQTT setup.
          Smallest disruption; defer to v0.4.09+.

       ACCEPT-CURRENT for v0.4.07: Bravo on v0.4.07, triangle on
       v0.4.06. Calibration commands work on v0.4.07 (the device
       receiving the commands). Triangle ranging works fine on
       v0.4.06 (the EMA-reseed fix is already in v0.4.06).

    ──── v0.4.08 FIX (shipped 2026-04-25) ────────────────────────────
    Two surgical changes in v0.4.08 to address the failure modes:

    Fix 1 — esp_now_unregister_recv_cb() + esp_now_deinit() in
            otaCheckNow() AFTER the BLE deinit + heap pre-flight
            but BEFORE the esp_https_ota download begins
            (include/ota.h, just before the loopTask wdt_add).
            REASON: ESP-NOW receive callbacks are not IRAM_ATTR.
            esp_partition_write disables instruction caches while
            programming flash; if a frame arrives during that window,
            the callback dispatch path tries to execute flash-resident
            code and the CPU faults with EXCCAUSE 0 (IllegalInstruction
            in _xt_coproc_restorecs).
            EVIDENCE: Bravo's v0.4.07 OTA stack trace at 70 % progress
            shows exactly this fault, inside esp_partition_write →
            spi_flash_enable_interrupts_caches_and_other_cpu →
            _xt_coproc_restorecs.
            EXPECTED OUTCOME: silent-hang (Alpha) and panic-mid-write
            (Bravo) failure modes both eliminated. The triangle fleet's
            beacons-every-3-s no longer race the flash write.

    Fix 2 — espnowRangingLoop() early-returns if otaValidationIsPending()
            AND millis() < ESPNOW_POST_OTA_QUIET_MS (30 s default).
            REASON: Charlie's v0.4.07 OTA flashed cleanly and booted
            v0.4.07, but task_wdt fired during the validation window
            before otaValidationConfirmHealth() ran. Triangle position
            costs ~5–8 s of WiFi association; while that's blocking,
            ESP-NOW beacon TX + receive callback work eats loopTask
            budget and starves the WiFi+MQTT setup path.
            EXPECTED OUTCOME: triangle devices get a clean 30 s
            head-start to associate, connect MQTT, publish heartbeat,
            and call otaValidationConfirmHealth() before any beacon
            traffic resumes. After that, ESP-NOW ranging continues
            normally.

    NOT changed (deliberately):
       - OTA_VALIDATION_DEADLINE_MS stays at 5 min. Extending it
         would mask real fault detection. The fix removes the
         starvation, not the deadline.
       - OTA_PROGRESS_TIMEOUT_MS stays as-is — the progress watchdog
         is independent of the cache-disabled-fault class.

    ──── v0.4.08 IN-FIELD VALIDATION ────────────────────────────
    Bravo (USB-PC) OTA from v0.4.07 → v0.4.08 (2026-04-25 16:27):
       - Download progressed cleanly through 10–100 % (no panic
         at the 60–70 % cache-disabled boundary that killed v0.4.07).
       - "Flash succeeded — restarting" → "Armed rollback for
         incoming version '0.4.08'" → SW_CPU_RESET (clean).
       - Re-boot on app1 partition state 2 (VALID since
         ROLLBACK_ENABLE is off; NVS-flag path validates).
       - WiFi associated, MQTT connected, boot announcement at
         firmware_version "0.4.08", uptime=2 s.
       - Sustained heartbeat at uptime=240 s — Phase-2 deadline
         was 300 s; well within budget on USB-PC environment.
       - Conclusion: Fix 1 (ESP-NOW deinit) eliminates the panic-
         mid-flash-write failure mode. Bravo previously panicked
         at 70 % every attempt; clean now.

    Charlie (triangle, on v0.4.06, OTA → v0.4.08 attempted twice):
       - Attempt 1 (16:32, Delta+Echo still ranging): Charlie
         offline LWT → boot announcement at v0.4.06 with
         boot_reason=int_wdt. No v0.4.08 boot announcement.
       - Attempt 2 (16:34, Delta+Echo silenced via cmd/espnow/
         ranging "0"): same — Charlie offline → boot at v0.4.06
         with boot_reason=task_wdt. No v0.4.08 boot announcement.
       - Conclusion: Fix 1 is in v0.4.08, but Charlie ran v0.4.06's
         OTA code during the download, which has neither Fix 1
         nor the more robust task-watchdog handling. The watchdog
         fires DURING the download (chunks too slow over the
         marginal AP link) before flash even completes. Charlie
         reboots back on the still-valid v0.4.06 partition.

    ──── Chicken-and-egg insight ────────────────────────────────
    The OTA reliability fixes shipped in v0.4.08 only protect
    devices that are ALREADY running v0.4.08. The version doing
    the download executes its own OTA code path; if that code
    path has a bug that triggers a reset (panic / int_wdt /
    task_wdt / silent hang), the OTA can never complete and the
    device is stuck on the old version.

    BREAKING THE EGG (only path that works for v0.4.06 → v0.4.08):
       Serial-flash one triangle device at a time via USB-PC.
       Move device to COM5 → `pio run -t upload` → return to
       triangle. Once on v0.4.08, future OTAs use Fix 1 + Fix 2
       and complete reliably.

    NEXT-RELEASE WORK (v0.4.09+, planned):
       a. Increase task watchdog timeout to 30 s during the OTA
          download window only (revert after the download completes
          OR after reboot). Prevents the slow-link task_wdt class
          on triangle/marginal-signal devices.
       b. Periodic timer-based esp_task_wdt_reset() during OTA so
          the WDT doesn't depend on per-chunk progress callbacks.
       c. (Speculative) Pre-flight ESP-NOW deinit could also be
          shipped as a v0.4.09 hotfix — devices on v0.4.06 still
          can't benefit, but it's a belt-and-braces measure for
          any future regression.

       NOTE: none of the above help v0.4.06-flashed devices. Those
       MUST be serial-flashed once. After that, the v0.4.09+
       OTA improvements protect against regressions for the rest
       of the device's lifetime.

    ──── Third failure mode — silent hang (Alpha, observed 2026-04-25):
       Alpha was OTA-triggered earlier in the same v0.4.07 rollout
       and went silent: built-in blue status LED stuck SOLID ON,
       MQTT LWT fired (offline message published).
       The LED-pattern table in led.h has only two states that
       hold the LED solid HIGH:
            BOOT         — early-boot, before WiFi up
            OTA_UPDATE   — set on cmd/ota_check entry, cleared on
                           OTA exit (success OR error)
       Alpha is therefore stuck inside the OTA code path with no
       clean panic to trigger watchdog reset. Likely a flash-write
       cache-restore stall (similar to Bravo's panic but the panic
       handler itself didn't fire — possibly because IRAM wasn't
       reachable when the fault hit).

       MITIGATION: manual power-cycle is the only recovery from
       this state. Park Alpha for now (not blocking calibration
       work). Document as a third v0.4.07-OTA failure mode beyond
       Bravo (panic mid-write) and Charlie (task_wdt mid-validate).

    ──── Post-mortem addendum 2026-04-25 (Bravo USB-PC OTA retry) ────
    After the fleet rollback, an attempt was made to OTA Bravo while
    it was connected to PC USB on COM5 with the serial monitor running
    (cleanest possible environment — strong WiFi, single-device
    association, full power, full visibility). Result: ALSO failed,
    but in a different mode than the triangle devices.

    Serial trace (Bravo, 2026-04-25 14:55):
        rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
        [AppConfig] Node name: ESP32-Bravo
        [I][OTAValid] Boot partition 'app1' state: 2     ← v0.4.05
        [I][WiFi] Connected - IP: 192.168.10.112
        [I][MQTT] Connected to broker
        [I][MQTT] Boot announcement published (v0.4.05)
        [I][MQTT] OTA check triggered via MQTT
        [W][MQTT] Disconnected (TCP_DISCONNECTED) - retrying in 1000 ms
        rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT) ← HARD RESET
        [AppConfig] Node name: ESP32-Bravo
        [I][OTAValid] Boot partition 'app1' state: 2     ← STILL v0.4.05

    Observations:
       - No "OTAValid: armed rollback for incoming version 0.4.06"
         log line — the new image was never even staged for boot.
       - The reboot is POWERON_RESET (0x1), not SW_CPU_RESET. That is
         a HARD reset (brownout / external watchdog), not a normal
         esp_restart() path. The OTA download itself crashed the
         module before any image was committed.
       - Came back on the same partition (app1) with the same
         version (v0.4.05) — no rollback fired because no new image
         was ever booted.

    This is a SECOND, DIFFERENT failure mode from the triangle case:
       - Triangle case (3 devices simultaneous):  flash succeeded
         → boot v0.4.06 → AP contention → MQTT timeout → Phase-2
         rollback fires correctly.
       - Bravo USB-PC case (1 device, perfect environment):
         flash never completes → hard-reset during download →
         original partition retained.

    Hypothesised causes (in order of likelihood):
       1. Brownout during simultaneous WiFi-RX + flash-write +
          serial-TX. The PC USB hub may not be sourcing enough
          current to satisfy peak demand. Bravo specifically has
          addressable LEDs wired to D27 (per session notes), which
          could add to peak current.
       2. HTTPS/TLS handshake failure on the OTA download — but
          this would normally produce a clean ESP_FAIL log line,
          not a hard reset.
       3. Bravo-specific module hardware flake (this module has
          historically had issues — was the device that exhibited
          the v0.3.33→v0.3.35 OTA panic that prompted item #35
          in the first place).

    Charlie, Delta, Echo, Alpha are all working on v0.4.06 (Charlie
    via OTA, Delta and Echo via fresh serial flash, Alpha is parked
    on v0.4.05 with similar symptoms). Bravo and Alpha are the only
    units reproducing OTA failure modes; both happen to be from the
    same procurement batch. SUGGESTED FOLLOW-UP:

       a. Try OTA on Bravo from VIN power (no USB cable attached) —
          rules in/out the brownout-via-USB-power hypothesis.
       b. If still fails, try OTA after disabling the addressable
          LED strip on Bravo — rules in/out peak-current-from-LEDs.
       c. If still fails, capture full ESP-IDF debug log
          (CORE_DEBUG_LEVEL=5) during OTA — should expose whether
          it's HTTPS, partition-write, or signature-verify failing.
       d. If still fails on Bravo specifically, classify the module
          as "OTA-untrustworthy, serial-flash only" and replace it.
          The fleet has 5 working units (Charlie/Delta/Echo + Alpha
          if/when re-flashed via serial); Bravo's RFID role can be
          covered by any other unit.

    PRIORITY: Low — the working-fleet count is sufficient for the
              calibration work. Park as a Bravo-specific
              investigation, not a release-blocker.

36. Operational practice: heartbeat / boot-reason monitoring
    Confirm 60s heartbeat is sufficient for Node-RED's 90s offline
    detector. If devices ever report boot_reason=task_wdt or panic in
    the wild, treat as a real bug to investigate, not a transient.
    Build a Node-RED dashboard tile that surfaces non-poweron
    boot_reason values across the fleet.
    PRIORITY: Medium — easy Node-RED change, high diagnostic value.

37. ESP-NOW ranging — A↔B asymmetry causes and mitigations
    OBSERVATION (2026-04-25):  In the dashboard's per-direction chart,
    A→B and B→A distances on the same physical pair frequently disagree
    by 30–60% on uncalibrated devices. Operators ask why.
    ROOT CAUSES, ranked by impact in this fleet:
       1. Per-device calibration constants left at compile-time defaults
          (txPower_dBm=-59, path_loss_n=2.5). Each device's real TX
          power and antenna gain vary by 2–8 dB → log-distance formula
          translates that into ~2× distance error.
          MITIGATION: run the 3-step wizard on every node-pair.
       2. Antenna orientation — ESP32-WROOM PCB trace antennas have a
          figure-of-8 radiation pattern, not omnidirectional. A→B and
          B→A pick up different lobes.
          MITIGATION: cannot fully fix in firmware; document for
          installers. External antennas would close the gap.
       3. Asymmetric path obstructions (laptop / mug / cable near one
          side). Real RF reciprocity breaks down in cluttered spaces.
       4. AGC + noise floor differences between two radios. ±2 dB even
          for identical chips.
       5. EMA history skew during fast distance changes — converges
          within ~10–20 s.
       6. Local interference (USB hub, WiFi AP, microwave) near one node
          but not the other.
    POSSIBLE FUTURE WORK:
       a. Pair-aware calibration mode: run the wizard once per pair
          (instead of per-device), store per-peer calibration constants
          rather than the current single-tx-power-per-node. Closes the
          calibration-mismatch gap entirely. ~half-day of firmware work
          (extend AppConfig to a small per-MAC calibration map).
       b. Auto-calibration: when 3 nodes are at known fixed positions
          (anchors), compute path-loss exponents per pair from the
          observed RSSI-vs-known-distance data. No operator wizard
          required. ~1 day of firmware work.
       c. Dashboard "asymmetry health" badge: per pair, compute |A→B − B→A|
          and colour-code green / yellow / red against a threshold.
          Surfaces calibration drift without operator interpretation.
          Pure Node-RED change.
       d. Document the figure-of-8 antenna pattern in the operator
          install guide so site planners orient nodes thoughtfully.
    PRIORITY: Medium for (a) and (c) — both improve perceived accuracy
              materially without big effort. Low for (b) — needs anchors,
              which the current sites don't have.

39. ESP-NOW calibration — multi-point + arbitrary-distance variants
    OBSERVATION (2026-04-25):  The current calibration wizard is hard-coded
    to 1 m + 1 operator-chosen distance. Two real-world gaps emerged:
       (a) Ceiling-mounted nodes can't easily be brought down to 1 m.
       (b) The 2-point fit has no outlier resistance — if either of the
           two median samples is contaminated (passing person, wall
           reflection burst), the resulting tx_power_dbm / path_loss_n
           are silently bad, with no way for the operator to know.
    PROPOSAL — two new modes coexisting with the current one:

    A. ARBITRARY two-point mode
       New cmd: cmd/espnow/calibrate {"cmd":"measure_at","peer_mac":"...","distance_m":2.5,"samples":30}
       Operator runs measure_at twice at two known distances (any two,
       not constrained to 1 m). Commit fits both unknowns simultaneously:
           tx_power_dbm = rssi_a + 10*n*log10(d_a)
           n = (rssi_a - rssi_b) / (10 * (log10(d_b) - log10(d_a)))
       Use case: ceiling installs where 1 m is impractical.
       Effort:   ~1 h firmware (extend espnowCalibrateCmd state machine).

    B. MULTI-POINT mode
       Operator collects N >= 3 measurements (e.g. defaults 1 m / 3 m /
       5 m / 10 m, editable). Firmware accumulates the (d, rssi_median)
       pairs in RAM. On commit, performs least-squares linear fit in
       (log10(d), rssi) space and computes:
         - tx_power_dbm and path_loss_n (best-fit values)
         - R^2 of the fit (model goodness-of-fit, 1.0 = perfect)
         - residuals per point (operator-visible diagnostic)
       Publishes the full result set to .../response so Node-RED can
       render: a fit-quality badge (green if R^2 >= 0.9, yellow 0.7-0.9,
       red below) and an optional residuals plot.
       Use case: warehouse / outdoor / known-multipath sites where the
       2-point estimate is unreliable.
       Effort:   ~half-day firmware (RAM buffer + linreg) + ~half-day
                 Node-RED (dynamic form for adding measurement points,
                 residuals visualisation).

    PRIORITY: Low-medium. The current 2-point flow is adequate for the
              dominant office-install case. These modes pay off in
              awkward installs (ceiling, warehouse) and for operators
              who want quantitative confidence in the calibration.
              Worth bundling A + B together since they share most of
              the state-machine extension.

38. ESP-NOW ranging — runtime-tunable beacon / publish intervals
    WHERE:    include/config.h (ESPNOW_BEACON_INTERVAL_MS = 3000UL,
              ESPNOW_PUBLISH_INTERVAL_MS, ENR_PEER_STALE_MS) — currently
              compile-time only.
    PROBLEM:  3 s beacon / 2 s publish is comfortable for slow-walking-pace
              tracking but too sluggish for active testing or fast-motion
              detection. Operators have no way to switch modes per session
              short of editing config.h, rebuilding, and OTA-ing the fleet.
    FIX:      Add three retained MQTT command topics (or a single bundled
              cmd/espnow/timing taking a JSON object):
                cmd/espnow/timing → {"beacon_ms":500,"publish_ms":500,"stale_ms":3000}
              Apply at runtime; persist in NVS alongside the existing
              calibration constants. Validate ranges (e.g. beacon_ms ∈
              [200, 60000]) and reject out-of-band values silently.
    PRACTICAL VALUES (from 2026-04-25 walk-through discussion):
       - "Walk-pace" (current default): 3000 / 2000 / 15000
       - "Active testing":              500  / 500  / 3000   (alpha→0.5)
       - "Sub-second motion":           200  / 500  / 1500   (alpha→0.6)
    CONSTRAINTS:
       - Floor: ~200 ms beacon — below this the ±20% jitter window
         collapses and channel contention with WiFi MQTT path matters.
       - Battery devices: keep beacon ≥3 s; sub-second beacon prevents
         any meaningful sleep.
       - At fast intervals, EMA alpha needs to rise (less smoothing,
         since you have more samples per second). Calibration also
         finishes proportionally faster — net positive.
    BENEFIT:  Operator can pick a regime that matches the test scenario
              without a firmware build. Pairs well with a Node-RED preset
              dropdown ("Slow / Standard / Fast / Sub-second").
    PRIORITY: Medium — clear UX win for the ESP-NOW v2 dashboard tab.
              Defer until interval tuning is actually wanted in the field.

41. Hardware finding — breakout board + RFID-RC522 distort the WROOM antenna
    OBSERVATION (2026-04-25): During calibration walk-through, ran an A/B/C/D
    test of the same two devices in different mounting configurations. The
    breakout board in use is the "ESP32-S 30P expansion board" with an
    MFRC522 RFID module wired to its 8 SPI/IRQ/RST/3v3/GND pins. Distance
    Alpha-to-Charlie was held constant for the comparisons.

    Configuration                                                              Alpha->Charlie   Charlie->Alpha   Asymmetry
    A. Alpha on breakout (RFID powered),    Charlie bare                              -76              -53        23 dB
    B. Both bare                                                                      -77              -72         5 dB    <- baseline
    C. Charlie on breakout (RFID powered),  Alpha bare                                -76              -70         6 dB
    D. Charlie on breakout (RFID UN-powered),  Alpha bare                             -80              -67        13 dB
    E. Alpha on breakout (RFID UN-powered), Charlie bare                              -72              -61        11 dB
       (Alpha powered via its own micro-USB)
    F. Alpha on breakout (RFID UN-powered), powered VIA breakout VIN, Charlie bare    ~-50*            -67       ~17 dB
       *raw RSSI jumped from -72 to ~-47 dBm (+25 dB on Alpha's TX)
       but Charlie's outlier gate rejected the new samples (deviation > 15 dB
       from old EMA), so rssi_ema stayed stuck at -83 and rejects climbed
       continuously. EMA only updates after a power-cycle / 15 s peer
       eviction / outlier_db widening.
    G. Charlie on breakout (RFID UN-powered), powered VIA breakout VIN, Alpha bare    -79              -56        23 dB
       (Charlie's TX boosted ~+20 dB vs C/D where it was on the breakout but
       USB-powered. Charlie's RX path unchanged at ~-79. Confirms the
       power-path effect is reproducible: whichever device is powered via
       its breakout's VIN gains the TX boost. Asymmetry direction therefore
       depends on which device is on the breakout — it always favours the
       powered-via-VIN device's TX path.)

    H. Charlie on breakout, VIN-powered, RFID powered + correctly wired, Alpha bare  -85              -57        28 dB
       Compared to G (same setup, RFID off): Charlie's RX dropped 6 dB
       (-79 -> -85) while TX held steady at -57 (already at the VIN-powered
       max). Confirms the RFID coil reduces RX sensitivity of the host
       WROOM by ~6 dB even when the breakout regulator is healthy. TX
       was already saturated by the VIN-power effect, so the coil's
       additional perturbation could not push it further.

    I. Alpha on breakout, VIN-powered, RFID powered, Charlie bare                     -63              -42        21 dB
       Mirror of H with Alpha on the breakout instead of Charlie. Both
       directions stronger than the USB-powered RFID variant (config A:
       -76 / -53). Alpha's TX (Charlie's view): -76 -> -63 = +13 dB from
       VIN power. Alpha's RX (its own view of Charlie): -53 -> -42 = +11 dB.
       Charlie's bare TX at -42 dBm is approaching receiver-saturation
       territory; EMA tracking can become non-linear above approximately
       -40 dBm because the WROOM's AGC compresses. Per-device asymmetry
       direction: VIN-powered+RFID side dominates BOTH TX and RX in this
       case, but the RX dominance came from the breakout coupling rather
       than directly from VIN power (we observed earlier in config G that
       VIN power alone primarily boosts TX). Adding RFID amplifies the
       RX-coupling component on the host device.

    Cautionary footnote (originally captured as the broken H):
       During this test we accidentally hit a separate failure mode: an
       RC522 wired with 3v3/GND SWAPPED appears identical to a real
       brownout from the dashboard side (peer goes silent, retained LWT
       persists, no boot event arrives). When debugging "device suddenly
       offline after I touched the wiring", verify polarity FIRST and
       inspect for damaged components on both the RC522 and the breakout
       regulator before assuming a software / radio issue.

    STATISTICAL SUMMARY (10-config sweep, see docs/SESSIONS/RF_CONFIG_TEST_2026_04_25.md
    for the raw data and full analysis):

       Reproducibility (bare-bare measured twice):
          Asymmetry noise floor: ~2 dB.  Absolute RSSI noise: ~5 dB.
          So differences smaller than ~5 dB across configurations are
          not statistically significant.

       Mean asymmetry change vs bare-bare baseline (4 dB):
          Adding the breakout PCB alone           +8 dB asymmetry
          Adding the RFID coil to the breakout    -7 to +12 dB (per-device)
                                                  range 19 dB,
                                                  unpredictable direction
          Switching USB power -> VIN power        +9 dB asymmetry
                                                  but +7 dB stronger TX

       Mean signal level change vs bare-bare:
          USB-cable removal (VIN power)           +7 dB stronger
          Otherwise +/- 2 dB across configs.

       Worst-case stack (breakout + RFID + VIN power, configs H/I):
          +17 to +24 dB worse asymmetry than baseline.
          Translates to ~9x distance error in one direction relative to
          the other at typical office path-loss exponent (n=2.5).

       Per-device variation:
          The same physical configuration gives noticeably different
          asymmetry depending on which WROOM module is on the breakout
          (up to 17 dB spread for "breakout + RFID + USB-power").
          Calibration constants are NOT transferable between mountings.

    REVISED PRIORITY ORDER for the user's current hardware (descending impact):
       1. Mount RC522 on a separate small board, >=5 cm from WROOM antenna.
          (Restores most of the lost reciprocity. Cheap mechanical fix.)
       2. Standardise power path: pick USB or VIN for the whole fleet, not
          mixed. Reproduce in both calibration and operation.
       3. Standardise breakout-vs-bare for the whole fleet. Don't mix.
       4. Route USB power cables AWAY from the WROOM antenna (>=10 cm
          clearance, perpendicular if possible). USB cables in the antenna
          near-field cost 5-10 dB asymmetry and up to 25 dB TX boost on
          one direction (parasitic radiator effect).
       5. Calibrate each device in its exact final mounting AND position.
          Both hardware AND room multipath contribute to ranging behaviour.
          A device calibrated at desk position A will be wrong at position B
          even with identical hardware mounting.
       6. (Firmware) Add EMA-reseed-on-step-change to peer_tracker.h —
          avoids stuck-EMA failure mode after any environmental change.
       7. (Firmware) Add per-peer calibration constants — partial fix
          for residual per-pair asymmetry that hardware redesign does not
          fully eliminate.

    POSITION-VS-HARDWARE DECOMPOSITION (added 2026-04-25 from Part B
    swap experiments — see RF_CONFIG_TEST_2026_04_25.md Section B):
       Apparent per-device "Charlie's TX is +18 dB stronger" decomposes
       into:
          ~8-12 dB genuine per-device hardware variation
                 (still well over ESP32 datasheet ±2 dB spec)
          ~5-10 dB per-position multipath effect
                 (Charlie's original spot was an environmentally favourable
                 location for TX in Bravo's direction by ~9 dB)
       Position effect was confirmed by physically swapping Alpha and
       Charlie — when Alpha occupied Charlie's old spot, Bravo's view of
       Alpha's TX gained +9 dB versus when Alpha was at its original spot.
       Implication: in real deployments, ranging accuracy depends on the
       SPECIFIC mounting LOCATION, not just the device + cable + antenna
       configuration. Calibration must be done at the deployment location;
       moving a calibrated device 1 m laterally invalidates the calibration
       by ~5-10 dB.

    Side observation — accidental anchor persistence:
       During the same dashboard walk-through, an Anchor-form click
       slipped through and committed Alpha as anchor at (0, 0, 0):
         retained: cmd/espnow/anchor = {"role":"anchor","x_m":0,"y_m":0,"z_m":0}
       This persists in NVS and rides on every status heartbeat.
       Implications:
         (a) Future Map-tab multilateration will treat Alpha as a fixed
             anchor at the origin — almost certainly wrong physically,
             would corrupt position estimates of mobile peers.
         (b) The dashboard makes it easy to commit an anchor with default
             values (0, 0, 0) by clicking Save without filling in real
             coordinates. Form should default `is_anchor` to false and
             require an explicit confirmation when committing (0, 0, 0)
             coordinates with `is_anchor: true`.
       Cleanup: publish retained `{"role":"mobile"}` to Alpha's
       cmd/espnow/anchor topic (or use the Anchor form with is_anchor
       UNticked) before any production use.

    Take-aways (cross-comparing configs):
       - Bare-bare (B) is the most symmetric configuration. The 5 dB delta
         is plausible random antenna-pattern variation between two boards.
       - The breakout alone (without RFID) still distorts: configs D and E
         both show 11-13 dB asymmetry. The expansion board's metal traces
         and ground plane are themselves enough to reshape the WROOM's
         antenna pattern. RFID is not the only culprit.
       - The RFID coil ON TOP of the breakout adds another layer:
            Compare A (Alpha + breakout + RFID powered) vs E (Alpha +
            breakout + RFID disconnected): with RFID powered, Alpha's RX
            of Charlie went from -61 to -53 (an extra 8 dB RX boost).
            The TX side moved from -72 to -76 (4 dB worse).
            So the powered RFID coil is a strong parasitic radiator that
            HELPS RX but HURTS TX of the device it sits on.
       - Effect is per-device-per-direction. Configs A (Alpha + breakout +
         RFID) and C (Charlie + breakout + RFID) at the same physical
         distance give very different asymmetries (23 vs 6 dB), even though
         it's the "same" configuration just on a different module. So the
         specific physical interaction between THIS WROOM and THIS breakout
         matters — likely down to PCB tolerances or how cleanly the WROOM
         seats in the headers.
       - POWER-PATH effect (configs F + G): changing where the WROOM's
         power feeds in (its own micro-USB vs. through the breakout's VIN
         pin) added ~20-25 dB to that device's TX strength. RX side stays
         essentially unchanged. This is the SINGLE LARGEST EFFECT measured
         in the seven configurations — bigger than the breakout itself
         (~10 dB), bigger than the RFID coil (~8 dB). Reproduced on both
         Alpha (config F) and Charlie (config G), confirming it's not a
         per-device quirk.
         Likely mechanism: the micro-USB cable (when used for power) acts
         as a parasitic antenna near the WROOM trace antenna. Its length,
         position, shield grounding, and proximity to the antenna pull the
         WROOM's radiation pattern off-axis and absorb / re-radiate energy.
         Removing the USB cable (powering via breakout VIN instead) removes
         that parasitic and recovers the WROOM's intended TX behaviour.
         POWER WIRING IS PART OF THE RF DESIGN. Mounting AND cable layout
         must be reproduced exactly during calibration and during operation.
         For accurate ranging, the recommended deployment power path is
         **through the breakout's VIN pin (or a similar non-USB path)**,
         with the calibration done in that exact configuration.

    Secondary firmware finding from config F:
       Charlie's peer_tracker outlier gate (default 15 dB) silently
       discarded every strong frame after Alpha's TX jumped, leaving its
       rssi_ema and dist_m stuck at the old weak values while the rejects
       counter climbed continuously. There is no way for the EMA to
       recover from a real-world step-change of >15 dB except via peer
       eviction (15 s silence), full reset, or temporarily widening the
       outlier threshold. CONSIDER: an "EMA-jump-detection + reset" branch
       in peer_tracker.h that, if N consecutive frames all deviate the
       same direction by more than outlier_db, accepts them and re-seeds
       the EMA. Avoids a stuck EMA when a device is moved, repowered, or
       the install changes. Half-day firmware change.
    INTERPRETATION:
       The breakout + RFID combination acts as a parasitic structure that
       reshapes the WROOM's antenna radiation pattern. The effect is
       per-device, per-direction, and per-RFID-state. RSSI reciprocity
       (the fundamental assumption of all RSSI-distance models) is broken
       in hard-to-predict ways even within a uniform-hardware fleet.
    IMPLICATIONS:
       1. Calibration on a breakout-mounted device captures the boosted TX
          but the RX side is still the unboosted trace antenna. Distance
          estimates Alpha-computes-from-Charlie's-frames will always be
          dominated by Alpha's weaker RX, not the boosted-TX numbers
          that calibration step 1 captured. Mismatch between step 1 and
          runtime behaviour.
       2. The Pair Chart's bidirectional-distance asymmetry is largely
          this hardware effect, not a real geometric asymmetry.
       3. For accurate ranging, ALL devices should be in the SAME hardware
          configuration AND that configuration should ideally not introduce
          parasitic radiation. Bare board with deliberate ground plane is
          probably more honest than the current breakout.
    RECOMMENDATIONS:
       a. For accurate ranging, prefer BARE BOARDS or a clean carrier PCB
          designed for RF (proper ground plane, antenna keep-out zone, no
          metal traces or coils within 3 cm of the WROOM antenna). The
          current "ESP32-S 30P + RC522" combo is the worst case — the RFID
          coil sits very close to the WROOM trace antenna.
       b. If RFID functionality is required, mount the RC522 on a SEPARATE
          board connected by a short cable, with the coil placed >5 cm
          from the WROOM antenna. Or move the RFID coil to the opposite
          end of the device chassis from the antenna.
       c. Consider an ESP32-WROOM module variant with U.FL connector +
          external antenna — that sidesteps the trace-antenna detune
          problem entirely.
       d. Document the breakout/RFID effect in the install guide so
          operators don't mix configurations within a fleet, and so that
          ranging accuracy expectations are realistic in this hardware.
       e. Add a fleet-config consistency check to the dashboard: surface a
          warning if pair RSSI asymmetry exceeds ~15 dB consistently —
          that's a near-certain hardware-config or mounting-config mismatch
          rather than an environmental effect.
       f. Calibrate every device IN THE EXACT MOUNTING IT WILL DEPLOY IN.
          Same breakout. Same orientation. Same RFID power state. Same
          nearby cables. Otherwise calibration constants do not transfer.
    PRIORITY: Medium for documentation (entry #40 install guide should
              cite this). Medium for separating the RFID coil from the
              WROOM antenna in any v2 hardware design — that's the cheap
              hardware fix that recovers reciprocity.

40. Operator install guide — ESP32-WROOM antenna orientation
    OBSERVATION (2026-04-25):  During calibration walk-through a tx_power_dbm
    of -91 was seen at "1 m" — ~36 dB worse than the typical -50 to -55 dBm
    a normally-oriented WROOM gives at 1 m. Almost certainly antenna
    null / parasitic absorption from a nearby cable. There is currently no
    documentation telling field installers how to orient nodes.
    ROOT CAUSE: ESP32-WROOM PCB trace antennas are NOT omnidirectional.
    The radiation pattern is a figure-of-8 in the plane of the PCB:
       - Two strong lobes off the antenna END of the board.
       - Two deep nulls off the long EDGES of the board.
       - Sensitivity to nearby metal (ground planes, USB cables, batteries)
         within ~3 cm of the antenna trace — those detune the radiator
         and reshape the pattern unpredictably.
    DELIVERABLE: a short install-guide section (Markdown in docs/, or a
    page inside the AP-portal HTML) with:
       1. A figure-of-8 pattern diagram showing the antenna end vs edges.
       2. Orientation rules:
          - Antennas pointed AT each other (board-end to board-end). Best.
          - Avoid edge-on alignment between two nodes — that's the null.
          - Keep ≥3 cm clear around the antenna (no cables / metal / hand).
          - Do NOT mount directly on a metal surface (use plastic standoffs).
          - Calibrate in the SAME orientation the device will be deployed in.
            A desk calibration does not transfer to a ceiling install.
       3. A field-test recipe: rotate one device 90° while watching the
          Live signal in the v2 wizard, find the orientation that gives the
          MOST positive RSSI (least negative dBm), calibrate from there.
       4. RF environment checklist (added 2026-04-25 after observing
          calibration values consistent with receiver desense):
          - WiFi router proximity: keep at least 2 m between the test
            devices and the AP during calibration. A router transmitting
            1-2 m away can desense the ESP32 LNA enough to lose 5-15 dB of
            apparent RSSI on faint ESP-NOW frames. Symptom: stuck low
            RSSI values like -88..-91 at supposed 1 m, with very tight
            variance (only the strongest beacons make it through; weak
            ones drop out entirely so the distribution is skewed).
          - WiFi channel: ESP-NOW is channel-pinned to the AP's channel
            (currently channel 6 in this fleet). If neighbouring WiFi
            networks are crowded on the same channel, ESP-NOW competes
            for airtime. Mitigation: configure the AP on channel 1 or 11
            so channel 6 is quiet for ESP-NOW. (Or vice versa — pick
            whichever channel is least crowded in the installation.)
            Use `netsh wlan show networks mode=bssid` (Windows) to survey.
          - WiFi MQTT activity: heavy MQTT upload bursts monopolise the
            shared radio briefly and cause ESP-NOW frame loss windows.
            Calibrate during quiet periods if possible.
          - Bluetooth devices in the room: BT headsets, mice, trackpads
            transmit in the same 2.4 GHz band and add to the noise floor.
            Switch them off during calibration if practical.
          - Microwave ovens within ~5 m: leaky 2.4 GHz emitter, will
            ruin RSSI measurements while running.
          - Time of day: in offices, neighbouring WiFi traffic is lighter
            06:00-08:00 and 20:00-22:00. Calibrate at off-peak if the
            channel survey is bad.
          - Test recipe: do an A/B run of the same calibration pair, once
            close to the router and once 3 m from it. If the RSSI shifts
            by >5 dB between the two, the RF environment is dominating
            the measurement and any calibration there is unreliable.
    BENEFIT:  Closes the single biggest source of bad first-time calibration
              data. Especially valuable when handing the system to a
              non-RF-aware installer.
    PRIORITY: High for any deployment outside the dev bench. Cheap to
              produce — half a day of writing + one diagram.

    KEY TAKEAWAY: Mounting Alpha on the RFID-RC522-equipped breakout
    added ~17-23 dB of asymmetry to Alpha→Charlie vs Charlie→Alpha
    ranging, even when the RFID was un-powered. The breakout +
    RFID reader (as a passive RF resonator) distort the WROOM
    antenna pattern; per-pair calibration is the only practical fix.
    STATUS: RESOLVED as a documented finding 2026-04-25. Informs #37
    (per-peer asymmetry calibration in v0.4.07/v0.4.09 — shipped) and
    #40 (operator install guide — open). The firmware-side solution
    is per-pair calibration constants which is already shipped via
    #39 multi-point. Index moved to RESOLVED 2026-04-28.

42. ESP-NOW ranging — temporary "active" / "calibrating" / "setup" mode
    OBSERVATION (2026-04-25): Calibration takes 30 samples per step at the
    default 3 s beacon interval = ~90 s per step, ~3 minutes total per
    device-pair. Active tracking of someone walking has noticeable lag
    when the chart updates every 2-3 s. During first-install setup, the
    operator wants instant feedback when they move a device or rotate it.
    But the default 3 s / 2 s rates are correct for steady-state operation
    (low power, low channel contention).
    PROPOSAL — short-lived rate boosts triggered by specific events,
    auto-reverting after a timer or on event-end:
       Mode             Beacon  Publish  Stale    Auto-revert after
       ----             ------  -------  -----    -----------------
       Steady (default) 3000    2000     15000    -
       Active tracking  500     500      3000     30 s of no chart pan/
                                                  motion-detection trigger
       Calibrating      500     500      2500     'committed' / 'reset' /
                                                  60 s of no measurement
       Setup            300     500      1500     5 min, or operator-end
       Battery save     10000   10000    60000    permanent until toggled

    TRIGGERS:
       - Calibrating mode: dashboard sets it on 'started' event for that
         device. Reverts on 'committed' / 'reset' / 'error' or 60 s of no
         measurement. Cuts calibration time from 90 s/step to 15 s/step.
       - Active tracking: a chart-pan event in the dashboard (operator is
         actively watching) sets it for the entire fleet. Reverts after
         30 s of operator inactivity.
       - Setup: operator clicks a "Setup" button on the dashboard before
         physically moving a device. Reverts after 5 min or explicit end.
       - Battery save: per-device toggle for deployments running off cells.

    IMPLEMENTATION:
       - Builds on entry #38 (runtime-tunable intervals) — that's the
         underlying mechanism. This entry adds:
           a) per-device-and-per-pair scoping (so calibrating one pair
              doesn't speed up the whole fleet)
           b) auto-revert timers on the firmware side
           c) dashboard buttons that publish 'enter mode X' / 'exit mode X'
              retained MQTT commands the firmware listens for
       - cmd/espnow/mode {"mode":"calibrating","timeout_s":120}
       - On timeout or 'exit' command: revert to steady defaults.
    BENEFIT:
       - Calibration UX: 90 s -> 15 s per step (6x faster), encourages
         operators to actually run the wizard instead of skipping it.
       - Active-tracking UX: smoother chart, sub-second motion latency.
       - Battery deployments: keep low-power default, enter active mode
         only when needed.
    CAVEATS:
       - Channel contention: at 500 ms beacon, three devices = ~6 broadcasts/
         second total, still cheap. A 10-device fleet at 200 ms = 50/s,
         which starts to interfere with shared WiFi airtime.
       - Power consumption rises proportionally to beacon rate. Battery-
         powered nodes need the auto-revert hard-deadline to be enforced.
       - Outlier gate (15 dB default) needs to be re-tuned at higher
         sample rates or it'll reject too aggressively. Active mode might
         pair with outlier_db = 25, then revert.
    PRIORITY: Medium. Big UX win for calibration alone. Pairs naturally
              with #38 (runtime tunable intervals) — implement them
              together as one feature.

43. Local build leaves firmware_version field EMPTY in MQTT messages
    OBSERVATION (2026-04-25):  After bootstrapping Delta and Echo via
    `pio run -t upload` from a developer workstation (not via CI), both
    devices report:
        firmware_ts: 1745539200    ← matches v0.4.06 build timestamp
        firmware_version: ""       ← empty (BUG)
    Whereas Charlie (OTA-updated from CI-built artifact) reports:
        firmware_version: "0.4.06" ← correct

    ROOT CAUSE:
        platformio.ini line 74 unconditionally injects:
            -DFIRMWARE_VERSION_OVERRIDE=\"${sysenv.FIRMWARE_VERSION}\"
        When the FIRMWARE_VERSION env var is unset (local dev), the
        macro becomes:
            -DFIRMWARE_VERSION_OVERRIDE="\"\""
        config.h then sees OVERRIDE *defined* (it can't tell defined-as-
        empty apart from defined-with-value at preprocessor time):
            #ifdef FIRMWARE_VERSION_OVERRIDE
            #define FIRMWARE_VERSION FIRMWARE_VERSION_OVERRIDE   ← ""
            #else
            #define FIRMWARE_VERSION "0.4.06"   ← never reached
            #endif
        Result: FIRMWARE_VERSION = "".

    FIX OPTIONS (in increasing complexity):
        a) Easiest — modify modify_link_path.py (the existing pre-build
           hook) to read FIRMWARE_VERSION env var and only inject the
           OVERRIDE flag when it's set and non-empty. ~5 lines.
        b) Alternative — make platformio.ini build_flags conditional via
           `[env:esp32dev_local]` that omits the override flag entirely
           and rely on config.h fallback. Requires CI to use the other
           env-name explicitly.
        c) C-level — change config.h to use a runtime-selected pointer
           that picks FALLBACK when OVERRIDE is empty. Requires every
           consumer to use a function call instead of a string literal.
           Disqualified — too invasive.

    IMPACT: Heartbeats/boot announcements with empty firmware_version
    are useless for fleet inventory and OTA decision-making. The
    Recent Abnormal Reboots tile rendering shows fw="" rows for
    locally-flashed devices, which obscures triage.

    PRIORITY: Medium. Block by Phase 2 firmware work — fix in v0.4.07.

    ADDRESSED v0.4.10 (2026-04-25):  Option (a) chosen — config.h fallback
    literal updated to "0.4.10-dev" so locally-flashed dev binaries are
    distinguishable from CI-built releases. modify_link_path.py already
    performs the conditional override correctly (only injects when env
    var is set + non-empty); the bug was that the fallback literal had
    no "-dev" suffix despite the comment claiming it did. Future
    version bumps should keep this literal in sync with the next
    release tag (e.g. when bumping to v0.4.11, set fallback to
    "0.4.11-dev"). Documented inline in config.h.

44. Addressable LED status colors (green/yellow/red) not lighting
    STATUS: RESOLVED 2026-04-27 in v0.4.13 via deferred-flag pattern.
    onMqttConnect() (async_tcp task) sets _mqttLedHealthyAtMs=millis();
    mqttHeartbeat() (loopTask) consumes the flag and posts the
    LedEventType::MQTT_HEALTHY event safely. Visual confirmation:
    green breathing observed on Alpha's WS2812 strip post-OTA.
    Pattern documented as canonical in TWDT_POLICY.md.
    OBSERVATION (2026-04-25):  Devices in the triangle (Charlie/Delta/
    Echo) and Bravo on USB-PC do not exhibit the documented status-LED
    behaviour: green when WiFi+MQTT are healthy, yellow during OTA, red
    on rollback or fault. Bravo specifically has WS2812 addressable LEDs
    on D27 (per session bring-up) but only shows the slow blue
    "WIFI_CONNECTING" pulse — no transition to green when MQTT connects.

    POSSIBLE CAUSES (need bench-side investigation):
        a) ws2812.h / status-LED logic ties color to a state enum that
           may not be updated on MQTT_CONNECTED transition (only on
           WIFI_CONNECTED).
        b) The LED pin (D27) may not match what ws2812.h is initialised
           with — historically the firmware has used D2 or another pin
           on the ESP32-DEVKIT V1 boards. Bravo specifically has an
           external WS2812 strip wired to D27.
        c) APP_CFG_LED_PIN / status-LED feature flag may be disabled in
           AppConfig defaults; only the built-in blue LED (GPIO2) blinks.

    INVESTIGATION STEPS:
        1. Read include/ws2812.h to confirm what state-machine maps to
           which colors and whether MQTT_CONNECTED triggers green.
        2. Confirm AppConfig.led_pin (or compile-time LED_PIN) for each
           device: factory-flashed devices may default to a different
           pin than Bravo's D27 strip.
        3. Trigger OTA on a device with serial monitor running; watch
           for the LED-state log lines and confirm color transitions.

    PRIORITY: Low — diagnostic visibility only. Triangle-position
              calibration work (#39, #41.7) does not depend on LED
              status. Park as a separate maintenance task.

    ADDRESSED v0.4.10 (2026-04-25):  Cause (a) confirmed — the WS2812
    state machine in include/ws2812.h had no MQTT_CONNECTED transition.
    onMqttConnect() in mqtt_client.h only called ledSetPattern() which
    drives the on-board GPIO2 status LED, never the WS2812 strip. Fix:
    added LedState::MQTT_HEALTHY (slow 4-s green breathing, distinct
    from RFID_OK's solid green), a matching LedEventType::MQTT_HEALTHY
    event, and posts from onMqttConnect() (→ green) and onMqttDisconnect()
    (→ revert to BOOT_INDICATOR "wifi" fast blue pulse). Status table
    publishes "mqtt_healthy" string. RFID/OTA overlays now restore to
    MQTT_HEALTHY instead of IDLE because _ledPreviousState is set when
    the event fires.

45. Fleet-control buttons (OTA/Restart/Firmware) need stagger
    OBSERVATION (2026-04-25):  All three fleet-wide control buttons in
    the Device Status tab fire MQTT commands SIMULTANEOUSLY (zero delay
    between devices). Audit of flows.json:

       - "Send OTA Update"  (hb_ota_btn → hb_cmd_fn): forEach UUID,
                            node.send(cmd/ota_check) — no setTimeout.
       - "Restart All ESP32s" (hb_restart_btn → hb_restart_fn):
                            forEach UUID, node.send(cmd/restart) —
                            no setTimeout.
       - "Send Firmware" — DOES NOT EXIST as a fleet-level button.
                           Only the OTA-check exists; firmware push is
                           inferred (the ESP32-OTA-Pull library reads
                           ota.json and self-fetches).

    IMPACT (proven on 2026-04-25 v0.4.06 release):
       Three devices simultaneously rebooting after OTA cause AP
       association contention; all three fail Phase-2 validation and
       roll back. See entry #35 case study for full chain.

    REQUESTED CADENCE (operator):
       - cmd/ota_check fanout    — 5 min stagger between devices.
                                   (Allows v_new boot, MQTT-validate,
                                    soak before next device starts.)
       - cmd/restart fanout      — 1 min stagger between devices.
                                   (Allows boot + MQTT reconnect
                                    before next device drops off.)
       - Firmware push (if added) — 1 min stagger.

    IMPLEMENTATION:
       - Replace the synchronous forEach in hb_cmd_fn / hb_restart_fn
         with a setTimeout chain:
            uuids.forEach(function(uuid, i) {
                setTimeout(function() {
                    node.send({ topic: ..., payload: '' });
                }, i * STAGGER_MS);
            });
       - Where STAGGER_MS = 300000 for OTA, 60000 for restart.
       - Add a flow-context flag to suppress double-clicks during
         an in-flight rollout.
       - Visual indicator: the canary-OTA RFC in #35 covers this in
         depth. The minimum acceptable interim is a textual "Rollout
         in progress: 1 of 5 — ESP32-Charlie" banner.

    PRIORITY: HIGH — every future fleet OTA without stagger WILL hit
                     the same rollback cascade documented in #35.
                     This is the bare-minimum interim before the full
                     #35 staggered-OTA RFC is implemented.

    ADDRESSED v0.4.10 (2026-04-25):  Patched hb_cmd_fn and hb_restart_fn
    via Node-RED admin API. Behavior:
      - "Send OTA Update" (payload "ota") — 5 min stagger, sorted by
        friendly name (Alpha → Bravo → ...), with canary cancel-on-
        failure: at each step the function inspects flow.boot_history
        and aborts if any previously-triggered device has logged a
        task_wdt / int_wdt / panic / brownout boot since chainStartMs.
        This delivers the bare-minimum interim AND much of the full
        #35 canary RFC in one change (visual indicator: node.status()
        shows "OTA: 2/5 (ESP32-Bravo)" and flips to red on abort).
      - "Check Firmware Version" (payload "check") — 2 s stagger
        (cheap, no AP contention).
      - "Restart All ESP32s" — 5 min stagger (changed from the originally
        requested 1 min after observing real-world MQTT-reconnect time
        of ~30-90 s; 1 min was too tight on Bravo).
    flow.hb_ota_chain_started_ms / _total / _done are exposed for any
    future status-banner UI (#45 final paragraph).
    STATUS: RESOLVED v0.4.10 (2026-04-25). The ADDRESSED block above
    is the implementation. Index moved to RESOLVED 2026-04-28.

46. Recent Abnormal Reboots — fleet-wide WDT / panic investigation
    OBSERVATION (2026-04-25, post-OTA-chaos):  All 5 devices in the
    fleet currently have abnormal boot reasons in their LATEST retained
    boot announcement (captured via mosquitto_sub):

       Charlie (v0.4.06):  task_wdt
       Alpha   (v0.4.06):  software   ← clean esp_restart()
       Bravo   (v0.4.05):  int_wdt
       Delta   (v0.4.06):  task_wdt
       Echo    (v0.4.06):  other_wdt

    The "Recent Abnormal Reboots" dashboard tile (boot_history_grp +
    boot_history_fn at flows.json:2673) correctly captures and dedups
    these. The tile WORKS — the question is why the underlying
    boot reasons are happening on healthy-looking devices.

    POSSIBLE CAUSES:
       a) RESIDUAL FROM TODAY'S CHAOS — the retained boot announcement
          reflects the LAST boot, which for most devices was during
          the OTA-rollback session today. If the next clean reboot
          happens via clean esp_restart() these will all flip to
          'software' or 'poweron'. Easy to verify: trigger a clean
          reboot on each device and re-capture.
       b) v0.4.06 SUBSYSTEM RACE — peer_tracker.h F4 outlier-streak
          field was added in v0.4.06; if the new code path takes
          longer than the task watchdog grace period under high RSSI
          throughput it could trigger task_wdt. Unlikely (the new
          code is ~10 instructions per frame), but worth ruling out
          with a soak test on Charlie.
       c) AP-CONTENTION LINGERING — devices that fell into AP-mode
          portal during OTA rollback may have been hard-rebooted by
          watchdog while waiting for the operator. The other_wdt on
          Echo is consistent with this.
       d) PER-MODULE FLAKE — Bravo specifically has historical OTA
          panic issues (see #35 addendum). Its int_wdt today may be
          unrelated to v0.4.06.

    INVESTIGATION PLAN:
       1. Trigger cmd/restart on each device (one at a time, 60 s apart
          per #45) and re-capture retained boot announcement. If
          subsequent boots show 'software', the WDT entries were
          residual.
       2. Soak Charlie/Delta/Echo for 1 h with ranging enabled and
          ESPNOW_BEACON_INTERVAL_MS at default. Check for new
          task_wdt entries.
       3. If new task_wdt entries appear, capture serial trace of the
          panic to identify the stuck task.

    PRIORITY: Medium-High — task_wdt and int_wdt are NEVER acceptable
              in steady state. Investigate after #45 stagger lands so
              the cleanup-restart can be done safely.

    UPDATE 2026-04-28 morning — DECODED. Built v0.4.20 ELF in a worktree
    (`git worktree add /c/Users/drowa/v04-20-decode v0.4.20 && pio run`)
    and ran addr2line on the 14-frame backtrace. Result is a perfect
    match for the #51 bad_alloc cascade shape:

       panic_abort
         abort()
           std::terminate()
             __cxa_throw                              ← bad_alloc
               operator new(unsigned int)
                 std::vector<unsigned char>::reserve  ← AsyncMqttClient buffer
                   AsyncMqttClient::PublishOutPacket ctor (Publish.cpp:42)
                     AsyncMqttClient::publish (AsyncMqttClient.cpp:742)
                       mqttPublish (mqtt_client.h:230)
                         espnowRangingLoop (mqtt_client.h:396 / espnow_ranging.h:650)
                           loop (main.cpp:726)
                             loopTask
                               vPortTaskWrapper

    SIGNIFICANCE — the v0.4.11 heap-guard
    (`ESP.getMaxAllocHeap() >= MQTT_PUBLISH_HEAP_MIN` where
    MQTT_PUBLISH_HEAP_MIN = 4096) is INSUFFICIENT. The heap-guard at
    mqtt_client.h:216 passed, then the function built `String topic =
    mqttTopic(prefix)` at line 229 (5-7 String concat allocations),
    THEN called `_mqttClient.publish()` which internally `vector::
    reserve()`s a contiguous buffer. By the time the reserve runs, the
    String concatenations have fragmented the heap below the
    publish's contiguous-block need; the bad_alloc fires.

    FIX SHIPPED v0.4.22 (in flight as of this writing):
       1. Re-check ESP.getMaxAllocHeap() AFTER the topic String build,
          immediately before the _mqttClient.publish() call. The first
          check still gates the LOG_D + topic build; the second check
          gates the actual library call where bad_alloc would fire.
       2. Bump MQTT_PUBLISH_HEAP_MIN 4096 → 8192. Defensive margin for
          the AsyncMqttClient internal vector reserve + async_tcp send
          buffer + any concurrent allocations. Empirically, our largest
          payload is ~640 B (status JSON heartbeat); 8 KB largest-
          contiguous gives ~12× margin.
       3. Wrap `_mqttClient.publish()` in try { } catch (std::bad_alloc&) { }
          as defense-in-depth. arduino-esp32 has exception support
          compiled in (proven by the std::terminate frame in the
          decoded backtrace). Caught bad_alloc → LOG_W + drop, same
          UX as the heap-guard skip.

    HISTORICAL — the original Charlie 02:44 panic (v0.4.10 #51 root
    cause investigation) had the SAME backtrace shape. v0.4.11 added
    the threshold-4096 guard and that LOOKED to fix it through the
    cascade-fix marathon. Tonight's Alpha panic confirms the guard
    was load-bearing but undersized.

    Alpha (production v0.4.20 release, CI build app_sha_prefix
    "a5bb3114") panicked once after ~3 hours of clean uptime. Fleet
    was steady, no chaos in flight, no broker churn. Auto-recovered
    via reboot; uptime restarted from 0; subsequent boot announcement
    confirms `boot_reason:"panic"`.

    The v0.4.17 coredump-to-flash path captured the backtrace cleanly
    (the whole point of #65 sub-E):

       exc_task: loopTask     (NOT async_tcp — different from
                                tonight's earlier Bravo/Charlie
                                coredumps which were async_tcp /
                                lwIP raw.c)
       exc_pc:   0x4008ec14
       exc_cause: IllegalInstruction (PC pointing at non-code
                                     memory; classic memory corruption
                                     symptom)
       backtrace: 0x4008ec14 0x4008ebd9 0x400954ad 0x401ae04b
                  0x401ae080 0x401ae15f 0x401ae1f2 0x400e4b0d
                  0x400e2c81 0x400ee19e 0x400f8bc2 0x40100023
                  0x40107ce4 0x4008ff31

    Cannot decode locally — Alpha's app_sha_prefix "a5bb3114" is the
    CI v0.4.20 binary; local builds produce SHA prefix "dd877030".
    Need either the CI artefact's ELF (may be in GitHub Actions
    artefacts retention window) or a build-from-tag-v0.4.20-source-
    SHA pio run to reproduce.

    Significance: this is a SEPARATE bug from #78 (which is async_tcp
    context). It's in loopTask — our main loop, where MQTT publishes
    + heartbeats + ESP-NOW ranging happen. IllegalInstruction
    typically means a corrupted function pointer; combined with the
    long-uptime trigger, fits a slow heap leak or stack overflow
    that eventually overwrites a vtable.

    ACTION: include this Alpha coredump in the next-session #78 /
    #76 sub-G follow-up audit. Charlie's canary soak (continuous
    via OTA_DISABLE) is now the fleet's primary stack-overflow
    detector — if Charlie hits the canary halt with a similar
    frame count, the corrupted-stack hypothesis gets confirmed.

47. Hardware verification of #39 multi-point + #41.7 per-peer calibration
    STATUS: Firmware shipped in v0.4.07 (#39 linreg) and v0.4.09 (#41.7
    per-peer constants). Dashboard UI shipped on 2026-04-25 with
    "Add measurement @ distance" button, "Clear points buffer" button,
    linreg R² + RMSE in commit response, and the "cal" ✓ badge column
    in the peer table. NOT YET VERIFIED ON HARDWARE — deferred so
    other-priority work could land first.

    PRECONDITIONS for the test:
       - Calibrating device on v0.4.09 (currently only Bravo).
       - Target peer on any v0.4.06+ firmware (the calibration
         only writes to the calibrating device's NVS; the peer just
         needs to be broadcasting beacons).
       - Operator has a tape measure or known-distance markers.

    PROCEDURE (per pair, ~5 minutes):
       1. In the v2 dashboard, set "Calibrating device" = Bravo,
          "Target peer" = (the peer you want).
       2. Place Bravo at exactly 1.0 m from the target peer.
          Click "Measure @ 1 m". Wait for the green tick + buffered
          points = 1.
       3. Move Bravo to 2.0 m. Set the "distance (m)" form to 2.0.
          Click "Add measurement @ distance". Wait for tick +
          buffered points = 2.
       4. Move Bravo to 3.0 m (or 4 m). Repeat. Stop when 3-5 points
          are buffered, or sooner if the room runs out.
       5. Click "Commit". Wizard status shows the linreg result:
            tx_power_dbm, path_loss_n, R², RMSE, scope=per_peer
       6. Verify the peer table's "cal" column for that peer flips
          to ✓ within 2 s.
       7. Verify Bravo /espnow publish shows cal_entries++ and the
          target peer's "calibrated":true.

    ACCEPTANCE CRITERIA:
       - R² ≥ 0.95 in a clean indoor room
       - tx_power_dbm in [-65, -45] (per quality-check thresholds)
       - path_loss_n in [2.0, 4.0]
       - RMSE < 2.0 dB
       - Peer-specific distances on the chart converge near the
         physical placement after commit (sanity check).

    FULL FLEET COVERAGE (deferred):
       To get per-peer calibration on every pair (6 pairs × 2
       directions = 12 entries for a 4-device fleet):
          - Repeat the procedure with each device taking the role of
            "Calibrating device". Currently blocked on triangle
            devices being on v0.4.08; serial-flash to v0.4.09 would
            unblock this. See #35 chicken-and-egg notes.

    PRIORITY: Medium. The asymmetry findings in
              docs/SESSIONS/RF_CONFIG_TEST_2026_04_25.md show this is the
              biggest remaining accuracy lever. Until the
              verification runs, the firmware/UI work is "shipped
              but unproven". Capture the resulting tx_power/n
              values per pair as Part C of RF_CONFIG_TEST_2026_04_25
              when the test is run.

48. Device UUID drift — Delta and Echo had unexpected UUIDs on 2026-04-25
    OBSERVATION (2026-04-25, post-v0.4.10 OTA chain):  Three of the four
    OTA commands in the staggered fleet rollout silently failed because
    Delta and Echo were publishing under DIFFERENT UUIDs than the values
    that had been carried in CLAUDE.md / fleet scripts:

       Expected (from prior session notes):
         Delta: 2b89f43c-c66e-4b30-9020-ff7da99ac3eb
         Echo:  2fdd4112-c2cc-43d2-bdc0-30cd76b09ec0

       Actual (mosquitto.log evidence on 2026-04-25 ~19:21 SAST):
         Delta: 2b89f43c-2fd8-4ed6-ac9d-fb0d8f97c282
         Echo:  2fdd4112-9255-42a8-a099-ada0075a677b

    Note: the FIRST 8 hex characters match — same prefix, different
    suffix. That rules out "we swapped two devices" and is consistent
    with the UUID being PARTIALLY regenerated. DeviceId::get() is
    expected to load a single immutable UUID from NVS namespace
    "esp32id"; if the namespace is missing, a NEW UUID is generated
    and persisted. So the prefix matching is suspicious — needs
    code-level inspection.

    POSSIBLE CAUSES:
       a) NVS namespace "esp32id" was wiped at some point — full
          reflash (esptool erase_flash) followed by upload would
          do this. Delta has been reflashed multiple times during
          the calibration work; Echo less so.
       b) DeviceId::ensure() / generate() bug — partial-write to
          NVS leaves a half-formed UUID that gets rebuilt next
          boot, yielding new bytes after the prefix.
       c) Two UUIDs co-existed in NVS at one point (e.g. credential
          rotation race) and the wrong one became authoritative.
       d) Operator reset via the AP portal — does the portal
          regenerate the device UUID? If so, that's the leak.

    INVESTIGATION STEPS:
       1. Read include/device_id.h (or equivalent) and trace every
          path that writes NVS_KEY for the UUID. Is there any code
          that REGENERATES the UUID after the first boot?
       2. Check git log for the device_id module — was a regeneration
          path added in v0.4.x that didn't exist earlier?
       3. On Delta/Echo: dump NVS namespace "esp32id" via serial
          (add a temporary debug command) and confirm only one
          UUID is stored.
       4. After flashing v0.4.10 to Delta + Echo, capture their
          boot UUIDs. If they change again on a clean reflash that
          PRESERVES NVS, that's smoking gun for cause (b) or (c).

    IMPACT: Operational. Fleet scripts that hardcode UUIDs silently
            fail when devices "rotate" their UUID. Workaround in
            place (always resolve UUIDs from live MQTT first) but
            the root cause should be fixed — a stable per-device
            UUID is foundational for credential rotation, OTA
            tracking, and the boot_history dedup logic.

    PRIORITY: Medium. Workaround is viable for now; track silent
              UUID regeneration as a stability bug worth fixing
              before the relay+Hall hardware (v0.5.0) ships, since
              that scope adds another set of devices that will need
              stable identities.
    STATUS (v0.4.11): Visibility improvement shipped — firmware now logs
              a WARN if UUID is regenerated at boot and publishes the
              actual UUID in the /status payload so MQTT reflects the
              truth. Root cause (NVS partial-wipe path) still open;
              targeted fix deferred to v0.5.0 (#48 dependency).

49. Bootstrap protocol does not propagate OTA URL to new siblings
    OBSERVATION (2026-04-25, during v0.4.10 fleet OTA):  Delta serial
    log captured during MQTT-triggered OTA shows:

       [I][OTA] Manifest fetch 1/3: https://myorg.github.io/esp32-firmware/ota.json
       [W][OTA] Manifest fetch 1/3 failed (code 404)
       [I][OTA] Manifest fetch 2/3: https://dj803.github.io/esp32_node_firmware/ota.json
       [I][OTA] Manifest fetched from fallback URL #1

    The PRIMARY URL `myorg.github.io/esp32-firmware/ota.json` is the
    placeholder LITERAL from include/config.h:239 (OTA_JSON_URL define).
    Delta's NVS field gAppConfig.ota_json_url is empty, so the firmware
    fell through to the compile-time default — which has never been a
    real deployment URL.

    OTA only succeeded because the OTA_FALLBACK_URLS array hardcodes
    the correct dj803 URL as fallback #1. Remove that safety net and
    Delta + Echo would NEVER have OTA'd.

    ROOT CAUSE (probable):  The ESP-NOW bootstrap protocol exchanges
    credential bundles (WiFi SSID/password + MQTT URL + keys) between
    siblings, but the OTA URL is a SEPARATE field with its own
    message type pair (ESPNOW_MSG_OTA_URL_REQ 0x05 /
    ESPNOW_MSG_OTA_URL_RESP 0x06) that the new sibling apparently
    does NOT request after credential bootstrap completes. So:

       1. New device boots, has no credentials  → ESP-NOW broadcast
          credential request.
       2. Sibling responds with credential bundle (NO ota_json_url).
       3. New device persists credentials, connects WiFi+MQTT, marks
          itself OPERATIONAL.
       4. New device NEVER asks any sibling "what is your OTA URL?"
       5. Result: gAppConfig.ota_json_url stays empty; compile-time
          placeholder is used; manifest fetch falls through to
          OTA_FALLBACK_URLS.

    This is consistent with #48 — Delta + Echo were re-bootstrapped
    at some point (which also explains the unexpected UUID change
    if NVS was wiped before re-bootstrap).

    INVESTIGATION STEPS:
       1. Read include/espnow_responder.h and trace whether the
          credential bundle includes ota_json_url, OR whether the
          bootstrap state machine fires ESPNOW_MSG_OTA_URL_REQ as
          a follow-up after persisting credentials.
       2. If ota_json_url is missing from the credential bundle:
          add it (one extra string field, version-bump
          ESPNOW_PROTOCOL_VERSION to keep mixed fleets safe).
       3. Alternatively: have the new sibling send
          ESPNOW_MSG_OTA_URL_REQ once after MQTT comes up; persist
          the response to NVS and update gAppConfig.
       4. Best hardening: have the firmware log a WARN line on
          every boot if gAppConfig.ota_json_url is empty / equals
          the config.h placeholder, so the failure mode is visible
          in MQTT before the next OTA attempt.

    IMPACT: Tied to #48 — every freshly-bootstrapped sibling has
            silently been using the safety-net fallback URL since
            the manifest URL changed. Today this works because the
            fallback is correct, but it's a latent failure: changing
            the manifest URL would brick OTA for any device whose
            NVS was wiped without re-provisioning via the AP portal.

    PRIORITY: HIGH — same priority class as the OTA stagger (#45),
              because OTA reliability is the foundation for every
              future fleet operation. Cheap fix (~50 lines) and
              the visibility win alone justifies it.
    STATUS (v0.4.11): Visibility improvement shipped — firmware now logs
              a WARN on boot if gAppConfig.ota_json_url is empty or
              matches the config.h placeholder, so the failure mode
              surfaces in serial/MQTT before the next OTA attempt.
              Propagating the OTA URL through the bootstrap bundle
              (root fix) still open; targeted for v0.5.0.

50. esptool v5.2 erase-flash does NOT wipe NVS on first-power-on chips
    OBSERVATION (2026-04-25):  Brand-new ESP32 module ("Foxtrot",
    MAC 28:05:a5:32:50:44, factory shrink-wrapped, never flashed)
    flashed with v0.4.10 immediately after running:

        python -m esptool --chip esp32 --port COM5 erase-flash

    Output: "Flash memory erased successfully in 1.8 seconds."

    1.8 seconds is suspicious — a typical 4 MB SPI NOR chip-erase takes
    30+ s, not 1.8 s. Likely the command performed a sector-level erase
    of a single region rather than a true chip-erase.

    First-boot serial log on Foxtrot:
        [DeviceId] Loaded UUID: 3b3b7342-80e7-43dd-afc7-78d0470861e2
        [AppConfig] OTA JSON URL: https://dj803.github.io/esp32_node_firmware/ota.json
        [BOOT] Admin credentials found — skipping bootstrap
        [WiFi] Association attempt 1 of 3 — SSID: Enigma
        [MQTT] Connected to broker

    All three of these REQUIRE pre-existing NVS state:
        - "Loaded UUID" (vs. "Generated new UUID") = device_id.h read NVS,
          found a stored UUID, used it.
        - OTA URL = correct dj803 URL = previously saved AppConfig.
        - "Admin credentials found" = CredentialStore::hasPrimary()
          returned true = NVS namespace "esp32cred" had src=ADMIN.

    On a TRULY blank chip, all three should be:
        - "Generated new UUID: ..."
        - OTA URL = config.h placeholder ("myorg.github.io...")
        - bootstrap path entered (no admin creds in NVS)

    This finding REOPENS the diagnosis of #48 + #49:
        - The "UUID drift" theory for Delta/Echo (#48) needs revisiting.
          If chip-erase silently doesn't wipe NVS, then a re-flash without
          a separate NVS-erase step leaves stale state — including stale
          UUID.
        - The "OTA URL not propagated by bootstrap" theory for #49 may
          be partly wrong: AppConfig.ota_json_url was the correct dj803
          URL on Foxtrot. The earlier Delta/Echo fallthrough to the
          placeholder might have been a different bug (e.g. NVS read
          racing with first boot).

    INVESTIGATION STEPS:
       1. Re-run the experiment with explicit NVS-region erase:
            python -m esptool --chip esp32 --port COM5 erase-region 0x9000 0x6000
          (assuming partitions.csv places NVS at 0x9000, 24 KB). Or
          read partitions.bin to get the exact NVS range.
       2. After the explicit erase, re-flash and watch serial. If
          "Generated new UUID" + bootstrap path now appear, the
          chip-erase command was the culprit.
       3. File an upstream esptool issue if confirmed; pin to v4.x
          or use `erase-region` for NVS until fixed.
       4. Update CLAUDE.md "Build & Test" section to use a known-good
          erase sequence for fresh-device provisioning.

    THEORIES BEYOND esptool BUG (2026-04-25 update — operator confirms
    Foxtrot was sealed shrink-wrapped, never powered, never flashed):

       T1. esptool erase-flash silently no-ops or partial-erases on
           this chip variant. The 1.8 s erase time supports this.

       T2. PySerial DTR-toggle on port-open caused a reset between
           firmware first-boot and our serial capture, so the log we
           see is the SECOND boot. The FIRST boot ran ESP-NOW
           bootstrap from a healthy sibling (Alpha/Bravo/etc were all
           on v0.4.10 by this point and broadcasting), persisted
           creds + UUID + OTA URL, then DTR-reset interrupted, and
           on second boot the NVS was already populated.
           This theory is WEAKENED by the bootstrap window math:
           ESP-NOW channel scan needs 13 × 500 ms = 6.5 s minimum
           per attempt, and the gap between write-flash hard-reset
           and pyserial open is ~1-2 s. Bootstrap couldn't complete
           in that window — UNLESS the new sibling-respond path is
           faster than the spec timing, or a sibling responds on
           the FIRST channel hit (Wi-Fi channel 6 is the active one,
           which a fresh device would scan early).

       T3. The dev-board vendor flashed test firmware that happens
           to share our NVS namespace names ("esp32cred", "esp32id",
           "esp32appcfg") and persisted the user's credentials
           somehow. Vanishingly unlikely.

       T4. Factory eFuse / OTP region holds default values that the
           firmware reads. ESP32 has eFuse but it's chip-id / MAC,
           not arbitrary key/value pairs. Unlikely.

       Operator-recommended next step: re-erase + isolate Foxtrot
       from the broadcast range of other v0.4.10 nodes (power down
       the fleet, OR move Foxtrot to a different room) to rule out
       theory T2. If the same NVS state still appears after that,
       T1 is confirmed.

    RESOLVED 2026-04-25 (same session — fleet-isolation re-test):
       Sequence: powered down all 5 v0.4.10 fleet nodes, then on
       Foxtrot:
          esptool --after no-reset erase-flash       (2.0 s — same fast time)
          esptool --after no-reset write-flash ...
          pyserial open with controlled RTS reset

       First-boot serial captured cleanly:
          [E][Preferences.cpp:47] begin(): nvs_open failed: NOT_FOUND
          [I][ESP-NOW] My MAC: 28:05:A5:32:50:44
          [I][ESP-NOW] Async bootstrap attempt 1 of 3
          [I][ESP-NOW] Init OK — scanning channels 1–13
          [W][ESP-NOW] Timeout — no sibling response
          (repeats for attempts 2 and 3)

       CONCLUSION:
          T1 (esptool bug) → REJECTED. erase-flash IS clearing NVS
            properly. The NOT_FOUND error on nvs_open is the
            authoritative proof of an empty namespace.
          T2 (DTR-induced second-boot) → CONFIRMED. The previous
            "Loaded UUID + Admin credentials found" log was a
            SECOND boot. Between write-flash hard-reset and
            pyserial DTR-open (~1-2 s wall-clock), Foxtrot:
              (a) booted ROM bootloader (~300 ms)
              (b) booted firmware app (~500 ms — faster than spec)
              (c) found a sibling on Wi-Fi channel 6 (the active
                  channel) within sub-second by sheer luck of
                  scanning channel order — much faster than the
                  6.5 s worst-case spec
              (d) received credential bundle, persisted NVS,
                  generated UUID, persisted UUID, advanced to
                  WIFI_CONNECT
              (e) PySerial open asserted DTR → reset
              (f) second boot showed populated NVS

       SECONDARY FINDING (revises #49): the OTA URL on Foxtrot's
       second boot was the CORRECT dj803 URL. So the credential
       bundle exchanged via ESP-NOW DOES include ota_json_url
       (or AppConfig's load() applies a smart default). #49's
       theory ("OTA URL not propagated") is partly wrong — needs
       a code-level audit of CredentialBundle wire format to
       confirm. The Delta/Echo OTA-URL fallthrough that opened
       #49 must have a different cause; possibly Delta/Echo were
       provisioned BEFORE the firmware version that added
       ota_json_url to the bundle.

    UPDATED PRIORITY:  esptool tooling = NOT a bug. Drop priority.

    RECONFIRMED 2026-04-27 ~22:00 SAST. Ran the test again on Bravo
    (COM4, MAC F4:2D:C9:73:D3:CC):
       1. Pre-erase UUID: 6cfe177f-92eb-4699-a9a6-8a3603aae175
       2. esptool erase-flash via PIO toolchain (3.7 s — slower than
          Foxtrot's 2.0 s, suggests full chip-erase on this part)
       3. pio run -e esp32dev -t upload (rebuilds + flashes release)
       4. First boot via firmware reset captured by my MQTT subscribe
          (no DTR-second-boot interference because I sub'd from a
          different host)
       5. New UUID: ece1ed31-4096-488b-a083-d5880002c223 (DIFFERENT)
       6. node_name empty (also wiped from NVS, expected)
       7. Bootstrap via ESP-NOW recovered Wi-Fi creds + broker URL
          from a sibling within seconds; first heartbeat at uptime_s=60.

    erase-flash works as expected on this chip variant too.
    CLAUDE.md + daily_health_config.json updated to the new UUID.
    The old UUID's retained payloads were cleared via mosquitto_pub
    -n -r to avoid confusing future fleet snapshots.
       Workflow change still warranted: when capturing serial of
       a fresh provisioning, USE `--after no-reset` on esptool
       AND open serial with controlled RTS reset (do NOT let
       pyserial DTR-toggle the chip). Documented in CLAUDE.md.

    IMPACT:  Provisioning workflow is broken — operators believe a
             chip-erased device is fresh, but it actually retains
             credentials, UUID, OTA URL, and any other NVS state.
             Combined with #48, this means a re-flashed device may
             silently come up with a NEW UUID (if device_id.h has a
             retry path that regenerates on read failure) AND old
             credentials, leading to identity confusion in the fleet.

    PRIORITY: HIGH — affects every future device provisioning.
              Trivial workaround (explicit erase-region) but the
              one-line tooling fix prevents days of triage on
              future "why is this fresh device behaving like an
              old one" investigations.

# ── #48 UPDATE 2026-04-25 (live capture) ─────────────────────────────
Foxtrot UUID drift caught LIVE during the same session it was first
provisioned. Sequence:
  1. Foxtrot first-boot bootstrap → UUID 3b3b7342-80e7-43dd-afc7-78d0470861e2,
     creds + OTA URL + UUID persisted to NVS.
  2. Subsequent restart (USB cable wiggle / RTS reset / similar) →
     fresh boot serial shows  UUID c1278367-21af-478d-8a8b-0b84a4de60df.
     Same physical chip, same NVS partition (admin credentials still
     present), but UUID is new.

This rules out the "NVS wiped" hypothesis for #48 — credentials AND
ota_json_url survived the restart, only the UUID was regenerated. So
DeviceId::init() is reading SOMETHING wrong on the second boot and
falling through to the regenerate path (Serial.printf "Generated new
UUID:" was suppressed in this capture but the new value proves it
ran). Smoking gun is in include/device_id.h — likely the NVS namespace
"esp32id" is being opened in a way that fails intermittently after
first write, OR the load path has a subtle bug that returns success
with an empty buffer. CRITICAL fix path:
  - Add LOG_W on the UUID-regenerate branch in device_id.h so the
    issue is visible without serial capture.
  - Inspect prefsTryBegin(NVS_NAMESPACE="esp32id") for namespace-name
    string-length issues (limit 15 chars — "esp32id" is 7, fine).
  - Consider migrating UUID storage to the same namespace as
    credentials (esp32cred) to eliminate the multi-namespace race.


51. v0.4.10 stability regression — suspected LED MQTT_HEALTHY hooks
    OBSERVATION (2026-04-26 morning daily-health, RED status):
    Fleet experienced widespread instability overnight that does NOT
    fit a simple "mains power outage" explanation. boot_history
    captures from Node-RED (flow context boot_history on tab
    0cc8e394107eb034) show paired firmware crashes — not
    power-cycle reboots:

       2026-04-25 23:42:56  Alpha v0.4.10      panic
       2026-04-25 23:43:12  Delta v0.4.10-dev  int_wdt
       2026-04-26 00:04:26  Alpha v0.4.10      other_wdt
       2026-04-26 00:04:56  Delta v0.4.10-dev  other_wdt
       2026-04-26 ~08:54    Alpha v0.4.10      other_wdt   (live-observed)

    Mosquitto.log timeline of last-seen disconnects (all "exceeded
    timeout" except where noted):

       21:04   Echo     v0.4.10-dev  (mains; no power issue at this hour)
       23:02   Charlie  v0.4.10-dev  (battery — only death attributable to power)
       23:36   Foxtrot  v0.4.11-dev  ("closed by client" — caused by
                                      isolation-test pyserial RTS-reset)
       23:38   Bravo    v0.4.10-dev  (battery STILL CHARGED — operator
                                      confirmed; not power)
       02:05   Delta    v0.4.10-dev  (mains)
       08:54   Alpha    v0.4.10      (mains, currently in crash loop)

    OPERATOR CONFIRMS: only Charlie's death is explained by battery
    exhaustion. Bravo's battery still has charge. Mains was off
    overnight but mains-powered devices show panic / watchdog boot
    reasons, not poweron — i.e., they were running when they crashed.
    Plus Alpha continues to crash at idle ~8 hours after the most
    recent mains restoration.

    LEADING HYPOTHESIS — v0.4.10 LED MQTT_HEALTHY hooks:
       v0.4.10 introduced exactly two new firmware code paths that
       fire on every MQTT connect/disconnect (ref: include/mqtt_client.h
       in onMqttConnect — the ws2812PostEvent for LedEventType::MQTT_HEALTHY,
       and onMqttDisconnect — the ws2812PostEvent for LedEventType::BOOT_STATE
       with animName="wifi"). Every other v0.4.10 change is either
       cosmetic (config.h "0.4.10-dev" literal) or server-side
       (Node-RED stagger + canary). So the LED hooks are the only
       runtime addition.

    KEY DATAPOINT — only ALPHA has a WS2812 strip physically wired:
       Alpha is the only device in the fleet with the addressable
       LED strip on GPIO 27 actually connected. Bravo had the strip
       earlier in the day (it was on COM5 during the LED-fix verify)
       but the strip is now on Alpha. Other devices toggle GPIO 27
       with nothing attached — the data stream goes nowhere. So:
         - If the regression is purely software (queue post race,
           memory corruption), every device should crash equally.
           That fits Alpha + Delta + Bravo + Echo crashes.
         - If the regression is hardware (current spike, brownout
           when strip is driven), only Alpha would be hit. That
           fits Alpha being the most chronic crasher (3 events in
           9 hours) while Delta has 2 over the same window despite
           NO physical strip.

       The truth is probably "both" — a software defect that's
       easy to trigger and a hardware-amplified version of it on
       Alpha. Or, Delta's 2 crashes were artifacts of yesterday's
       OTA-chaos session and only Alpha's are live.

    CONFOUNDS to rule out:
       - Many of last night's crashes overlap my session activity
         (OTA chain test, Foxtrot fleet-isolation test, USB flash
         pass for v0.4.10 across 4 devices). The 23:42-00:04 window
         particularly was during multiple device repower cycles.
         Alpha's 08:54 crash is the cleanest "idle steady-state"
         data point — no operator activity at that moment.
       - GitHub Pages was disabled on the repo overnight (cause
         unclear — maybe inactivity-related auto-disable on free
         tier). Restored by operator 2026-04-26 ~08:50. UNRELATED
         to firmware crashes but scared us as a RED on the morning
         check.

    PROPOSED VERIFICATION:
       Cut a v0.4.10.1 patch reverting ONLY the two new
       ws2812PostEvent calls in mqtt_client.h (keep the LedState
       MQTT_HEALTHY enum + render case — they're inert without
       a poster). USB-flash Alpha. If Alpha stays up >4 h with
       no abnormal boot reasons, regression confirmed and the
       fix is to either (a) re-architect the post path to be
       fully async-tcp-safe, or (b) leave the LED feature out
       and revert to v0.4.09 visual behavior. If Alpha STILL
       crashes, hypothesis falsified — root cause is elsewhere
       (maybe the upgraded asynctcp / ESP-NOW interaction added
       in earlier v0.4.x).

    PRIORITY: HIGH — fleet is unreliable on v0.4.10. Either a
              quick revert ships, or v0.4.10 gets blacklisted
              and the fleet rolls back to v0.4.09 manually.

    STATUS: RESOLVED 2026-04-27 in the v0.4.13 → v0.4.20 cascade-fix
    marathon. Root cause was NOT the LED hooks — it was
    `MQTT_HUNG_TIMEOUT_MS = 12s` triggering simultaneous ESP.restart()
    on every device whenever a broker outage exceeded 12 s, then
    AsyncTCP _error path race during the restart storm. Fixed via:
       - v0.4.11 mqttPublish heap-guard for bad_alloc (intermediate)
       - v0.4.14 timeout 12s → 90s (intermediate)
       - v0.4.15 force-disconnect + 300s timeout (intermediate)
       - v0.4.16 pre-connect broker probe (FINAL FIX) — eliminates
         the disconnect-storm trigger; lwIP's natural ~75s SYN
         timeout never fires AsyncTCP's _error path.
    Validated fleet-wide via M3 (180s outage) on 2026-04-27 17:59:
    6/6 devices reconnected via event=online preserving uptime, zero
    panics, zero abnormal boots. Same M3 on v0.4.15 produced 4/4
    abnormal boots — clear delta. The LED MQTT_HEALTHY hooks themselves
    were re-implemented safely via deferred-flag pattern in v0.4.13
    (see #56). The latent AsyncTCP `_error` path race in lwIP
    raw_netif_ip_addr_changed (Wi-Fi flap trigger, NOT broker) is
    tracked separately as #78.

52. Node-RED file logging not configured (observability gap)
    OBSERVATION (2026-04-26):  Node-RED is running as PID 2184 since
    2026-04-25 11:57:38 (started in a Command Prompt with stdout
    redirected). Operator reported the log file appearing frozen at
    20:30 last night, which was during the v0.4.10 fleet-wide crash
    window (#51). Investigation showed:
       - No *.log files exist anywhere under ~/.node-red/
       - No ProgramData/node-red/ logs
       - No Node-RED running as a Windows service
       - Logging falls back to stdout, captured by the cmd window's
         line buffer; buffer flushes only on full / window close

    IMPACT:  When something goes wrong overnight (broker drops,
    mass-disconnect, runtime exception), there is no historical
    record. Today the boot_history flow context recovered enough
    info to triage #51, but only because that flow was already
    capturing retained boot announcements; runtime errors / flow
    exceptions / MQTT-broker disconnects from Node-RED's side are
    lost.

    PROPOSED FIX:  Add a file logger to ~/.node-red/settings.js:

        logging: {
            console:  { level: 'info' },
            file:     { level: 'info',
                        file: 'C:/Users/drowa/.node-red/nodered.log',
                        flushInterval: 10 }
        }

    Restart Node-RED after the change. With flushInterval = 10 the
    file is written every 10 lines, so progress is live-tail-able
    via `Get-Content -Wait`. Add log rotation if the file grows
    unbounded (built-in support: maxFiles + maxSize keys).

    Update CLAUDE.md "Diagnostic process" section to point at the
    new log path so future sessions know to check it.

    PRIORITY: Low — operational quality-of-life. Action when convenient.
    STATUS: RESOLVED 2026-04-27 (Tier-1 T1.1 per ROADMAP). Node-RED
    file logging configured + log rotation via PowerShell scheduled
    task. Index moved to RESOLVED 2026-04-28.

53. Per-heartbeat LOG_HEAP for fleet-wide leak surveillance
    OBSERVATION (2026-04-26 audit):  Existing LOG_HEAP markers fire
    at boot phases (after-serial, after-wifi, after-mqtt, after-ble)
    but never during steady-state operation. Heap fragmentation /
    leak hypotheses (#51 hypothesis #4) cannot be confirmed or
    ruled out without runtime sampling.

    PROPOSED FIX:  In include/mqtt_client.h, extend the heartbeat
    publisher to include `heap_free` and `heap_largest` fields in
    the JSON status payload. Sample is taken just before publish,
    every HEARTBEAT_INTERVAL_MS (60 s). One-line change in the
    payload builder. No new MQTT topic — reuses /status.

    DOWNSTREAM:  Add a Node-RED dashboard tile that plots the
    rolling 24 h heap-free trajectory per device. Slow downward
    trend over hours = leak. Sudden drop + recovery = transient
    pressure (e.g. TLS handshake spike). Stable = healthy.

    BENEFIT:  Catches future leaks BEFORE they cause a crash.
    Also gives a lower-bound baseline for v0.4.10.1 stability
    confirmation: if Alpha runs 24 h with flat heap, hypothesis #4
    is conclusively ruled out.

    PRIORITY: Medium. Cheap implementation; high diagnostic value.
              Bundle with v0.4.11 release if Phase A passes.
    STATUS (v0.4.11): Firmware part SHIPPED — heap_free and heap_largest
              included in every /status heartbeat payload. Node-RED
              dashboard tile (DOWNSTREAM above) still pending; planned
              for v0.4.15.
    STATUS: RESOLVED 2026-04-28 — primary firmware mechanism shipped
    months ago and used productively throughout the v0.4.13-v0.4.20
    cascade-fix marathon (heap_free trajectory observed per device).
    The Node-RED dashboard tile is a UI-polish followup, tracked
    separately if desired. Index moved to RESOLVED 2026-04-28.

54. Stack-canary build (CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2)
    OBSERVATION (2026-04-26 audit):  arduino-esp32 default has stack
    overflow detection OFF. Hypothesis #5 in #51 (stack overflow in
    ws2812 task at 4 KB) cannot be detected at runtime without
    rebuilding the IDF base.

    PROPOSED FIX:  Provide an alternate platformio.ini env
    [env:esp32dev_canary] that adds:

        build_flags = -DCONFIG_FREERTOS_CHECK_STACKOVERFLOW=2

    This is the "canary" mode (writes a known pattern at the bottom
    of every stack and checks it at every context switch). Cost is
    ~+8 ms per task switch on average — fine for a single soak
    device, not for production fleet.

    USAGE:  Build the canary env, USB-flash to ONE device, soak for
    7 days. If any task overflows its stack, the firmware halts
    with a serial print of the offending task name and the stack
    high-water mark. We then bump that task's stack and ship the
    fix without canary in production.

    PRIORITY: Low. Diagnostic only. Schedule alongside the v0.4.13
              tag if no other stack-related issues surface by then.

55. AsyncMqttClient malformed-packet counter
    OBSERVATION (2026-04-25 mosquitto log review):  Mosquitto recorded
    2 'malformed packet' disconnect events in 2 days — Charlie at
    16:02:48 and Delta at 18:07:02. Rare but non-zero signal.
    Possible causes: transient memory corruption mid-publish, bug
    in AsyncMqttClient framing, bit-flip on the wire.

    PROPOSED FIX:  Add a counter `_mqttMalformedCount` to
    mqtt_client.h, incremented from the AsyncMqttClient onError
    callback (or onDisconnect with reason indicating protocol
    error). Surface in heartbeat status JSON alongside #53's heap
    fields. Operators can spot a device drifting from "0" to "1+"
    over time without log archaeology.

    BENEFIT:  Surfaces a class of low-frequency-but-high-importance
    failures that are currently invisible.

    PRIORITY: Low. Bundle with #53 work — same heartbeat payload
              extension.

56. Re-implement MQTT_HEALTHY safely via deferred-flag pattern
    OBSERVATION (2026-04-26 audit + #51 Phase A in progress):  The
    v0.4.10 LED hooks posted ws2812 events directly from
    onMqttConnect/onMqttDisconnect, both of which run on the
    TWDT-subscribed async_tcp task. Suspected cause of #51.
    v0.4.10.1 disables the posts; the LED state machine itself is
    fine.

    PROPOSED FIX (v0.4.12):  Re-introduce the visual feedback
    (operator-facing green-when-healthy + revert-on-disconnect)
    using the same deferred-flag pattern already proven in the
    sleep / restart paths (search mqtt_client.h for `_mqttSleepAtMs`
    and `_mqttRestartAtMs`):

        // In callback (async_tcp ctx):
        _mqttLedHealthyAtMs = millis();   // single atomic store

        // In loop() (loopTask ctx, not WDT-subscribed in normal ops):
        if (_mqttLedHealthyAtMs && millis() >= _mqttLedHealthyAtMs) {
            LedEvent e{}; e.type = LedEventType::MQTT_HEALTHY;
            ws2812PostEvent(e);
            _mqttLedHealthyAtMs = 0;
        }

    The flag is a single 32-bit volatile store (atomic on Xtensa).
    The actual ws2812PostEvent + struct copy happen on loopTask
    where we have full FreeRTOS budget. No queue-fill risk during
    rapid reconnect cycles either: the flag is overwritten, not
    queued.

    DOCUMENTATION:  Add the pattern to TWDT_POLICY.md as the
    canonical "callback wants to do work" template. Update #44 and
    #51 with the resolution.

    DEPENDENCY:  Phase A of #51 must pass first to confirm the
    revert is sufficient. Then v0.4.11 ships the revert; v0.4.12
    re-enables this safely.

    PRIORITY: Medium-High. Restores the operator-visible green-MQTT
              indicator without the WDT risk. Small change with
              high UX value.
    STATUS:   RESOLVED 2026-04-27 in v0.4.13. Verified in
              include/mqtt_client.h: `_mqttLedHealthyAtMs` set in
              onMqttConnect() callback (line 1179) and consumed in
              mqttHeartbeat() (line 1597) on loopTask.

57. Install host gcc/g++ to enable native unit tests
    OBSERVATION (2026-04-25 + 2026-04-26):  CLAUDE.md says
    "Host-side unit tests: pio test -e native -v ← run before
    every commit". Reality: `pio test -e native` fails with
    "'gcc' is not recognized as an internal or external command"
    because no host C/C++ compiler is installed on this Windows
    workstation. The xtensa-esp32-elf cross-compiler bundled with
    PlatformIO works for ESP32 builds, but the native env needs
    a host gcc.

    IMPACT:  Unit tests in test/test_native/ have never run on
    this machine. Code that those tests would catch (NVS namespace
    drift, sector-trailer guard math, ranging math, hex helpers)
    is going out untested. We've shipped without crashes attributable
    to those subsystems, but the safety net is missing.

    PROPOSED FIX:  Install a Windows-native gcc. Two cleanest
    options:

       1. winget install -e --id MartinStorsjo.LLVM-MinGW.UCRT
          (LLVM-based, ~150 MB, modern, single command).
       2. Download WinLibs MinGW-w64 zip from https://winlibs.com/
          and unzip into C:\mingw64\, then add C:\mingw64\bin to
          system PATH. Lower-tech but well-tested.

    Verify: open a fresh shell, run `gcc --version`. Then from
    esp32_node_firmware/: `pio test -e native -v` should compile
    and run the test suite (~30 s).

    DOWNSTREAM:  Once gcc works, enforce the CLAUDE.md rule via
    a pre-commit git hook that runs `pio test -e native -v` and
    blocks the commit on failure. Documented in
    docs/TOOLING_INTEGRATION_PLAN.md — fits Tier 1 / T1.5
    (PlatformIO standardisation).

    PRIORITY: Low. Operational quality-of-life. No active blocker
              today. Address when convenient — most likely
              alongside the next batch of Tier 1 tooling work.
    STATUS:   RESOLVED 2026-04-27. MinGW-w64 (UCRT, gcc 15.2.0) is
              installed at C:\mingw64\bin. Verified with
              `PATH=/c/mingw64/bin:$PATH pio test -e native -v` from
              esp32_node_firmware/: 105 test cases passed in 3.09 s.
              Pre-commit hook (downstream item) still pending — file
              separately if/when wanted.

────────────────────────────────────────────────────────────────────────────────
Setup gaps — 2026-04-26 full-stack audit
(Claude / GitHub / PlatformIO / Mosquitto / Node-RED / repo hygiene)
────────────────────────────────────────────────────────────────────────────────

Phase A — Fix confirmed breakage

58. Fix daily_health_config.json local_clone path                (2026-04-26 audit)
    WHERE:    C:\Users\drowa\tools\daily_health_config.json  →  "local_clone" key
    PROBLEM:  Path set to C:\Users\drowa\git\esp32_node_firmware — that directory
              does not exist. The canonical clone is at
              C:\Users\drowa\Documents\git\Arduino\NodeFirmware\esp32_node_firmware.
              File-system health checks in daily_health_check.py fail silently.
    FIX:      Update the value to
              C:\Users\drowa\Documents\git\Arduino\NodeFirmware\esp32_node_firmware
    VERIFY:   python C:\Users\drowa\tools\daily_health_check.py — confirm no
              "path not found" warnings in the report.
    PRIORITY: High. Silent failure in the daily health baseline.
    STATUS:   RESOLVED 2026-04-27. local_clone now points at the repo root
              (NOT the firmware subdir) so the .git presence check passes.
              daily_health_check.py reports "Local firmware clone: present
              on branch master".

59. Root .gitignore (repo root is currently unprotected)         (2026-04-26 audit)
    WHERE:    C:\Users\drowa\Documents\git\Arduino\NodeFirmware\.gitignore  (create)
    PROBLEM:  No .gitignore at the repo root. The firmware subfolder may have
              its own, but top-level dirs like Logs/, ble_dashboard_flow.json,
              ble_track_inject.json, and .claude/scheduled_tasks.lock are
              already showing as untracked noise in git status.
    FIX:      Create root-level .gitignore covering:
                  .pio/
                  *.bin  *.hex  *.elf
                  .DS_Store  Thumbs.db
                  Logs/
                  *.lock
                  __pycache__/  *.pyc
    PRIORITY: Medium. Keeps git status clean; prevents accidental binary commits.
    STATUS:   RESOLVED 2026-04-27. Created at repo root with the patterns
              above (substituted .claude/scheduled_tasks.lock for *.lock so
              poetry/package locks aren't accidentally swept up).

60. Root .gitattributes (CRLF normalisation)                     (2026-04-26 audit)
    WHERE:    C:\Users\drowa\Documents\git\Arduino\NodeFirmware\.gitattributes (create)
    PROBLEM:  CI runs on Ubuntu; dev machine is Windows. Without an explicit
              line-ending policy, mixed CRLF/LF commits accumulate and produce
              spurious diffs.
    FIX:      Minimal root .gitattributes:
                  * text=auto eol=lf
                  *.bin  binary
                  *.hex  binary
                  *.elf  binary
                  *.png  binary
    PRIORITY: Medium. Prevents CI/local diff noise on every text file.
    STATUS:   RESOLVED 2026-04-27. Created at repo root with the patterns
              above plus *.jpg as binary.

Phase B — Security & identity

61. Mosquitto: add auth (passwd + ACL)                           (2026-04-26 audit)
    DECISION: Intentionally deferred — no auth during active dev work.
              Open broker on private LAN is acceptable for now.
    REVISIT:  Before any of: guest network access, remote access, or moving
              beyond the dev phase. When ready:
      1. Copy mosquitto.conf.new → mosquitto.conf, uncomment:
             allow_anonymous false
             password_file C:\ProgramData\mosquitto\passwd
             acl_file C:\ProgramData\mosquitto\acl
      2. Create passwd:  mosquitto_passwd -c passwd esp32_fleet
                         mosquitto_passwd passwd nodered
                         mosquitto_passwd passwd claude_mcp
      3. Create ACL restricting the fleet to Enigma/JHBDev/Office/# only.
      4. Restart service; verify all 6 nodes still publish within 30 s.
      5. Update async-mqtt credentials in include/config.h + Node-RED MQTT nodes.
    PRIORITY: Deferred. Re-evaluate at end of dev phase.

62. LICENSE file                                                  (addressed 2026-04-26)
    DECISION: All rights reserved. LICENSE file created at repo root.
              Copyright (c) 2026 dj803. No open-source licence at this time.

Phase C — CI completeness

63. Add trufflehog secrets-scan job to build.yml                 (2026-04-26 audit)
    WHERE:    .github/workflows/build.yml
    PROBLEM:  docs/SUGGESTED_IMPROVEMENTS references §11.3 (secrets scanning)
              and TOOLING_INTEGRATION_PLAN.md lists trufflehog as a required
              CI job — but build.yml only has compile + host-test + release jobs.
              No secrets scan has ever run on any commit.
    FIX:      Add a job after the build job:
                  secrets-scan:
                    runs-on: ubuntu-latest
                    steps:
                      - uses: actions/checkout@v4 --fetch-depth 0
                      - uses: trufflesecurity/trufflehog@main
                        with:
                          path: ./
                          base: ${{ github.event.repository.default_branch }}
    PRIORITY: Medium. Blocks secrets-in-commit incidents; already spec'd.

64. Root README.md                                               (2026-04-26 audit)
    WHERE:    C:\Users\drowa\Documents\git\Arduino\NodeFirmware\README.md  (create)
    PROBLEM:  Cloning the repo shows a blank GitHub landing. No orientation.
    FIX:      Minimal README: one-paragraph description, link to CLAUDE.md,
              link to esp32_node_firmware/docs/. No need to duplicate content.
    PRIORITY: Low. Cosmetic, no operational impact.
    STATUS:   RESOLVED 2026-04-27. Created at repo root with one-paragraph
              description, layout, and pointers to CLAUDE.md / docs / OTA
              manifest URL.

Phase D — Developer experience

65. esp32_node_firmware/include/build_config.h stub              (2026-04-26 audit)
    WHERE:    esp32_node_firmware/include/build_config.h  (create)
    PROBLEM:  docs/TECHNICAL_SPEC.md §10.1 references build_config.h as the
              home for build-time flags. The file does not exist. Any code
              or doc that tries to include it will fail to compile.
    FIX:      Create a minimal stub:
                  #pragma once
                  // Build-time feature flags. Set via platformio.ini build_flags.
                  // #define ENABLE_SERIAL_DEBUG
                  // #define SKIP_BOOTSTRAP_WAIT
              Document in platformio.ini comment that flags go here.
    PRIORITY: Low. No active include errors today, but resolves the dangling
              spec reference before Phase 1 feature work references it.
    STATUS:   RESOLVED 2026-04-27. Created at esp32_node_firmware/include/
              build_config.h with the stub above. esp32dev build verified
              clean.

66. .claude/commands/ — operational shortcuts                    (2026-04-26 audit)
    WHERE:    C:\Users\drowa\.claude\commands\  (create dir + 3 files)
    PROBLEM:  .claude/commands/ directory does not exist. Repeated multi-step
              workflows (compile+test, fleet snapshot, fleet OTA status) have
              no single-keystroke entry point. daily-health.md exists as a
              skill but is invoked via a different mechanism.
    FIX:      Create three command files:
                  pio-build.md   — pio test -e native -v && pio run -e esp32dev
                  mqtt-sub.md    — mosquitto_sub snapshot of fleet status topics (-W 5)
                  fleet-status.md — run tools/fleet_status.sh and summarise
    PRIORITY: Low. Quality-of-life; saves typing on daily dev tasks.
    STATUS:   RESOLVED 2026-04-27. Created C:\Users\drowa\.claude\commands\
              files: pio-build.md, mqtt-sub.md, fleet-status.md.
              They appear in the available-skills list and can be invoked
              via `/pio-build`, `/mqtt-sub`, `/fleet-status`. Scope is
              user-level so they live outside the repo.

68. Node-RED: enable adminAuth in settings.js                    (2026-04-26 audit)
    DECISION: Intentionally deferred — no auth during active dev work.
              Admin API is localhost-only (127.0.0.1:1880); acceptable for now.
              CLAUDE.md documents this explicitly.
    REVISIT:  Before any of: exposing Node-RED on the LAN/WAN, adding other
              users, or moving beyond the dev phase. When ready:
        In C:\Users\drowa\.node-red\settings.js, uncomment and fill in:
            adminAuth: {
                type: "credentials",
                users: [{ username: "admin", password: "<bcrypt hash>",
                          permissions: "*" }]
            }
        Generate hash: node-red admin hash-pw
    PRIORITY: Deferred. Re-evaluate at end of dev phase.

67. Node-RED project package.json — declare custom node deps     (2026-04-26 audit)
    WHERE:    C:\Users\drowa\.node-red\projects\esp32-node-firmware\package.json
    PROBLEM:  package.json lists only basic metadata (name, description,
              version 0.0.1). All custom nodes (Dashboard 2.0, MQTT nodes, etc.)
              are installed globally in Node-RED but not declared as project deps.
              Setting up on a second machine requires manually identifying which
              nodes the flows use.
    FIX:      Add a "dependencies" block listing all @flowfuse/node-red-dashboard
              and other palette nodes the flows.json references. Can be extracted
              by scanning flows.json for "type" fields prefixed with custom module
              names.
    PRIORITY: Low. Only matters at second-machine setup time.

# ── #51 UPDATE 2026-04-26 mid-day — Phase A INCOMPLETE ─────────────────
Phase A's hypothesis (LED hooks alone cause the crashes) is REFUTED:

  10:14  Foxtrot  int_wdt   v0.4.11-dev  (had hooks — fits original hypothesis)
  10:18  Alpha    HUNG      v0.4.10.1-dev (silent deadlock, no boot_reason — NEW failure mode)
  10:29  Delta    int_wdt   v0.4.10-dev  (had hooks)
  10:29  Bravo    int_wdt   v0.4.10-dev  (had hooks)
  10:29  Charlie  int_wdt   v0.4.10.1-dev (HOOKS REVERTED — refutes hooks-only theory)

Two separate failure modes now in scope:

  (a) int_wdt cascade across multiple devices simultaneously, regardless
      of firmware version. Strongest theory: peer-broadcast storm via
      ESP-NOW from a misbehaving transmitter (Bravo top suspect — 136
      mosquitto disconnects in 2 days, predates v0.4.10, chronic flake
      per #46). Bravo currently powered off; ~1 h watch in progress.

  (b) Silent FreeRTOS deadlock on Alpha after ~70 min uptime. RTC WDT
      did not recover the chip. WS2812 frozen mid-frame, GPIO 2 LED
      off. Single observation; needs reproduction. Worst-case path:
      both cores blocked + watchdog silenced.

Decision: do NOT cut v0.4.11 release yet. Working-tree firmware
(revert + NDEF + heap + visibility) flashed to Alpha + Foxtrot as
live observers; Charlie still on the v0.4.10.1-dev test bed.

Next experiments documented in ESP32_FAILURE_MODES.md "2026-04-26
mid-day update" section.

71. Per-device feature-subset firmware variants
    OBSERVATION (operator suggestion 2026-04-26):  Devices in the
    fleet have different physical hardware + roles. Foxtrot has an
    RFID reader; Alpha has a WS2812 strip; not every device needs
    BLE; not every device needs ESP-NOW ranging. Currently all
    devices flash the SAME firmware.bin which includes every
    optional subsystem regardless of hardware. Flash is at 93.3%
    (down to 82.1% with BLE disabled per #51 diagnostic).

    PROPOSAL:  Build multiple firmware variants from the same source
    tree, each compiled with a different set of feature flags
    (BLE_ENABLED, RFID_ENABLED, ESPNOW_RANGING_ENABLED, etc.). The
    OTA manifest grows from one URL to N URLs keyed by variant
    string. Each device announces its variant in the credential
    bundle / boot status; OTA picks the matching firmware.bin.

    PROS:
       - Smaller binaries per role (estimated):
            "minimal" (no BLE, no RFID, no ranging): ~1.3 MB → ~600 KB headroom
            "rfid-only": ~1.5 MB
            "ranging-only": ~1.4 MB
         Enables features blocked by the 1966080-byte app partition
         (NTAG424 DNA #12, recovery partition #26).
       - Lower attack surface per device. Subsystems compile out.
       - Faster boot (fewer init paths).
       - Easier #51-style diagnosis: deploy a feature-stripped
         variant to a subset of the fleet to isolate which
         subsystem is misbehaving.
       - IRAM/DRAM headroom — NimBLE pulls IRAM from a tight
         budget (currently 98% with sleep driver, 128 KB base + 2 KB
         extension per memory.ld). Variants without BLE recover
         that IRAM for other features.

    CONS:
       - CI build pipeline complexity 1 → N variants per release.
         Build time multiplies; gh-pages storage grows.
       - OTA manifest schema must change (today is single
         {Version,URL}; new is per-variant array). Backwards-compat
         needs care so existing devices don't break on the next
         manifest fetch.
       - Wrong variant flashed to wrong device = device boots
         without the hardware-driving code it needs. Mitigation:
         capability negotiation in bootstrap, but adds round-trip.
       - Credential bundle / bootstrap must carry variant string;
         requires ESPNOW_PROTOCOL_VERSION bump.
       - Documentation churn: every feature doc gets a "variant
         availability" badge. Roster doc tracks per-device variant.

    DESIGN SKETCH:
       1. platformio.ini envs: [env:esp32dev_full],
          [env:esp32dev_rfid], [env:esp32dev_minimal], etc.
          Each env overrides flags via build_flags.
       2. CI builds all envs on tag; uploads each binary to
          gh-pages as firmware-<variant>-<version>.bin.
       3. OTA manifest:
            { "Configurations": [
                { "Variant": "full",    "Version": "0.5.0", "URL": "..." },
                { "Variant": "rfid",    "Version": "0.5.0", "URL": "..." },
                { "Variant": "minimal", "Version": "0.5.0", "URL": "..." }
            ] }
       4. Firmware bakes BUILD_VARIANT define at compile time;
          OTA check picks matching entry.
       5. CLAUDE.md fleet-roster table: device → variant.
       6. Optional NVS override `cmd/ota/variant` for testing.

    PRIORITY: Medium-High once #51 stability is resolved. Flash-
              savings ROI alone justifies the CI work. Pairs
              naturally with v0.5.0 relay+Hall hardware
              introduction — that's when the fleet starts having
              genuinely different hardware roles.

    NEAR-TERM SUBSTITUTE:  The per-device feature flag flip we
    just did for #51 (BLE off via single config.h #define comment)
    is enough until the variant infrastructure lands. If a device
    needs BLE back, USB-flash that one device with an alternate
    build. Manual but cheap.

    PARTIAL PROGRESS 2026-04-27:  Added [env:esp32dev_minimal] to
    platformio.ini extending esp32dev with -URFID_ENABLED. Built
    clean, USB-flashed to Bravo, boot announce clean. HOWEVER
    `rfid_enabled` still reported `true` in the heartbeat —
    config.h's unconditional `#define RFID_ENABLED` shadowed the
    `-U` from the env. Fixed by:
       1. config.h: gated the define with `#ifndef RFID_DISABLED`.
       2. env: switched from `-URFID_ENABLED` to `-DRFID_DISABLED`.
       3. mqtt_client.h: wrapped handleRfidWhitelist + its call
          site in `#ifdef RFID_ENABLED` so the link doesn't fail
          when the function bodies (in rfid.h) are compiled out.

    Re-flashed to Bravo. Heartbeat now reports `rfid_enabled: false`.
    Validated via M1 (5 s blip) + M2 (30 s blip) chaos tests
    2026-04-27 ~21:53-21:55: all 6 fleet devices (including Bravo
    on the minimal variant + Charlie on the canary build)
    reconnected via `event=online` / heartbeat, uptime preserved,
    no panics, no reboots, no abnormal boots. The v0.4.16 broker-
    probe path (#78 mitigation) handled both blips cleanly across
    full + minimal + canary variants of the same source tree.

    SCOPE: only the minimal variant is shipped. The full per-device
    variant story from this entry's PROPOSAL section (rfid-only
    variant, ranging-only variant, esp32dev_full) requires:
       - ESPNOW_RANGING_ENABLED guard wrapping main.cpp's responder
         + ranging publish + bootstrap-ranging hooks.
       - BLE_ENABLED gating same pattern as RFID_ENABLED above
         (config.h `#ifndef BLE_DISABLED` then env `-DBLE_DISABLED`).
       - OTA manifest extension to per-variant {Variant, Version, URL}
         entries + firmware-side variant-aware fetch.
       - CI build pipeline build all variants on tag, upload to
         gh-pages as firmware-<variant>-<version>.bin.
    Defer until v0.5.0 hardware-divergent fleet introduction.

    Real flash savings on the current minimal variant: ~negligible
    in this build because RFID code is small and the optimizer
    inlined most of it. The savings hypothesis from the original
    proposal applies more strongly to BLE (~30 KB) and ranging
    (~unknown). Worth re-measuring once those guards land.

72. Bench-supply voltage stress testing rig
    OBSERVATION (2026-04-26 operator suggestion):  Failure modes #51
    include `brownout` and `other_wdt` as candidates. Today we
    can't easily distinguish "firmware bug" from "marginal Vcc"
    when a device crashes. A bench power supply lets us drive
    one ESP32 at controlled voltage and reproduce the failure
    mode deterministically.

    PROPOSED RIG:
       - Bench supply set to 5.0 V (nominal USB), 4.5 V (cable
         resistance / weak hub), 3.7 V (battery sag near cutoff),
         5.5 V (USB-C high-side spec), and 6.0 V (regulator stress).
       - Connect to VIN of a dev-board ESP32 (the on-board AMS1117
         drops to 3.3 V).
       - Or skip the regulator and feed 3.3 V directly into the
         3V3 pin to test the chip's own brownout threshold (~2.43 V
         per ESP32 datasheet).
       - Optional: add a programmable load on a GPIO to simulate
         RFID coil / WS2812 strip current spikes during the test.

    TEST MATRIX:
       - Vcc sweep down from 5 V to 3 V in 100 mV steps; log boot
         success / fail / brownout boot reason at each.
       - Hold at 4.0 V (worst-case brownout-prone) for 4 hours;
         compare crash rate to nominal 5 V.
       - Compare: device with WS2812 strip drawing 500 mA peak vs.
         no strip — does Vcc dip during peak cause crashes?

    DIAGNOSTIC VALUE:
       - Deterministic reproduction of brownout-class failures.
       - Separates firmware bugs from hardware/power issues — the
         Alpha int_wdt + silent-deadlock pattern can be re-tested
         at controlled Vcc to rule in/out brownout.
       - Validates the brownout detector threshold (config.h
         doesn't set one; IDF default is ~2.43 V).
       - Catches regulator inadequacy (AMS1117 thermal cutoff at
         ~150 °C, sustained 600+ mA loads can push it).

    DATA TO CAPTURE:
       - Vcc reading at the chip (multimeter or oscilloscope on
         the 3V3 pin during peak load).
       - Boot reasons over time at each voltage.
       - Heap free at boot (now in heartbeat per #53).
       - Wi-Fi RSSI (proxies for radio-side power demand).

    PRIORITY: Medium. Worth doing once the immediate #51 cascade is
              understood — then bench-supply rig characterises the
              fleet's brownout margin properly. Could also feed
              into v0.5.0 hardware design (relay + Hall on the
              same board) where current draw will be higher.

# ── #51 / #71 follow-up — 2×2 BLE × ESP-NOW build matrix ────────────
Once #51 settles, build a follow-up A/B with three additional firmware
variants to isolate which subsystem contributes to which failure mode:

   1. BLE on,  ESP-NOW on   (current default — baseline failure mode)
   2. BLE off, ESP-NOW on   (just flashed — current no-BLE build, stable so far)
   3. BLE on,  ESP-NOW off  (NEW variant to build) — implementation: comment
                              ESPNOW_BEACON_INTERVAL_MS init / disable
                              espnowResponderStart() in main.cpp
   4. BLE off, ESP-NOW off  (minimum-baseline sanity)

Flash variant 3 to one device alongside the existing test bed:
   - If variant 3 stays stable → ESP-NOW (or BLE+ESP-NOW interaction) is
     the regression source, not BLE alone.
   - If variant 3 crashes like the default → BLE alone is sufficient to
     trigger the failure mode.

Also useful for IRAM characterisation:
   - Each variant's `.iram0.text` size tells us BLE vs ESP-NOW IRAM cost
     independently. Currently we know BLE = ~30 KB IRAM. ESP-NOW alone is
     unknown.

Pairs naturally with #71 per-device variants once that infrastructure lands.


73. Silent-failure watcher (catches deadlocks that don't reboot)
    OBSERVATION (2026-04-26):  Today's boot_history watcher fires on
    NEW abnormal-boot entries — i.e., devices that crashed AND
    rebooted. Today's silent-deadlock failure mode (Alpha, Echo,
    Delta) produces NO boot event because the chip stays powered
    but FreeRTOS hangs. We only noticed because operator eyeballed
    the LEDs. No automated alert fired.

    PROPOSED FIX (cheap):  Run a Monitor task that subscribes to
    each device's /status topic and alerts when:
       (a) The retained value flips to {"online":false,"event":"offline"}
           — Mosquitto publishes this LWT after MQTT keepalive
           timeout (~15-30 s).
       (b) Or: poll Node-RED's hb_devices flow context every 60 s;
           alert if any device's lastSeen timestamp is >3 min old.

    Either approach catches silent deadlocks within ~1-3 minutes,
    not "next time the operator looks at the bench".

    PROPOSED FIX (richer):  Combine LWT-based + heartbeat-staleness
    + boot_history-new-entry into one unified watcher. Output one
    line per event regardless of failure mode:
        [time] DEVICE down — reason: timeout|crash|hang|brownout

    DOWNSTREAM:  Add a Node-RED dashboard tile "Silent failures"
    that lists devices in the offline-but-not-rebooted state.
    Adjacent to the existing boot_history tile. Operators see the
    full picture in one glance.

    PRIORITY: HIGH for diagnostic sessions like today's; Medium for
              steady-state operations. Cheap to implement (~30 lines
              of Python in tools/silent_watcher.sh).
    STATUS:   RESOLVED 2026-04-27. tools/silent_watcher.sh exists and
              CLAUDE.md "Monitoring sessions" section documents it as
              the standard Monitor task to arm during stability work.

# ── #51 ROOT CAUSE 2026-04-27 ─────────────────────────────────────
The 24-hour diagnostic chase culminated in a captured serial panic
backtrace from Charlie at 02:44 SAST during a network-reconnect cascade:

   abort() called at PC 0x401ad3f7 on Core 1
   Decoded backtrace:
     panic_abort
       abort()
         std::terminate()
           __cxa_throw                  ← bad_alloc thrown
             operator new(unsigned int) ← heap allocation FAILED
               AsyncMqttClient::publish (AsyncMqttClient.cpp:742)
                 mqttPublish (mqtt_client.h:180)
                   espnowRangingLoop (mqtt_client.h:339)
                     loop (main.cpp:703)

   Last log line before panic:
   "[I][Responder] BROKER_RESP → sibling: 192.168.10.30:1883"

INTERPRETATION:
  AsyncMqttClient::publish() internally `new`s a contiguous buffer
  for the framed MQTT message. Under network-reconnect storms the
  heap fragments — `free` heap stays healthy (~117k observed) but
  the LARGEST contiguous block drops below what the publish needs.
  operator new throws std::bad_alloc. arduino-esp32 doesn't have
  full C++ exception support → throw escalates to std::terminate()
  → abort() → boot_reason=panic. The trigger is the per-3s ESP-NOW
  ranging publish hitting the fragmented heap right after a network
  blip.

EXPLAINS BOTH OBSERVED CASCADES:
  - 2026-04-26 10:34 (Bravo+Delta+Charlie int_wdt within 22s).
    Charlie was on hooks-reverted v0.4.10.1 yet still crashed →
    not LED-hooks. Now we know it was bad_alloc in stress.
  - 2026-04-27 02:43-44 (Delta+Charlie+Alpha panic within 2 min).
    Pure repro after a broker blip with no operator activity.

FIX SHIPPED v0.4.11-dev (2026-04-27):
  In include/mqtt_client.h::mqttPublish(), pre-check
  `ESP.getMaxAllocHeap() >= MQTT_PUBLISH_HEAP_MIN` (4096) before
  calling _mqttClient.publish(). On under-threshold, log a rate-
  limited WARN and drop the publish. Next publish (after queue
  drains and heap defragments) succeeds. Drop is acceptable
  because all our publishes are either retained (Node-RED replays)
  or cosmetic ranging telemetry where one missed message is
  harmless.

UPDATED FAILURE-MODE LANDSCAPE:
  (a) Network-stress bad_alloc panic — FIXED in v0.4.11-dev by the
      heap-guard above. Affects all devices regardless of BLE state.
  (b) BLE silent deadlock — separate failure; with-BLE devices hang
      ~70 min after reconnect events. WORKAROUND: BLE disabled.
      Real fix needs NimBLE/ESP-NOW/WiFi coexistence audit.
  (c) Charlie chronic flake — likely cable/port (brownout 11:50).
      Hardware-side, not firmware.

DEEPER FIX OPPORTUNITY (future):
  AsyncMqttClient could be replaced with PubSubClient or a static-
  buffer-based MQTT client that doesn't `new` per publish. Larger
  refactor; defer until v0.5.x.

61. Misleading "boot" event on every MQTT reconnect
    STATUS: RESOLVED 2026-04-27 in v0.4.13. Added FwEvent::ONLINE
    (enum value 18, string "online"). Static bool _firstMqttConnect
    in mqtt_client.h tracks first-connect vs reconnect: BOOT on
    first (retained, includes boot_reason); ONLINE on every
    subsequent reconnect (non-retained, no boot_reason). Node-RED
    boot_history filter `p.event !== 'boot'` already discards
    non-boot events so no flow change required.
    OBSERVATION (2026-04-27 02:34 cascade):  When MQTT reconnects
    after a transient disconnect, onMqttConnect() calls
    mqttPublishStatus(FwEvent::BOOT) which republishes the retained
    boot announcement. Payload has event="boot" but uptime_s=current
    (not 2) and boot_reason=original first-boot reason. Operators /
    dashboards seeing event=boot expect a fresh reboot — have to
    look at uptime_s to disambiguate.

    PROPOSED FIX (Option 1, cleanest):  Replace reconnect re-announce
    with event="reconnect" or event="online". Retained still goes
    out; event field is honest. Search-replace at the single
    onMqttConnect call site. ~10 lines + dashboard tile update.

    PRIORITY: Low. Misleading but not buggy. Bundle with next
              mqtt_client.h refactor.

# ── #51 SOAK RESULT 2026-04-27 morning ───────────────────────────
8-hour overnight soak after the heap-guard fix was flashed to 3 of 5
active devices (Charlie / Echo / Foxtrot via USB, with v0.4.11-dev +
mqttPublish heap-guard). Alpha + Delta remained on the older
v0.4.11-dev WITHOUT the guard.

Results:
   * 3 FIXED devices: zero panics, zero abnormal boots in 8 h.
   * 1 UNFIXED device (Alpha): panic at 01:49:14 (isolated, no
     cascade), watchdog auto-recovered.
   * 1 UNFIXED device (Delta): zero panics — got lucky with heap
     fragmentation profile.

Conclusions:
   1. Heap-guard fix WORKS. Devices with the fix did not panic
      under normal operation despite Alpha proving the bug is
      still firing on the network.
   2. The bug fires stochastically. Not every blip → not every
      device crashes. Heap fragmentation has to be in just the
      right state at the moment of the publish.
   3. No cascade in 8 h — the dangerous mass-failure mode from
      the morning of 04-26 didn't recur. That's the most
      important behavioural change.

Next steps (operator decision):
   - Cut v0.4.11 release tag — fix + NDEF + visibility + heap
     heartbeat + BLE-off all bundled.
   - Flash Alpha + Delta with the fix (OTA after release, or
     USB swap).
   - Plan v0.4.12 to address (b) BLE silent deadlock — out of
     scope for this fix.



────────────────────────────────────────────────────────────
74. IPv6Address.h support — unblock newer AsyncTCP forks
    OBSERVATION (2026-04-27): When swapping to esphome/AsyncTCP
    for the v0.4.14 fix, the latest release (v2.1.4) failed to
    compile with:
       fatal error: IPv6Address.h: No such file or directory
    arduino-esp32 framework 3.3.8 (used here) does not ship
    a separate IPv6Address.h — IPv6 is folded into IPAddress.
    Pinned to v2.0.1 (pre-IPv6 commit) as a workaround.

    PROPOSAL:
       Either upgrade the arduino-esp32 framework to a version
       that ships IPv6Address.h, OR shim a stub header that
       re-exports IPAddress as IPv6Address. Unblocks adoption of
       newer hardened AsyncTCP releases as they ship.

    SEVERITY: LOW. v2.0.1 is sufficient for the v0.4.14 fix and
    the LAN is IPv4-only today.
    STATUS: RESOLVED 2026-04-28 — moot. The v0.4.14 cascade-fix
    settled on mathieucarbou/AsyncTCP v3.3.2, which conditionally
    skips IPv6Address.h when ESP_IDF_VERSION_MAJOR >= 5 (arduino-esp32
    3.3.8 is on IDF 5.x). No shim ever needed. esphome v2.1.4 was
    rejected for unrelated tcp_alloc lock-assert behaviour. Re-open
    only if a future fork explicitly requires IPv6Address.h on IDF
    pre-5.x. Index moved to RESOLVED.

────────────────────────────────────────────────────────────
75. Chaos-testing framework — promote tools/chaos/ + site_acceptance.sh
    OBSERVATION (2026-04-27): The v0.4.13 panic cascade was only
    diagnosable because we manually arranged a synthetic broker
    blip with a no-DTR serial monitor armed on Charlie (COM5).
    Without that scaffolding the bug would have remained silent
    (no boot_reason captured, no backtrace).

    docs/CHAOS_TESTING.md captures the test plan. To make it a
    real framework:
       - tools/chaos/blip_short.ps1, blip_long.ps1, blip_burst.ps1
       - tools/chaos/wifi_cycle.ps1
       - tools/chaos/espnow_toggle.sh
       - tools/chaos/ble_toggle.sh
       - tools/chaos/runner.sh — orchestrates: arm watchers, run
         scenario, collect events for N seconds, decide pass/fail,
         write JSON report.
       - tools/site_acceptance.sh — runs the deployment-time
         subset before flipping a site to live.
       - CI hook: pre-release runs M1-M4 + EN1 against a smoke
         fleet (one device).

    SEVERITY: MEDIUM. Manual today; risks regression of v0.4.13
    class bugs unless promoted to automated framework.


────────────────────────────────────────────────────────────
76. Recovery + reporting hardening — restart policy redesign
    OBSERVATION (2026-04-27 chaos tests M2 ×2): When a 30-second
    broker outage triggers reconnect-storm, the firmware's
    "MQTT_RESTART_THRESHOLD = 10 → ESP.restart()" path fires
    aggressively on mathieucarbou/AsyncTCP because the new fork
    retries faster (= fail count climbs faster). On me-no-dev
    the path doesn't always fire because devices panic first.
    Either way, the operator sees a reset cluster with limited
    diagnostic context — only `boot_reason` and `firmware_version`
    survive the reboot. The actual reason WHY MQTT was deemed
    unrecoverable (heap state, last reconnect error, retry count,
    elapsed time since first failure) is lost.

    PROPOSAL — bundle these as v0.4.16 "restart-policy hardening":

    A. Pre-restart MQTT publish before ESP.restart().
       Already done for cred_rotate / cmd/restart (event=restarting
       with extra JSON). Extend to the MQTT-unrecoverable path:
         {
           "event":"restarting",
           "reason":"mqtt_unrecoverable",
           "fail_count":<n>,
           "first_fail_age_s":<sec>,
           "last_disconnect_reason":"<TCP_DISCONNECTED|...>",
           "heap_free":<n>,
           "heap_largest":<n>,
           "wifi_rssi":<n>
         }
       Drains in 200 ms (existing pattern). Without this, post-mortem
       in mosquitto.log shows only the LWT — no firmware-side context.

    B. Persist restart context to NVS for surfacing on next boot.
       Cross-boot diagnostics. NVS namespace "rstdiag" with last
       N entries (ring buffer, e.g. last 8). Boot announcement
       includes "last_restart_reasons":["mqtt_unrecoverable",...]
       so the dashboard sees patterns ("3 of last 8 boots were
       MQTT-unrecoverable" → broker is unstable, not the device).

    C. Increase MQTT_RESTART_THRESHOLD from 10 → 30 (or make it
       time-based: "no successful publish in N minutes" instead
       of "N consecutive failed reconnects"). Time-based is more
       intuitive and survives backoff-window weirdness.

    D. Reduce restart-loop risk via NVS cool-off counter.
       If a device has restarted ≥3 times in the last 10 minutes
       AND each was reason=mqtt_unrecoverable, fall back to
       AP/config portal mode (or modem-sleep + heartbeat-only
       passive mode) instead of restarting again. Operator can
       inspect via web UI; broker can be fixed without device-side
       interaction.

    E. Capture core dump on panic.
       Add `coredump` partition to partitions.csv (~64 KB).
       Configure `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` +
       `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`. After a panic
       reboot, firmware reads the saved core dump and publishes
       its base64-encoded blob to .../diag/coredump (one-shot,
       cleared after publish). Eliminates the "no backtrace
       captured" problem we hit twice today (#51 and the 10:42
       cascade) — backtrace is in flash, not lost on RTC WDT.

    F. Tune the watchdog.
       CONFIG_ESP_TASK_WDT_TIMEOUT_S default is 5 s. The reconnect
       handshake under high jitter / packet loss can come close.
       Bump to 10-15 s in sdkconfig.defaults — tradeoff: longer
       hangs are harder to detect, but legitimate slow reconnects
       no longer get clobbered. Combine with (A) so we publish
       diagnostic state before the WDT bites.

    G. Distinct boot_reason granularity.
       Today: "software" lumps together cred_rotate restart,
       cmd/restart, MQTT-unrecoverable, OTA reboot, etc. Add
       a complementary `restart_cause` field to the boot
       announcement (NVS-stored, set just before ESP.restart()).
       Lets the dashboard distinguish a deliberate cred_rotate
       boot from an emergency restart.
       SHIPPED 2026-04-27 in commit ef13507. New include/restart_cause.h
       wraps Preferences with set()/consume() (NVS namespace "rstdiag",
       key "cause"). mqttScheduleRestart() now persists the reason
       to NVS just before arming the deferred-restart deadline.
       mqttBegin() consumes-and-clears the saved cause once on boot,
       caches in _firstBootRestartCause. mqttPublishStatus("boot")
       appends `"restart_cause":"<reason>"` to the JSON when non-empty.
       Validated on Bravo: cmd/restart → reboot → boot announcement
       carries `restart_cause:"cmd_restart"`; subsequent heartbeats
       do not (one-shot). M1+M2 chaos pass on the new build.
       Sub-G complete; only persists software-initiated restarts
       (panic/wdt boots correctly produce empty restart_cause).

    H. Better backoff math.
       Current: 1 → 2 → 4 → 8 → 16 → 32 → 60 s. With
       MQTT_RESTART_THRESHOLD=10, the 10th attempt happens at
       sum(1..60) ≈ 4 minutes. So today a 30-second outage can
       hit threshold via fast retries OR a 5-minute outage can
       hit it via cap-of-60. Make the threshold time-based
       ("no successful publish in 10 minutes") rather than
       attempt-based.

    I. Decouple the two failure modes in chaos tests.
       The watchdog reset (TG1WDT/TG0WDT, "int_wdt") and the
       MQTT_RESTART_THRESHOLD path (SW_CPU_RESET) are both
       "device rebooted itself" but the diagnostic value is
       very different. Surface them as separate metrics in
       /daily-health and the heap-trajectory tile.

    SEVERITY: HIGH for fleet operability — without core dumps and
    pre-restart publishes, every fleet-wide event today required
    manual serial monitor + addr2line to root-cause. The 2026-04-27
    14:04 backtrace cost ~2 hours of session time we wouldn't have
    spent if (E) were already shipped.

────────────────────────────────────────────────────────────
77. Adaptive OTA stagger interval
    OBSERVATION (2026-04-27): Manual OTA staggers use a fixed
    2-minute gap between device triggers. That's conservative —
    sized for worst-case OTA download (1.6 MB over slow Wi-Fi)
    plus reboot + Wi-Fi reassoc + MQTT reconnect. Most devices
    finish the cycle in 30-50 seconds.

    PROPOSAL — adaptive stagger driven by ack:

    A. Reduce default 120s → 60s.
       Most observed cycles complete in <50s. 60s leaves headroom
       without doubling the rollout time.

    B. Adaptive: trigger next device as soon as previous publishes
       a heartbeat on the new firmware version (proves OTA succeeded
       AND the new build is healthy). Subscribe to `+/status`,
       wait for `event=heartbeat AND firmware_version=<target>`.
       Floor at 30s to give the broker time to flush retained
       messages from the just-rebooted device. Cap at 180s to
       protect against silent-fail.

    C. Backoff on observed failure.
       If the previous device shows `boot_reason=panic|task_wdt|
       int_wdt` after OTA, PAUSE the rollout. Operator decides
       whether to retry, roll back manifest, or escalate.

    D. Shipped as `tools/fleet_ota.sh` (already exists at
       tools/fleet_ota.sh — needs the adaptive logic added).

    SEVERITY: LOW. Quality-of-life improvement. Saves ~5-10
    minutes per fleet rollout. With #76 restart-policy hardening
    in place, the safety net is robust enough to allow shorter
    stagger.


────────────────────────────────────────────────────────────
78. AsyncTCP _error path race — replace stack or patch library
    OBSERVATION (2026-04-27 chaos M3 180s):  After v0.4.14 fixed
    the firmware-side trigger (MQTT_HUNG_TIMEOUT_MS too short) and
    v0.4.15 removed the ESP.restart() from mqttIsHung, M3 STILL
    cascades. Charlie panics at ~113s into a 180s broker outage
    with multiple distinct exception shapes across reboots:
       - LoadStoreAlignment (EXCVADDR ends in odd nibble)
       - StoreProhibited    (write to read-only / NULL+offset)
       - InstructionFetchError (PC in DRAM, function ptr corrupted)

    All backtraces converge on:
       PC: tcp_arg @ lwip/src/core/tcp.c:2039
       Caller: AsyncClient::_error @ AsyncTCP.cpp:1024
       From: _async_service_task

    The bug is inside AsyncTCP's error-callback path triggered when
    lwIP's natural TCP-timeout fires after a long unreachable connect
    attempt. The race is between AsyncTCP's async-task processing
    queued events and lwIP's TCP timer firing a separate error
    callback. Tested with mathieucarbou/AsyncTCP v3.3.2 — same shape.

    PROPOSAL (one of):

    A. Patch AsyncTCP locally.
       Add tcpip_api_call wrappers around tcp_arg / tcp_recv /
       tcp_sent / tcp_err in _error path so all lwIP calls happen
       under the TCPIP core lock. Vendor a patched AsyncTCP into
       lib/ to keep the patch reproducible.

    B. Replace AsyncMqttClient + AsyncTCP with a synchronous stack.
       PubSubClient or arduino-esp32's bundled WiFiClient + manual
       MQTT framing. No async race surface. ~50 KB extra flash;
       loses non-blocking publish (every publish blocks loop()
       briefly under network stress). Acceptable for the heartbeat
       cadence (every 30 s).

    C. Pre-connect broker probe.
       Before AsyncMqttClient.connect(), do a non-blocking TCP
       SYN probe (raw socket, 1 s timeout). If probe fails, skip
       this reconnect cycle and wait. Avoids issuing connect()
       calls into a dead broker, eliminating the ~75 s lwIP
       timeout window where the race fires.

    Option C is the most surgical (no library changes); option B
    is the most robust (eliminates whole bug class); option A is
    medium-cost-medium-risk.

    SEVERITY: MEDIUM. M2 (30s outages) which represent the
    common production failure (mosquitto log rotation, AP blip)
    are clean on v0.4.14+. M3 (>75s outages) hit primarily during
    network maintenance windows and AP / broker host crashes —
    less frequent but real. Devices DO recover via boot loop.

    UPDATE 2026-04-27 evening — REPRODUCED ON v0.4.20 (broker-probe
    in place):

    Two retained `/diag/coredump` payloads captured tonight via
    the v0.4.17 ESP-IDF coredump-to-flash path (sub-E of #76):

      Charlie  exc_pc=0x4012210e exc_cause=InstructionFetchError
                backtrace 0x4012210e 0x400e1d04 0x400e1fcb 0x4008ff31
      Bravo    exc_pc=0x40122127 exc_cause=StoreProhibited
                backtrace 0x40122127 0x400e1d08 0x400e1fcf 0x4008ff31

    Both decoded against the v0.4.20 ELF (esp32dev) via
    xtensa-esp32-elf-addr2line:

      panic site (frame 0):
        raw_netif_ip_addr_changed
        at lwip/src/core/raw.c:664-667 (discriminator 1/20)

      caller (frame 1, addr2line gave _accepted at AsyncTCP.cpp:1633-1634
      but symbol overlap is ambiguous — confirmed code path is
      _async_service_task → _handle_async_event → _s_fin)

      caller (frame 2):
        AsyncClient::_s_fin at AsyncTCP.cpp:1491
          (inlined into _handle_async_event @ 275, _async_service_task @ 307)

      task root (frame 3):
        vPortTaskWrapper at FreeRTOS port.c:139

    HYPOTHESIS REVISED:
    The panic site is NOT inside AsyncTCP itself — it's inside lwIP's
    `raw_netif_ip_addr_changed`, which fires when a NIC's IP address
    changes (DHCP renewal, link flap, AP reassoc). That callback walks
    the global `raw_pcbs` list. AsyncTCP allocates / frees raw_pcbs
    asynchronously from `_async_service_task`. The race:

      1. Wi-Fi link flaps; lwIP queues a netif IP-change callback.
      2. AsyncTCP's _async_service_task is mid-FIN-ACK handling, frees
         a raw_pcb via tcp_close → tcp_pcb_purge → memp_free.
      3. lwIP's IP-change callback walks raw_pcbs while one entry's
         memory has just been recycled.
      4. Random fault depending on what the freed memory now contains:
         StoreProhibited (write-into-readonly), LoadStoreAlignment
         (misaligned because previous memp's value isn't aligned),
         InstructionFetchError (function ptr field now contains
         non-code bytes).

    The v0.4.16 broker-probe (option C above) prevents the FIN-ACK
    for *broker connections that are dead* — it does not prevent the
    same race for *broker connections that succeed but later see Wi-Fi
    flap mid-publish*. Both Bravo and Charlie were on a healthy broker
    today; the trigger was Wi-Fi reassoc / DHCP traffic, not broker
    outage. So the broker-probe is necessary but not sufficient.

    NEW PROPOSAL — option D (most surgical for the actually-observed
    bug):

    D. Wrap the raw_pcb walk under TCPIP core lock.
       The lwIP `raw_netif_ip_addr_changed` callback walks raw_pcbs
       *outside* the TCPIP core lock by default — the callback is
       invoked from `netif_set_ipaddr` which IS under the lock, but
       the iteration over `raw_pcbs` doesn't synchronise against
       AsyncTCP's free path on a different task. Patch AsyncTCP's
       free-pcb path to take `LOCK_TCPIP_CORE() / UNLOCK_TCPIP_CORE()`
       around `tcp_close()`/`tcp_abort()`.
       This is a 5-10 line vendored patch.

    E. Also valid: avoid the bug entirely by switching MQTT publish
       to a synchronous stack (option B above). Larger change, less
       targeted, but eliminates whole bug class.

    NEXT STEPS:
       - Implement option D as a vendored AsyncTCP patch on Bravo's
         bench build. Validate via M2 + M3 + a Wi-Fi flap test
         (`netsh wlan disconnect; sleep 5; netsh wlan connect`).
       - If D fails, attempt B (PubSubClient + WiFiClient.publish).

    UPDATE 2026-04-27 ~22:00 — option D NOT obviously wrong-but-needed:
       The mathieucarbou fork ALREADY wraps tcp_close + tcp_abort in
       `tcpip_api_call`, which holds LOCK_TCPIP_CORE for the duration
       of the lwIP call. The IP-change callback `raw_netif_ip_addr_changed`
       walks `raw_pcbs` (a SEPARATE list from `tcp_pcbs`), and our app
       does not allocate raw_pcbs — those are managed by ESP-IDF
       internally for ICMP / ping. So the "use-after-free walking a
       freed pcb" hypothesis doesn't fit cleanly: a TCP free shouldn't
       corrupt raw_pcbs at all.

       Two possibilities remain:
         (a) Memory corruption from elsewhere (stack overflow in some
             task, heap overrun in our code, ESP-IDF regression in a
             3rd-party component) leaves bad bytes in a freed memp slot
             that raw_pcbs's iterator later reads. The PC is just the
             unfortunate site that exposes the corruption.
         (b) raw_pcbs entries ARE present (ESP-IDF created them for
             internal ICMP/DHCP) and one of them got freed by a code
             path we don't control. Then walked under the lock by
             raw_netif_ip_addr_changed.

       Charlie's canary soak (#54, now sticky after the OTA_DISABLE
       gate fix) is the right next data point: if Charlie panics with
       a stack-overflow halt → (a) confirmed → bump task stack. If
       Charlie panics with the same lwIP raw.c backtrace → (a) NOT
       stack overflow → likely (b), needs deeper memp_malloc audit.
       Defer fix attempt until canary produces a failure signature.

────────────────────────────────────────────────────────────
79. Version-update watcher as a standing dev tool + ack-driven OTA
    OBSERVATION (2026-04-27): During the v0.4.13 → v0.4.15 OTA
    rollouts today the operator asked "can you watch for the
    version update?" — confirming a per-session need for an
    always-on watcher that streams `firmware_version` transitions
    per device. Without it, OTA verification is a manual
    mosquitto_sub | grep cycle that hides edge cases (Charlie
    running -dev when fleet was meant to be on release).

    ALSO from #77: the "trigger next when previous is verified up"
    pattern is the natural complement.

    PROPOSAL — bundle as `tools/dev/`:

    A. tools/dev/version-watch.sh
       Long-running mosquitto_sub on `+/status` that prints one
       line per (device, version) transition. Filter to first
       boot/online/heartbeat per (device, version) pair so the
       output is signal-only. Print uptime + boot_reason for
       diagnostic context.

    B. tools/dev/ota-rollout.sh <target_version>
       Read fleet UUIDs from cached config (or discover via live
       MQTT). For each:
         (1) record current firmware_version
         (2) publish cmd/ota_check
         (3) wait for status with firmware_version == target AND
             event in (boot, online, heartbeat) AND uptime_s > 30
             (proves device stayed up)
         (4) on success → next device after a short safety gap
             (e.g. 15s)
         (5) on failure (no transition in N min, or device boot
             with abnormal boot_reason) → PAUSE and prompt
             operator
       Logs to JSON for post-rollout audit.

    C. Promote both to chaos-suite (#75):
       version-watch.sh runs always during chaos tests so version
       drift between expected/actual is caught immediately.
       ota-rollout.sh becomes a chaos scenario itself
       (sequenced_ota_storm) — verify rollouts don't spawn
       cascade-shaped failures.

    SEVERITY: MEDIUM. Saves ~10 min per rollout. Catches version
    drift earlier. Provides audit trail. With #78 broker-probe
    in firmware, OTA storms become a non-issue and ack-driven
    stagger can be aggressive (15-30s gaps).
    STATUS: RESOLVED 2026-04-27. Both tools shipped:
    tools/dev/ota-rollout.sh and tools/dev/version-watch.sh
    (commit deaa97f). Used through the v0.4.13 → v0.4.20 cascade
    rollout sessions. Sub-item C (promote to chaos-suite) carries
    forward into #75.

────────────────────────────────────────────────────────────
69. Wakeup vs persistent-monitor preemption
    OBSERVATION (2026-04-27): During the v0.4.13 → v0.4.16 cascade
    debugging session the agent ran 4 persistent Monitor tasks
    (silent_watcher, Bravo heap, Charlie serial, version-watch).
    With a 6-device fleet publishing heartbeats + espnow + status
    events, the gap between Monitor emissions averaged < 30 s.
    Each emission preempts the `/loop dynamic` wakeup idle timer,
    so any wakeup scheduled >60 s out almost never fires. The
    agent had to fall back to manual "next" prompts from the
    operator for OTA staggers.

    PROPOSAL — multiple complementary fixes:

    A. Tighter event filtering in monitors.
       Most events from silent_watcher / version-watcher are
       informational. Suppress heartbeat events; emit only state
       transitions (online → offline, version_old → version_new,
       boot reasons in {panic, *_wdt, brownout}). Cuts emission
       rate ~10×.

    B. Aggregator flow in Node-RED.
       One subscription that watches `+/status` and publishes a
       SINGLE summary event to `health/summary` once per N min
       OR on state change. Agent monitors that aggregate, not
       per-device events.

    C. Composite progress events for OTA rollouts.
       New Node-RED flow: subscribe to status, count devices on
       target version, publish `health/ota_progress` like
       `{"target":"0.4.16","done":4,"pending":["bravo","charlie"]}`
       only when the count changes. Agent monitors that one topic
       during a rollout.

    D. CronCreate (scheduled-task) instead of /loop dynamic.
       Wakeups via /loop dynamic compete with conversation
       events. CronCreate fires on a schedule regardless. For
       fixed-cadence work (OTA stagger, daily-health) this is
       the right primitive — but it doesn't accept dynamic
       prompts mid-loop. Use for predictable schedules; keep
       /loop for adaptive work.

    E. Single orchestrator script.
       tools/dev/ota-rollout.sh from #79 runs the entire stagger
       in one shell process. Agent uses ONE Monitor task on its
       output, not many. Operator watches one stream, agent
       responds to one signal stream.

    SEVERITY: MEDIUM. Affects autonomous-session productivity
    when monitors are dense; doesn't affect production reliability.
    Combo of A + C + E gives the best balance — fewer events,
    semantic events (summary not raw), single orchestrator.
    STATUS: RESOLVED 2026-04-28. Sub-E (single orchestrator) shipped
    via tools/dev/ota-rollout.sh (#79) + the new tools/dev/ota-monitor.sh
    (#84). Sub-D (CronCreate vs /loop dynamic) is now a documented
    decision — agents use /loop dynamic for adaptive work and
    CronCreate for fixed cadence. Sub-A (event filtering) is partially
    addressed by the wakeup-cadence rule in the autonomous template
    (#84's C-fix: 60-300 s active, 1200-1800 s idle). Sub-B/C
    (Node-RED aggregator + composite OTA progress events) are
    operator-side dashboard work — defer until the next user-visible
    Node-RED change cycle. Index moved to RESOLVED.

────────────────────────────────────────────────────────────
80. -dev suffix breaks OTA upgrade path (recurring friction)
    OBSERVATION (2026-04-27 multiple times):  Devices USB-flashed
    with a `0.4.X-dev` binary do not OTA-upgrade to the matching
    `0.4.X` release. Triggering `cmd/ota_check` on a -dev device
    produces no upgrade. Friction observed across:
       v0.4.13-dev vs v0.4.13 release (Charlie all afternoon)
       v0.4.14-dev vs v0.4.14 release (Charlie + Bravo)
       v0.4.15-dev vs v0.4.15 release (same)
       v0.4.16-dev vs v0.4.16 release (current)

    The ESP32-OTA-Pull library compares version strings; for the
    `0.4.X-dev` vs `0.4.X` pair it appears to treat them as equal
    (or treats -dev as newer due to longer string). Either way
    no upgrade pulls. Operator must USB-reflash to escape -dev.

    PROPOSAL — pick one:

    A. Bump the dev suffix to a CLEARLY-OLDER pattern.
       Use `0.4.X-dev.0` (or similar pre-release identifier that
       semver knows is older). Or include a build counter:
       `0.4.X-dev.123`. Fits semver pre-release ordering rules
       (pre-release < release).

    B. Use an extra decimal on dev builds.
       Local build:    `0.4.16.0`  (treated as older than 0.4.17)
       CI release:     `0.4.16`
       Pre-release:    `0.4.16.99` (just below next release)
       Cleanest for string-compare-based comparators.

    C. Custom version comparator in firmware.
       Override the OTA-pull library's compare function to:
         - strip `-dev` suffix before compare
         - if equal after strip → consider release strictly greater
       Adds a small patch to the OTA hot path; library-version-coupled.

    D. Clear NVS on USB reflash so the pending version isn't sticky.
       Less likely to be the cause given the symptom is "OTA does
       not pull", but worth verifying.

    SEVERITY: MEDIUM. Causes ~5-10 min of extra work per dev cycle
    when devs need to USB-reflash to bypass. Option B is the
    smallest change with the cleanest semantics.

    RELATED to #77 (adaptive OTA stagger) and #79 (ack-driven
    rollout) — both rely on the OTA path actually firing for -dev
    devices.
    STATUS: RESOLVED 2026-04-27 in v0.4.18 + v0.4.20. v0.4.18
    (commit b0705b1) made the firmware's semverIsNewer treat
    `0.4.X-dev` as older than the same-numbered release.
    v0.4.20 (commit 08142c2) added the 4-component
    "MAJOR.MINOR.PATCH.DEV" dev versioning for cleaner monotonicity.
    -dev devices now upgrade to the matching release on first OTA check.

────────────────────────────────────────────────────────────
81. Renumbering pass on SUGGESTED_IMPROVEMENTS_ARCHIVE.md
    OBSERVATION (2026-04-27):  The 2026-04-26 audit-driven
    additions and the 2026-04-27 cascade-fix-session additions
    both used #58–#70, producing collisions documented in the
    "KNOWN NUMBERING COLLISIONS" warning at the top of
    SUGGESTED_IMPROVEMENTS.md. Each colliding number currently
    has two entries in the archive; the index lists only the
    first-occurrence and disambiguation requires "top vs bottom
    of archive" hinting.

    PROPOSAL:  Reassign today's cascade-fix-session items to a
    fresh range. Today's items currently sharing numbers with
    audit items are #58, #59, #60, #63, #64, #65, #66, #67, #68,
    #70 — ten items, fitting cleanly into #71–#80. Walk both
    files in lockstep:
       - Update each cascade-fix entry's heading number in the
         archive (top half) to its new value in #71–#80.
       - Update the matching summary line in the OPEN INDEX.
       - Update any cross-references inside other entries (e.g.
         entry #70's "RELATED to #66 / #68" becomes its renumbered
         equivalents).
       - Update memory files that reference these numbers
         (see memory/v0_4_13_panic_cascade_2026_04_27.md and
         autonomous_session_2026_04_27.md for #65, #66, #70).
       - Remove the "KNOWN NUMBERING COLLISIONS" warning block
         from SUGGESTED_IMPROVEMENTS.md once the pass completes.

    EFFORT: ~15 min mechanical edit pass.

    SEVERITY: LOW. Cosmetic / hygiene; the warning block already
    documents the workaround. But the workaround relies on
    "first-occurrence" ordering inside the archive which is
    fragile if entries are ever inserted out of order.
    STATUS: RESOLVED 2026-04-27. Cascade-fix-session entries
    renumbered to #71-#80 in archive + index. Cross-references
    inside renumbered entries updated. KNOWN NUMBERING
    COLLISIONS warning removed from index header. Memory files
    autonomous_session_2026_04_27.md and v0_4_13_panic_cascade_2026_04_27.md
    were not edited because they describe a moment-in-time state
    (which numbers were being used during the cascade session)
    and rewriting the numbers would falsify the historical record;
    forward references in those files now refer to a stale numbering
    scheme but the surrounding context disambiguates.

────────────────────────────────────────────────────────────
82. Audit other tracking-style docs for the index/archive split pattern
    OBSERVATION (2026-04-27):  Per the tracking-doc convention
    introduced in commit 95a9fad (memory/tracking_doc_convention.md),
    SUGGESTED_IMPROVEMENTS was split into a short OPEN INDEX
    (SUGGESTED_IMPROVEMENTS.md, ~115 lines) plus a full
    SUGGESTED_IMPROVEMENTS_ARCHIVE.md (~3070 lines). Two other
    docs in esp32_node_firmware/docs/ fit the same shape:
       - ESP32_FAILURE_MODES.md  (502 lines, mix of resolved + open)
       - memory_budget.md        (277 lines, mix of resolved + open)

    PROPOSAL:  Apply the same INDEX + ARCHIVE split to both:
       - ESP32_FAILURE_MODES.md  → short INDEX of failure modes
                                   (one-line summary per item)
                                   plus ESP32_FAILURE_MODES_ARCHIVE.md
                                   for full per-incident detail.
       - memory_budget.md        → short INDEX of memory hotspots
                                   plus memory_budget_ARCHIVE.md
                                   for full per-region history.
       Resolved items move to a "RESOLVED" tail in the index;
       full text stays in the archive.

    EFFORT:  ~30 min per doc. Lower priority than #81 since
    these don't have collision warnings — purely a length /
    scannability improvement.

    SEVERITY: LOW. Improves agent + human navigation when these
    docs grow further; no functional impact today.
    STATUS: WONT_DO 2026-04-27 — re-examined the candidates and
    neither fits the convention's prerequisites:
       ESP32_FAILURE_MODES.md is a structured reference catalogue
       (failure-mode taxonomy + triage flowchart + historical
       evidence). Its "open vs resolved" distinction doesn't apply
       — the catalogue is meant to be read top-to-bottom, and
       per TRACKING_DOC_CONVENTION.md "When NOT to apply this
       pattern" this kind of doc is explicitly excluded.
       memory_budget.md is also a reference doc — flash/IRAM/DRAM
       breakdowns at known build versions. No item-lifecycle.
    Conclusion: the convention applies to backlog docs, not
    catalogues. Removing #82 from OPEN.


────────────────────────────────────────────────────────────
83. Mosquitto log frozen at 13:59 — file write stops after first
    blip-watcher cycle
    OBSERVATION (2026-04-27 22:47 SAST):  C:/ProgramData/mosquitto/
    mosquitto.log is 236 MB, last write timestamp 13:59:47, current
    wall-clock is 22:47 — 8+ hours without the file growing despite
    the broker actively serving the fleet. Mosquitto service is up,
    fleet is publishing/subscribing fine; only the LOG output to disk
    has stopped.

    The blip-watcher runs `net stop mosquitto; sleep N; net start
    mosquitto` for every M1/M2/M3 chaos test (we've run several
    today). Plausible causes:
       (a) The service-restart loses the file handle that mosquitto
           opens at startup, and on restart it cannot reacquire it
           because the previous handle is still held by something
           (defunct child? OS file-handle leak?).
       (b) The log path gets rotated (size-based trigger?) but the
           rotated-to file isn't created so writes silently no-op.
       (c) `apply-logging-config.ps1` (per CLAUDE.md) isn't being
           re-applied after the service restart, so the post-restart
           mosquitto runs with default logging.

    IMPACT: Mosquitto-log-based diagnostics (the FIRST step in the
    "Diagnostic process" section of CLAUDE.md) become useless after
    the first blip cycle. Daily-health's mosquitto-log-size warning
    fires (we saw it tonight: "230872.7 KB, last write 403.8 min ago"
    flagged as YELLOW).

    INVESTIGATION:
       1. Read mosquitto.conf and confirm `log_dest file <path>` is
          set; check if `log_type all` is enabled.
       2. After a known blip cycle, check `Get-Process mosquitto |
          Select-Object Id, Path` to confirm the running service.
          Compare to `Get-Process | Where-Object Id -eq <pid>` from
          before the blip — if PID changed, restart succeeded.
       3. Check whether `apply-logging-config.ps1` is hooked into the
          service-start path (Windows scheduled task on service start?
          NSSM wrapper?). If not, the logging config is one-shot.

    PROPOSED FIX:
       Either bake the logging config into mosquitto.conf permanently
       (so it survives service restarts), OR wrap mosquitto in NSSM
       which re-applies the config. The current "elevated apply once"
       model loses state on every restart.

    SEVERITY: MEDIUM. The diagnostic step isn't broken — just stale.
    Live MQTT subscribe still works (we used it throughout the
    autonomous window). But the historical-log step in
    Diagnostic process is currently rotted.

    DISCOVERED: 2026-04-27 evening autonomous window during the
    Alpha-panic investigation (couldn't grep mosquitto.log for the
    last-publish timestamp because the file was 9 h stale).

    ROOT CAUSE (confirmed 2026-04-28 ~08:08 SAST):
    The mosquitto.log file had grown to ~236 MB (no daily rotation
    task ever ran — the schtasks /create from rotate-log.ps1's setup
    note was never executed). Mosquitto on Windows silently FAILS to
    re-open an existing log file once it crosses the ~200 MB boundary
    on a service restart — but doesn't error to the service control
    manager. The service appears healthy; only the log is dead. This
    is why the file was untouched from 13:59 the previous day onward
    despite multiple `net stop/start mosquitto` cycles via the
    blip-watcher.

    FIX (applied 2026-04-28):
       1. Renamed the dead 236 MB file to mosquitto.log.archive-2026-04-28
          via `Move-Item` (works without elevation; mosquitto did NOT
          have an open handle, confirming the failed-open theory).
       2. Triggered a 5 s blip; mosquitto's restart created a fresh
          0-byte mosquitto.log and reopened it for append. Within 30 s
          the file grew to 23 KB with current heartbeats from the
          fleet — write path restored.
       3. Strengthened C:\ProgramData\mosquitto\rotate-log.ps1: added
          $maxSizeMB = 100 size-cap rotation in addition to the daily
          rotation. If the daily rotation has already run today AND
          the log has crossed 100 MB, rotates again with a counter
          suffix (mosquitto.log.YYYY-MM-DD.1, .2, ...). Prevents the
          freeze recurring even if the daily task is missed.
       4. Saved a copy at esp32_node_firmware/tools/mosquitto/rotate-log.ps1
          so the strengthened version is checked-in for reproducibility.
          The active runtime copy at C:\ProgramData\mosquitto\rotate-log.ps1
          is the same file content.

    REMAINING OPERATOR ACTION (one-time, requires elevation):
       Verify or register the daily rotation scheduled task:
          schtasks /create /tn "MosquittoLogRotate" \
                   /tr "powershell -File C:\ProgramData\mosquitto\rotate-log.ps1" \
                   /sc daily /st 02:00 /ru SYSTEM
       Without this, the size-cap fallback still saves us, but only
       if rotate-log.ps1 is invoked somehow (e.g. ad-hoc by operator).

    STATUS: RESOLVED 2026-04-28. Live mosquitto.log writes restored.
    Size-cap rotation guards against recurrence. Index moved to
    RESOLVED.


────────────────────────────────────────────────────────────
84. Agent post-action verification gap — silent waits after fire-and-forget
    OBSERVATION (2026-04-28 ~08:50 SAST):  After cutting v0.4.22 and
    triggering fleet OTA via `mosquitto_pub` to 5 devices, the agent
    went silent for ~23 min until the operator asked "what are we
    waiting for?". The fleet was actually fine — all 5 devices had
    OTA'd cleanly to v0.4.22 within ~3 min — but the agent posted no
    verification or "rollout complete" status. Operator had to
    intervene to find out the state.

    ROOT CAUSE — three interacting agent-discipline gaps:
       1. **Fire-and-forget actions don't auto-trigger verification.**
          A `mosquitto_pub cmd/ota_check` is a one-shot publish; it
          returns instantly; nothing awaits an "OTA succeeded" event.
          The agent moves on with no built-in poll.
       2. **/loop dynamic relies on events.** Wakeup-driven loops are
          designed around watchers / task-notifications. Without an
          armed Monitor, the loop iteration ends after the synchronous
          work and the next wakeup is the cadence default — but if
          the agent didn't schedule one, there's no next iteration.
       3. **No "report back" reflex.** The agent's mental model after
          "cmd/ota_check sent" was "fleet will OTA" without an
          immediate sanity check. Quiet success is treated the same
          as quiet failure — both produce silence.

    The same shape happened earlier in this autonomous window after
    the v0.4.21 OTA rollout, where the rollout script's stale-retained
    abort fired and the agent kept moving without checking that
    Alpha had actually OTA'd. (Operator had to ask there too.)

    PROPOSAL — three layered fixes, smallest first:

    A. **Verify-within-N-minutes habit.** Convention: after any
       state-changing action (flash, OTA trigger, blip, cred_rotate,
       Node-RED API push), schedule a verification poll within the
       expected completion window. Concrete:
          - Flash: serial monitor or MQTT subscribe within 60 s
          - OTA: subscribe to status for `firmware_version=<target>`
            for ~3 min per device, or use ota-rollout.sh ack-driven
          - Blip: monitor `/+/status` for `event=online` from all
            fleet members within 90 s
          - Then post a status line to the user, even if "all clean".
       Codify in CLAUDE.md "Diagnostic process" or
       AUTONOMOUS_PROMPT_TEMPLATE constraints.

    B. **Auto-armed rollout watcher.** Extend tools/dev/ota-rollout.sh
       (or write a sibling tools/dev/ota-monitor.sh) that, given a
       target version, subscribes to the fleet's /status topic and
       prints one line per device as each picks up the new version.
       Exits when all expected devices match or after a timeout.
       Used standalone or piped from the release skill's tail.
       Operator sees rollout progress in real time; agent gets a
       single "rollout complete in N min" or "X/Y devices stuck"
       summary to relay.

    C. **Periodic-status discipline in /loop dynamic.** When the
       agent self-paces (no Monitor armed), default to ~5 min
       wakeups rather than 25-30 min idle ticks during ACTIVE work.
       Bump back to 30 min only when the operator explicitly says
       "going AFK" or the agent has confirmed nothing is actively
       changing. The autonomous-template's COMMUNICATION STYLE
       section already says "I'll be back in a few hours usually
       means 5-30 min" — extend with "wakeup cadence should match
       expected next-event window, not the 25-30 min default".

    SEVERITY: MEDIUM — affects autonomous-session UX, not fleet
    reliability. The fleet was fine; the failure was that the
    operator couldn't tell it was fine without prompting.

    DISCOVERED: 2026-04-28 ~08:50 SAST during the v0.4.22 release
    rollout. Operator intervention surfaced an otherwise-clean
    state, exactly the wrong direction for an autonomous-mode
    session that's supposed to free operator attention.

    STATUS: RESOLVED 2026-04-28. All three layered fixes shipped:

       A. CLAUDE.md got a new "Verify-after-action discipline (#84)"
          section with a per-action verification-window table (USB
          flash 60 s, OTA single 3 min, OTA fleet 5 + 3 min/device,
          blip 90 s, cmd/restart + cred_rotate 90 s, Node-RED push
          30 s). AUTONOMOUS_PROMPT_TEMPLATE CONSTRAINTS gained a
          "Verify-within-N-minutes" bullet that explicitly requires
          the agent to post a status line — even when all-clean.

       B. tools/dev/ota-monitor.sh shipped. Passive observer (does
          NOT trigger OTAs) — subscribes to /+/status, prints one
          line per device as each picks up the target version,
          exits on all-match, timeout, or first abnormal boot. Pairs
          with `mosquitto_pub cmd/ota_check` or
          `tools/dev/ota-rollout.sh`. Smoke-tested by triggering a
          Bravo cmd/restart with a single-UUID watch — match line
          fired at 1 s, exit 0.

       C. AUTONOMOUS_PROMPT_TEMPLATE CONSTRAINTS gained a wakeup-
          cadence rule: 60-300 s during ACTIVE work, 1200-1800 s
          only when operator is AFK or fleet is confirmed steady.
          Folded into the same constraints block as A so the rule
          is one read.

    Tested live by tonight's autonomous session (this commit).


────────────────────────────────────────────────────────────
85. End-of-session doc-sweep is operator-prompted, not agent-driven
    OBSERVATION (2026-04-28 mid-morning): After the v0.4.23 release
    shipped + fleet OTA'd cleanly, the agent stopped without sweeping
    the docs. The operator had to issue a series of explicit "update
    all docs", "update TRACKING_DOC_CONVENTION", "update SUGGESTED_
    IMPROVEMENTS", "check what is still open" prompts to get the
    expected end-of-session hygiene done. Each prompt was answered
    correctly, but the operator's role here was driving a checklist
    that should have been part of the agent's natural session-close.

    Same shape as #84 (silent waits after fire-and-forget) but at
    a different abstraction layer: #84 was about not posting
    verifications during work; #85 is about not running the closing
    sweep at the end of work.

    PATTERN OBSERVED — when a session's main goal is reached (e.g.
    release tagged + fleet OTA'd), the agent's energy drops to
    "answer follow-up questions" instead of completing the session
    structurally. ROADMAP entries for the just-shipped release,
    NEXT_SESSION_PLAN refresh, OPEN-list audit for stale entries,
    docs/README.md updates for new docs — all silently skipped
    until prompted.

    PROPOSAL — three layered fixes, smallest first (mirroring #84):

    A. Codify in AUTONOMOUS_PROMPT_TEMPLATE DONE WHEN. Add a
       fourth implicit requirement: "End-of-session doc-sweep run."
       Add a new "End-of-session checklist" section with concrete
       steps (CLAUDE.md version bump, ROADMAP entry, NEXT_SESSION_PLAN
       refresh, OPEN-list audit, README index update, memory sync,
       final commit + push, fleet snapshot post). Done in this
       commit.

    B. Tooling — `tools/dev/end-of-session-sweep.sh`. Walks the
       checklist, surfaces candidates the agent must judge:
          - Compares CLAUDE.md fleet-table version against the
            latest release tag.
          - Diff-checks ROADMAP "Now" section vs `git log --oneline`
            since the prior ROADMAP edit; lists release tags missing
            an entry.
          - Greps SUGGESTED_IMPROVEMENTS_ARCHIVE for STATUS lines
            that say RESOLVED but where the OPEN-INDEX still lists
            the entry.
          - Verifies docs/README.md mentions every file in docs/
            and SESSIONS/ + archive/.
       Output: one-screen TODO that the agent can work through.
       Not implemented this session — planned for next followup batch.

    C. CLAUDE.md "Verify-after-action discipline" section gains a
       sub-section on "session close". Same checklist as A's
       template addition, just located where the operator is more
       likely to read it during a hand-off. Done in this commit.

    SEVERITY: MEDIUM — affects autonomous-session UX and the
    operator's experience of completion. Not fleet reliability.
    Symptom is real but easy to mistake for "agent is just wrapping
    up" — the underlying pattern shows up across multiple sessions
    in a row.

    DISCOVERED: 2026-04-28 mid-morning during the v0.4.23 follow-on
    cleanup. Operator-driven series of doc updates surfaced the gap.

    STATUS: PARTIAL FIX 2026-04-28 — A + C shipped this commit;
    B (tooling) deferred to a followup session. Re-evaluate after
    the next 2-3 autonomous sessions to see whether A + C alone
    are sufficient or whether the tooling is needed too.
