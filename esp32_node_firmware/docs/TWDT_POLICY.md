# Task Watchdog (TWDT) policy

> Per-task subscription model for the ESP32 firmware. Written 2026-04-23
> as part of v0.4.03 #29 audit (SUGGESTED_IMPROVEMENTS). Documents which
> tasks are subscribed to the IDF Task Watchdog Timer, which sites must
> feed it, and which are safe by construction.

## Background

The ESP32's Task Watchdog Timer (`esp_task_wdt`) bites if a *subscribed*
task doesn't call `esp_task_wdt_reset()` within `CONFIG_ESP_TASK_WDT
_TIMEOUT_S` (default 5 s). Tasks NOT subscribed to TWDT are never
checked — `esp_task_wdt_reset()` from an unsubscribed task is a silent
no-op (older arduino-esp32) or an `[E]` log line per call (arduino-esp32
3.x — the surprise that bit us in v0.3.27).

The Interrupt Watchdog Timer (`int_wdt`) is separate: ~300 ms; bites
when interrupts are disabled or starved for too long. Cannot be fed by
user code; only avoided by keeping ISR / critical-section durations
short. Trips show up as `boot_reason=int_wdt` in retained status.

## Subscribed tasks (current state, v0.4.03)

| Task | Subscribed | When | Feed responsibility |
|---|---|---|---|
| `IDLE0` (Core 0 idle) | ✅ Always | IDF default | Never user-touched. Bites if Core 0 is starved (lwIP / WiFi / async_tcp loop forever). |
| `IDLE1` (Core 1 idle) | ✅ Always | IDF default | Same, Core 1. |
| `loopTask` (Arduino main task) | ⚠️ Conditionally | Subscribed by **us** in `ota.h` (around the OTA download) and in `ap_portal.h::_generateTlsCreds` (around mbedTLS RSA keygen) | We feed via `esp_task_wdt_reset()` at every progress callback / between long mbedTLS steps. |
| `async_tcp` | ✅ Always (by AsyncTCP library) | Library default | We **unsubscribe** it before the OTA download (`esp_task_wdt_delete(asyncTcpTask)`) so the saturated TLS download doesn't trip it. Library does not need feeding under normal load — events are short-lived. |
| `nimble_host` | ✅ Always (by NimBLE) | Library default | We do nothing inside the `onResult` callback that would block long enough to need a feed. The callback takes `_bleMutex` with timeout=0 (drop on contention). |
| All other FreeRTOS user tasks | ❌ Not subscribed | n/a | None |

## Safe-by-construction sites (no feed needed)

These sites are reached on tasks NOT subscribed to TWDT, OR they yield
frequently enough that all subscribed tasks stay fed:

- **`broker_discovery.h` port scan loop.** Runs during `setup()` before
  loopTask becomes a long-running subscriber. Iterates with
  `delay(5)` per outer step, yielding to the FreeRTOS scheduler so any
  subscribed task stays fed.
- **`broker_discovery.h` mDNS query.** Synchronous lwIP call; runs only
  during setup; bounded by `MDNS_QUERY_TIMEOUT_MS`.
- **`ble.h` `_BleScanCb::onResult`.** Runs on `nimble_host` task. Takes
  `_bleMutex` with `timeout = 0` (drops if contended) and finishes within
  ~200 µs (one JSON struct parse + memcpy). Cannot block.
- **`rfid.h` SPI transactions in `rfidLoop`.** SPI bus is point-to-point
  to MFRC522; transactions complete in ~ms. Loop iterations remain well
  under 1 s. loopTask is not subscribed during normal operation.
- **`mqtt_client.h::onMqttMessage` handlers.** AsyncMqttClient invokes
  these on the lwIP / async_tcp task. Handlers are short (parse + flag
  set + queue post). Long-running effects are deferred to `loop()` via
  `_mqtt*AtMs` deadlines (the v0.3.20 sleep pattern).
- **`mqtt_client.h::onMqttConnect / onMqttDisconnect`.** Same async_tcp
  task. Subscribe + publish-status calls finish in ~ms; the WS2812
  MQTT_HEALTHY post is deferred to `mqttHeartbeat()` via
  `_mqttLedHealthyAtMs` (the v0.4.13 deferred-flag pattern — see
  dedicated section below). #44 root cause was an inline post from
  this callback.
- **All FreeRTOS software-timer callbacks** (e.g. `_otaProgressTimeout`,
  `_mqttReconnectTimer`). Run on the timer service task with a default
  small stack; we keep them under a few ms.

## Sites that DO feed (and why)

- **`ota.h` — entire OTA download path.** loopTask is explicitly
  subscribed before the download begins (`esp_task_wdt_add(NULL)`) and
  fed at every progress callback (~per chunk). Without this, the
  blocking HTTPS download would trip TWDT in ~3 s. This is the single
  most expensive blocking call in the firmware.
- **`ap_portal.h::_generateTlsCreds` — mbedTLS RSA-2048 keygen.**
  Keygen blocks ~10–15 s. loopTask is subscribed at function entry
  (idempotent — `ESP_ERR_INVALID_ARG` if already subscribed). Reset
  is called between every long mbedTLS step.

## Deferred-flag pattern — callback wants long work

The canonical answer to rule #2 below ("callbacks must stay short; defer
long work via flags / queues"). Worked example added in v0.4.13 for #44
re-enabling the green MQTT_HEALTHY LED.

**Problem.** `onMqttConnect()` runs on the `async_tcp` task (Core 0,
TWDT-subscribed by AsyncTCP). Posting a `LedEvent` to the WS2812 task
queue from inside the callback works MOST of the time, but under
contention (FastLED render in flight, queue full, mutex blocked) the
post stalls long enough that `async_tcp` skips its TWDT feed. That was
the v0.4.10 fleet-wide crash shape (#51 → diagnostic root-cause
analysis 2026-04-27).

**Anti-pattern (v0.4.10):**
```cpp
static void onMqttConnect(bool sessionPresent) {
    // … subscribe topics, publish boot announcement …
    LedEvent e{};
    e.type = LedEventType::MQTT_HEALTHY;
    ws2812PostEvent(e);          // ❌ post from async_tcp callback
}
```

**Pattern (v0.4.13):**
```cpp
// Module state — volatile because writer (callback) and reader (loop) are
// on different tasks; without volatile the compiler may cache reads in
// loop() across the task-switch boundary and the deferred work never fires.
static volatile uint32_t _mqttLedHealthyAtMs = 0;

