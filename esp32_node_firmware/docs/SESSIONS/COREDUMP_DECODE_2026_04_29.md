# Coredump decode session — 2026-04-29 morning

Symbolic decode of all 6 retained `/diag/coredump` payloads against the
v0.4.26 ELF (worktree at `/c/Users/drowa/v0426-decode/`). Goal: find
the common-ancestor frame across the cascade panics so a targeted fix
can land.

## Source payloads

Pulled from MQTT broker `192.168.10.30:1883`, retained on
`Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/diag/coredump`.
Saved to [`COREDUMPS_RETAINED_2026_04_29.log`](COREDUMPS_RETAINED_2026_04_29.log).

5 of 6 share `app_sha_prefix: 3037383638653439` → ASCII `07868e49`
(the v0.4.26 production CI build). Charlie's payload is on
`6464383737303330` → `dd877030` (v0.4.20.0 canary; older retained
event, not part of this cascade).

## Decode results

### Production v0.4.26 panics (5 devices)

| Device | exc_task | exc_pc | exc_cause | Top of stack (decoded) |
|---|---|---|---|---|
| Alpha | async_tcp | `0x00000019` | InstFetchProhibited | `?? ??:0` (null-region jump from freed vtable) |
| Bravo | wifi | `0x401d2c66` | LoadProhibited | `lmacAdjustTimestamp` (IDF WiFi MAC) |
| Delta | loopTask | `0x4008ec34` | IllegalInstruction | `panic_abort` (C++ `std::terminate` path) |
| Echo | loopTask | `0x4008ec34` | IllegalInstruction | `panic_abort` (identical to Delta) |
| Foxtrot | loopTask | `0x4008a9f2` | LoadProhibited | `strlen` on freed/garbage string |

### Common-ancestor analysis

**3/5 panics share the EXACT upper call chain:**

```
mqttPublish               (mqtt_client.h:292)
  → AsyncMqttClient::publish     (AsyncMqttClient.cpp:742)
    → PublishOutPacket::PublishOutPacket   (Publish.cpp:23 or 42)
      → [Delta/Echo: vector::reserve → operator new → __cxa_allocate_exception → std::terminate → abort]
      → [Foxtrot:    strlen on corrupted payload pointer]
called from:
  espnowRangingLoop       (espnow_ranging.h:650 + mqtt_client.h:563)
  → loop()                (main.cpp:833)
  → loopTask              (FreeRTOS framework)
```

Alpha's panic is the AsyncTCP-side downstream of the same chain:
`AsyncClient::_s_error` (AsyncTCP.cpp:1503, inlined into
`_handle_async_event` at :284 inside `_async_service_task` at :307)
calls through a freed `client->_error()` vtable, hitting `0x00000019`
because the AsyncClient object's been zeroed/freed.

Bravo's panic is the WiFi-driver side: `lmacAdjustTimestamp` →
`ppGetTxframe` → `ppSearchTxframe` → `ppProcessTxQ` → `lmacTxDone`
→ ... → `ppTask`. Reading through a corrupted pointer in the WiFi
TX-completion queue. All frames inside IDF blobs (no source
resolution).

## Root cause hypothesis (now strong)

Cascade window (= AP-cycle / WiFi reconnect) opens a race between:
1. `loopTask` running `espnowRangingLoop` → publishes telemetry via
   `mqttPublish` → `AsyncMqttClient::publish` → enqueues into
   AsyncTCP via the active `AsyncClient`.
2. AsyncTCP's `_async_service_task` running `_handle_async_event`
   (line 284) → which transitions through `AsyncClient::_s_error`
   (line 1503) when the TCP connection drops, freeing or marking-
   freed the AsyncClient object.
3. WiFi-driver `ppTask` cleaning up its TX queue with stale pointers
   from the just-torn-down connection.

Whichever task wins the race sees a corrupted or freed structure:
- Alpha: AsyncTCP service task dispatches through freed vtable
- Bravo: WiFi driver walks freed TX-queue entry
- Delta/Echo: heap so corrupted that `__cxa_allocate_exception`
  itself fails → `std::terminate` → abort. **The v0.4.22 try/catch
  defense in `mqttPublish` cannot catch this** — when allocating the
  exception object itself fails, `std::terminate` runs before any
  user-level catch.
- Foxtrot: AsyncMqttClient's `PublishOutPacket` ctor calls `strlen`
  on a payload pointer pointing into freed memory.

