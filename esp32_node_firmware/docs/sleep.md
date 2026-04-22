# Sleep Control Reference

Single source of truth for the three MQTT-triggered power-save modes
exposed by the ESP32 node firmware from v0.3.20 onward. Firmware
(`include/mqtt_client.h`, `include/config.h`, `include/fwevent.h`) and
the Node-RED **Sleep Control** group on the Device Status tab both
follow this document — update here first when the wire contract
changes.

---

## Three sleep modes

| Mode | MQTT command | CPU | Radio | MQTT session | Approx. current | `setup()` re-runs? | In-RAM state preserved? |
|---|---|---|---|---|---|---|---|
| **Modem sleep** | `cmd/modem_sleep` | Runs (240 MHz) | Associated, sleeps between DTIM beacons | **Stays connected** | ~60–70 mA (vs ~80–100 mA) | No | Yes |
| **Light sleep** | `cmd/sleep`      | Halted | Torn down (radio off) | Disconnected | ~1–5 mA | No | Yes |
| **Deep sleep**  | `cmd/deep_sleep` | Halted | Torn down (radio off) | Disconnected | ~10–150 µA | **Yes (cold boot)** | No (RAM wiped) |

Pick based on what you want:

- **Still responsive, just cheaper to run** → modem sleep. Device keeps
  heartbeating, RFID still polls, ESP-NOW still ranges, Node-RED can
  still issue commands (including `cmd/locate`, which works during
  modem sleep because the radio is still up).
- **Off-network for a known interval, but want it back fast** → light
  sleep. Wake is ~1 s faster than deep sleep because peripheral init
  (RFID, LED, ESP-NOW) does not re-run.
- **Off-network and lowest possible current** → deep sleep. Use for
  long idle periods (hours). Cold boot on wake — expect ~5–10 s to
  reconnect and re-subscribe.

---

## Command schema

All three commands share the same payload shape. Topic is under the
device's ISA-95 path:
`Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/cmd/<mode>`.

```jsonc
// seconds: integer, MIN_SLEEP_SECONDS (1) ≤ seconds ≤ MAX_SLEEP_SECONDS (86400).
// Out-of-range or malformed payloads are rejected (warning logged, no state change).
{"seconds": 30}
```

| Field | Type | Required | Range |
|---|---|---|---|
| `seconds` | integer | yes | 1 … 86400 (24 h cap) |

QoS:
- `cmd/sleep`, `cmd/deep_sleep` — QoS 0 (the radio will be offline when
  the would-be PUBACK flight completes, so QoS 1 adds no value and
  risks re-delivery on reconnect).
- `cmd/modem_sleep` — QoS 1 (session stays alive).

Retain:
- All three are **non-retained**. A retained sleep command would
  re-arm every node that reconnects after the fact.

---

## Wake behavior

### Modem sleep

After `duration_s` seconds, `mqttHeartbeat()` restores `WIFI_PS_NONE`
and publishes a fresh `heartbeat` event. Node-RED detects this and
flips the card back to Connected.

Note: modem sleep does **not** drop the CPU frequency. The dynamic
frequency-scaling API (`setCpuFrequencyMhz`) pulls in the ESP-IDF
`esp_pm` driver (~1 KB IRAM) which pushes the firmware over the
`iram0_0_seg` budget. Power saving from the radio power-save alone
is ~20–40 mA. If deeper savings are needed, revisit with a custom
`sdkconfig` that grows IRAM.

### Light sleep

The firmware publishes `event:"sleeping"`, drains the publish for
~200 ms, cleanly disconnects MQTT, tears down Wi-Fi, and calls
`esp_light_sleep_start()`. When the RTC timer fires, control returns
inside `mqttEnterSleep()`, which re-enables Wi-Fi STA and calls
`WiFi.begin()` with the cached bundle. `loop()` resumes normally; the
existing MQTT reconnect timer re-establishes the broker session
within a few seconds. No `boot` event is published because `setup()`
did not re-run — the next status message is a normal `heartbeat`.

### Deep sleep

`esp_deep_sleep_start()` never returns. RTC-timer wake triggers a
full cold boot: power-on self-test, `setup()`, bootstrap/operational
state machine. The first status message after wake is a retained
`boot` event, identical to a normal power-on.

---

## Status publish contract

At sleep entry, the firmware publishes to the device's `.../status`
topic (QoS 1, **non-retained**):

```json
{
  "device_id": "…",
  "mac": "…",
  "firmware_version": "0.3.20",
  "uptime_s": 1234,
  "rfid_enabled": true,
  "event": "sleeping",
  "duration_s": 30
}
```

`event` ∈ `sleeping` | `deep_sleeping` | `modem_sleeping`. Node-RED
computes `wake_at_ms = receivedAt + duration_s * 1000` and displays a
live countdown on the device card.

---

## Known limitations

1. **MQTT messages lost while radio is off.** AsyncMqttClient does not
   persist session state across `disconnect(true)`. Any publish to
   this device while it is in `sleep` or `deep_sleep` is lost — the
   broker has no subscription to buffer against. Modem sleep is
   unaffected (session stays alive).
2. **No wake-on-MQTT or wake-on-GPIO.** Only RTC timer wake. Absolute
   wall-clock wake times are not supported (no NTP on the device).
3. **No persisted sleep state across power loss.** If USB power is
   yanked during `deep_sleep`, the Node-RED countdown is stale until
   the device reconnects and publishes `boot`. Handled implicitly via
   the heartbeat-timeout → Disconnected transition.
4. **24 h ceiling.** `MAX_SLEEP_SECONDS = 86400`. For longer idle
   periods, have the Node-RED dashboard re-arm on the next wake.

---

## Integration with existing heartbeat timeout

Node-RED's Detect Heartbeat timer normally flips a card to
`Disconnected` after 90 s of silence. For `sleeping` and
`deep_sleeping` events the UI installs a longer per-sleep timer —
`duration_s + 30 s` (light) or `duration_s + 60 s` (deep) grace — so
an expected sleep does not alarm as a disconnect.

`modem_sleeping` does **not** alter the timer, because heartbeats
continue throughout.

---

## Related

- `include/mqtt_client.h` — `mqttEnterSleep()`, `mqttEnterModemSleep()`,
  `mqttExitModemSleep()`, and the three command handlers.
- `include/fwevent.h` — `FwEvent::SLEEPING`, `DEEP_SLEEPING`,
  `MODEM_SLEEPING`.
- `include/config.h` — `MIN_SLEEP_SECONDS`, `MAX_SLEEP_SECONDS`,
  `SLEEP_DEFER_MS`.
- **Node-RED Sleep Control group** on the Device Status tab —
  device picker + duration + unit + three mode buttons.