static void onMqttConnect(bool sessionPresent) {
    // … subscribe topics, publish boot announcement …
    _mqttLedHealthyAtMs = millis();     // ✅ flag only — returns immediately
}

// Consumed in mqttHeartbeat(), called every loop() iteration on loopTask
// (NOT TWDT-subscribed during normal operation; safe for blocking posts).
void mqttHeartbeat() {
    if (_mqttLedHealthyAtMs > 0) {
        _mqttLedHealthyAtMs = 0;
        LedEvent e{};
        e.type = LedEventType::MQTT_HEALTHY;
        ws2812PostEvent(e);
    }
    // … rest of heartbeat …
}
```

**Checklist when applying:**
1. Storage: module-static `volatile` integer/bool. The writer is the
   callback, the reader is `loop()` — different tasks, possibly different
   cores; `volatile` blocks register-caching across the task switch.
2. Writer: callback assigns once and returns. Use a non-zero sentinel
   (e.g. `millis()` or `1`) so the reader can distinguish "armed" from
   "idle". Don't gate on equality with a stale `millis()` value.
3. Reader: must be called from a TWDT-safe context. `loop()` and any
   function it calls during normal operation qualifies; a software-timer
   callback usually does NOT.
4. Idempotency: clear the flag BEFORE the deferred work runs, so a
   second arming during the work doesn't get coalesced away.
5. Bounded latency: the flag is consumed at most one `loop()` iteration
   late — typically < 1 ms. If the work is time-critical (e.g. you need
   < 100 µs response), this pattern is wrong; use a queue + dedicated
   task instead.

**When NOT to use this:** for work that itself blocks > 5 s on
TWDT-subscribed `loopTask` paths (OTA download, mbedTLS keygen). Those
need explicit `esp_task_wdt_add(NULL)` + per-step `esp_task_wdt_reset()`
as documented in "Sites that DO feed" above.

## Future-proofing rules

When adding new code, ask:

1. **Is the calling task subscribed to TWDT?** If yes (loopTask in OTA /
   keygen contexts; or any task you explicitly subscribed), every blocking
   call must complete in < `CONFIG_ESP_TASK_WDT_TIMEOUT_S` OR you must
   feed via `esp_task_wdt_reset()` between sub-steps.
2. **Is this a callback** (MQTT, NimBLE, AsyncTCP, FreeRTOS timer)?
   Callbacks run on tasks **the library subscribed**, not our code.
   Keep callbacks short (≤ a few ms). Defer long work via flags / queues.
3. **Are you adding a new long-running task?** Either subscribe it to
   TWDT and feed it appropriately, OR document why it's intentionally
   unsubscribed.
4. **Are you adding `esp_task_wdt_reset()` to a function** that might be
   called from setup() AND from loop()? Add `esp_task_wdt_add(NULL)` at
   the top with `ESP_ERR_INVALID_ARG`-as-benign idiom (see
   `ap_portal.h::_generateTlsCreds`). Otherwise the resets are silent no-ops
   from setup() but real feeds from loop() — inconsistent safety model
   and the v0.3.27 / v0.4.01 footgun.

## Lessons from v0.1.x → v0.3.x

The watchdog cluster (Cluster 1 in `SUGGESTED_IMPROVEMENTS.md` v0.3.xx
hardening) bit five different versions:

- **v0.1.5–v0.1.7**: TWDT fired during OTA download. Fixed by
  `esp_task_wdt_delete(async_tcp)`.
- **v0.3.13**: Same fix incomplete on failure paths (async_tcp ran
  unmonitored for the rest of the session).
- **v0.3.23**: TWDT fired again, different shape — added per-chunk feed.
- **v0.3.25**: Stack overflow because deeper IDF call chain. Bumped
  loopTask stack to 16 KB.
- **v0.3.27**: v0.3.23's per-chunk feed printed `[E]` log per call
  because loopTask wasn't subscribed → cumulative log spam → another
  stack overflow at ~60 % progress. Fixed by pre-subscribing loopTask.
- **v0.3.32**: Stalls that didn't trip TWDT (CDN black-hole). Added
  separate progress watchdog (FreeRTOS one-shot timer).
- **v0.4.10**: Fleet-wide crash from `ws2812PostEvent()` called inside
  `onMqttConnect/onMqttDisconnect` (async_tcp task). Workaround: disabled
  the post in v0.4.10.1. Real fix in v0.4.13 — the deferred-flag pattern
  documented above. #44 / #51.

The shape: every fix added a new symptom. Documenting the policy here
is the structural alternative — future contributors know which task is
subscribed and what the feed responsibility is, instead of finding out
when production deploys panic.