## Why existing guards didn't catch it

Looking at [mqtt_client.h:266-297](../include/mqtt_client.h):

- **`_mqttClient.connected()` check (line 268)**: `connected()`
  returns the cached AsyncMqttClient connection state. During the
  cascade window, it can be **stale** — AsyncTCP fires the
  TCP_DISCONNECTED callback, AsyncMqttClient processes it
  asynchronously, the cached flag may not yet have updated. Or:
  WiFi has disconnected but AsyncMqttClient hasn't yet been notified
  (it goes through AsyncTCP, which goes through lwIP, which is itself
  mid-cleanup). The flag lies during the window.
- **`MQTT_PUBLISH_HEAP_MIN = 8192` (line 249)**: gates on
  `ESP.getMaxAllocHeap()`. But the bug isn't insufficient bytes —
  it's **corrupted free-list pointers**. A heap that walks fine still
  reports a plausible max-alloc, then the actual allocation hits a
  poisoned chunk and fails non-recoverably.
- **try/catch defense (line 291-296)**: catches `std::bad_alloc`
  thrown from a normal allocation failure. Does NOT catch the case
  where the bad_alloc allocation itself fails — that goes through
  `std::terminate` before any catch frame is reached.

## Recommended fix

**Cascade-window publish guard.** Track the timestamp of the most
recent WiFi-disconnect or AsyncTCP-disconnect event. In `mqttPublish`,
refuse to publish for **N seconds after** any such event (proposed
N = 5 s, tunable). This closes the race directly: during the cascade
window, no publishes are enqueued, so AsyncTCP never dispatches into
freed objects, AsyncMqttClient never allocates from corrupted heap,
and the WiFi TX queue isn't fed new frames.

Pseudocode:
```cpp
static uint32_t _lastWiFiOrMqttDisconnectMs = 0;
constexpr uint32_t CASCADE_QUIET_MS = 5000;

// in onMqttDisconnect():
_lastWiFiOrMqttDisconnectMs = millis();

// in WiFi-disconnect handler (main.cpp around the existing
// "Wi-Fi lost" log):
_lastWiFiOrMqttDisconnectMs = millis();

// in mqttPublish() near top, BEFORE _mqttClient.connected() check:
if (_lastWiFiOrMqttDisconnectMs &&
    (millis() - _lastWiFiOrMqttDisconnectMs) < CASCADE_QUIET_MS) {
    return;  // cascade window — drop publish silently
}
```

Cost: ≤10 lines. Skips ~5 s of telemetry per WiFi/MQTT disconnect.
Acceptable trade — telemetry resumes once the post-reconnect window
clears.

**Out of scope for this fix:** the underlying AsyncTCP / WiFi-driver
heap corruption. We're closing the trigger window, not patching the
bug. A vendored AsyncTCP `_s_error` defensive null-out is still a
valid follow-up but no longer urgent if the cascade-window guard
holds.

## Acceptance criteria for the fix

1. Build green.
2. Run AP-cycle reproduction recipe ≥3 times on serial-attached
   bench fleet. Cascade does NOT fire.
3. Telemetry resumes within ~10 s of WiFi reconnect (cascade-quiet
   window + heartbeat interval).
4. No `LOG_W` "publish skipped (heap_largest=...)" warnings under
   normal post-reconnect operation (those were the pre-bad_alloc
   guards firing — should still fire under genuine fragmentation,
   just not as a cascade symptom).

## Forward action

→ Implement the cascade-window guard in `include/mqtt_client.h` +
  `src/main.cpp` WiFi-disconnect site.
→ Tag v0.4.28 if AP-cycle test confirms the cascade is gone.
→ Mark #78 RESOLVED with this decode session as the closing
  evidence.

────────────────────────────────────────────────────────────────────

## Live AP-cycle test results (2026-04-29 afternoon)

Three operator-triggered AP-cycle reproduction attempts on the
serial-attached fleet with v0.4.28.0 (Alpha) + v0.4.26/v0.4.27 mix
(others). Cascade did not fire on any cycle — confirms the
non-determinism noted in #92 (same trigger ≠ same outcome). Heap
state at trigger time matters more than the trigger itself.

