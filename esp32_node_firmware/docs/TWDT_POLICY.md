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

The watchdog cluster (Cluster 1 in `SUGGESTED_IMPROVEMENTS.txt` v0.3.xx
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

The shape: every fix added a new symptom. Documenting the policy here
is the structural alternative — future contributors know which task is
subscribed and what the feed responsibility is, instead of finding out
when production deploys panic.