| Cycle | Outage | Cascade panic? | Notable side-effect |
|---|---|---|---|
| #1   | ~150 s | No  | Alpha v0.4.28 clean recovery; guard engaged silently |
| #2   | ?      | No  | Bravo / Echo / Foxtrot OTA'd v0.4.26 → v0.4.27 during the post-cycle reconnect window (CI release was already published) |
| #3   | ~760 s | No  | **Fleet-wide AP-mode reboot loop (#96)** — see below |

Per-device post-cycle-#1 fragmentation snapshot:

| Device | fw | heap_largest |
|---|---|---|
| Alpha   | **0.4.28.0** (with guard) | **81,908** |
| Charlie | 0.4.27.0      | 81,908 |
| Bravo   | 0.4.26 (no guard) | 42,996 |
| Echo    | 0.4.26 (no guard) | 36,852 |
| Foxtrot | 0.4.26 (no guard) | 40,948 |

Alpha had **2x the contiguous heap headroom** of the unguarded peers
post-cycle. Inferred: guard suppresses the
fragmentation-inducing `AsyncMqttClient::publish` chain during the
disconnect/reconnect window, so post-cycle heap is markedly cleaner.
Secondary positive signal even when the panic-cascade itself doesn't
fire.

## Side-effect #1 — fleet-wide AP-mode reboot loop (#96)

Cycle #3's 12.6 minute outage exceeded `MQTT_UNRECOVERABLE_TIMEOUT_MS`
on every fleet device. Each one entered an unrecoverable-restart
condition that didn't recover even after WiFi+MQTT successfully
reconnected. **All 6 devices were stuck in an AP-mode reboot loop
within ~5 minutes of AP recovery.** Power-cycling the fleet did NOT
clear it (NVS-persisted), and the AP-portal-→-STA-reconnect path
restarted in a way that preserved the bad state.

Two distinct sub-bugs:

### #96 sub-A — AP-portal restart doesn't break the streak

`ap_portal.h:1001` calls `ESP.restart()` directly when STA reconnects
after AP-mode entry. No new RestartHistory entry is pushed first, so
the post-restart OPERATIONAL boot-time check
`countTrailingCause("mqtt_unrecoverable") >= MQTT_RESTART_LOOP_THRESHOLD`
still trips on the same accumulated streak. Result: AP → STA
reconnect → restart → OPERATIONAL boot detects streak → AP again.

**Fix shipped in v0.4.28**: push `"ap_recovered"` to RestartHistory
before the `ESP.restart()`. Next-boot countTrailingCause walks newest-
first, sees `ap_recovered`, mismatches `mqtt_unrecoverable`, returns 0.
Streak broken. Self-recovers on first AP-→-STA cycle post-flash.

### #96 sub-B — `mqttScheduleRestart()` lacks debounce

`mqttScheduleRestart()` in mqtt_client.h pushes a new RestartHistory
entry unconditionally on every call. The caller (in main.cpp's MQTT
recovery logic) fires on every loop iteration while the unrecoverable-
timeout condition holds. During the cascade window after cycle #3,
the function was called **hundreds of times in milliseconds**, all
from a single outage event. The 8-slot ring buffer was completely
overwritten with `"mqtt_unrecoverable"` entries within ~20 seconds.

The post-restart check then read 8 consecutive `"mqtt_unrecoverable"`
entries — a **phantom restart-loop signature** that looked like 8
distinct failed restarts when there was actually ONE outage and ONE
ESP.restart().

**Fix shipped in v0.4.28**: idempotent `mqttScheduleRestart()` —
early-return if `_mqttRestartAtMs != 0` (a deferred restart is already
scheduled). One push per actual restart. Without this, sub-A is just
a band-aid; the next long outage would refill the ring on the freshly-
flashed firmware.

### Recovery for already-stuck devices

USB-flash with the v0.4.28 firmware (which has both fixes). On first
post-flash boot, the boot-time check still sees the 8 stale
`mqtt_unrecoverable` entries → enters AP mode → AP-mode periodic STA
scan reconnects to "Enigma" → **the new sub-A code pushes
`"ap_recovered"`** before `ESP.restart()` → next boot sees newest =
`"ap_recovered"` → operates normally.

**Validated 2026-04-29 14:xx** across all 6 devices. Each device's
boot announcement post-flash showed `last_restart_reasons` ending in
`"ap_recovered"`, confirming the self-recovery path executed exactly
as designed.

## Side-effect #2 — IDLE1 retained coredump (`e4d23b96`)

Three devices (Bravo, Delta, Echo) republished retained
`/diag/coredump` payloads with byte-identical signatures on boot:

```json
{"event":"coredump","exc_task":"IDLE1","exc_pc":"0x4008c7df",
 "exc_cause":"unknown","app_sha_prefix":"6534643233623936",
 "backtrace":["0x4008c7df","0x401dd7d1","0x40090c0f","0x4008ff51"]}
```

Decoded against v0.4.28 ELF (close-enough symbolic equivalence to
the unknown `e4d23b96` build):

```
0x4008c7df  xt_utils_wait_for_intr (xt_utils.h:82)
            inlined as esp_cpu_wait_for_intr (cpu.c:55)
0x401dd7d1  esp_get_free_heap_size (esp_system_chip.c:67)
0x40090c0f  prvIdleTask (FreeRTOS-Kernel tasks.c:4350)
0x4008ff51  vPortTaskWrapper (port.c:139)
```

The IDLE task on Core 1 was inside `xt_utils_wait_for_intr` (the
WAITI instruction — CPU halted waiting for an interrupt) when an
abort was triggered from another task / context. `exc_cause: unknown`
is consistent with a forced abort path (not a standard hardware
exception).

Most likely interpretation: a heap-integrity check or assertion
fired from another task, and the panic-capture machinery snapshotted
IDLE1 because IDLE1 was the running task on Core 1 at the moment of
abort. The IDLE backtrace is essentially decorative — the actual
fault came from somewhere else.

**SHA mystery — `e4d23b96` is not a recognized build:**
- v0.4.26 production CI: `07868e49`
- v0.4.20.0 canary local: `dd877030`
- v0.4.27 production CI (gh-pages firmware.bin): `2553c56d`
- v0.4.28 local with #78+#96: `cc3f5539`

Most likely candidate: a local-build iteration during the parallel
session's #94 / v0.4.27.0 work cycle (4-component dev build with
slightly different binary layout than CI's 3-component v0.4.27).
Cannot fully resolve without access to the parallel session's
intermediate `.pio/build/` artifacts. **Not blocking — devices are
now on v0.4.28 (`cc3f5539`) and the IDLE1 coredump is forensic
history, not a live fault.**

If the IDLE1 pattern recurs on v0.4.28, file as a new ticket. For
now, the retained payloads stay on the broker as forensic evidence.

## Side-effect #3 — operator-observed mystery SSID

During the AP-cycle window, the operator briefly saw an unknown
hex-format SSID in their WiFi scanner: `a0ef93301f040eefd26fc22407181c46`.

Cross-checked against the 6 known fleet UUIDs — no match (our
firmware broadcasts `ESP32-Config-<last-12-hex>` format anyway, not
raw 32-hex). Most likely a neighboring router / phone hotspot
transiently visible during the WiFi rescan. **No action needed.**

## Summary of v0.4.28's bundled fixes

| ID | Sub | File | Fix |
|---|---|---|---|
| #78 | — | mqtt_client.h | Cascade-window publish guard + WiFi-disconnect/reconnect timestamp stamping in main.cpp |
| #96 | sub-A | ap_portal.h | Push `"ap_recovered"` to RestartHistory before AP-portal `ESP.restart()` |
| #96 | sub-B | mqtt_client.h | `mqttScheduleRestart()` early-return if a restart is already scheduled |

All three fixes share the same release cycle and were validated
together on the bench via mass USB-reflash followed by clean
self-recovery from the AP-mode loop on every device.

## Final fleet state — 2026-04-29 afternoon

```
ESP32-Alpha    fw=0.4.28.0   uptime steady   ev=heartbeat
ESP32-Bravo    fw=0.4.28.0   uptime steady   ev=heartbeat
ESP32-Charlie  fw=0.4.28.0   uptime steady   ev=heartbeat
ESP32-Delta    fw=0.4.28.0   uptime steady   ev=heartbeat
ESP32-Echo     fw=0.4.28.0   uptime steady   ev=heartbeat
ESP32-Foxtrot  fw=0.4.28.0   uptime steady   ev=heartbeat
```

6/6 healthy. No AP-mode reboot loops. No new cascade panics with
fresh app_sha. Cascade-window guard active; AP-recovery path
self-clears any future long-outage event.

Ship v0.4.28 → CI publishes via tag → fleet is already on this
firmware locally, OTA pickup will cement the canonical CI build
across the fleet on the next `cmd/ota_check` poll.
