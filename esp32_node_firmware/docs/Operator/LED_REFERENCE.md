ESP32 Node Firmware — LED reference (firmware-driven patterns)
================================================================
Last updated: 2026-04-29 (v0.4.31)


Onboard status LED (GPIO 2)
---------------------------
Source: include/led.h::LedPattern
Set by firmware state machine. Never driven by MQTT — the patterns
below are exactly what the device emits based on its current state.

  Pattern             Waveform                              Trigger
  -------             --------                              -------
  BOOT                solid ON                              Early init / pre-WiFi
  WIFI_CONNECTING     200 ms ON / 200 ms OFF (2.5 Hz)       WiFi disconnected; backoff retry active
                                                            (alarm: cannot reach WiFi)
  WIFI_CONNECTED      500 ms ON / 500 ms OFF (1 Hz)         WiFi up, MQTT not yet connected
  MQTT_CONNECTED      1900 ms ON / 100 ms OFF               MQTT subscribe success — operational
                      (mostly-on with brief blip)            (steady glow: I'm healthy)
  AP_MODE             50/50/50/850 ms                       AP captive portal active
                      (double-blink-pause)                   (operator come configure me)
  OTA_UPDATE          solid ON                              Flash write in progress
  ERROR               3 × 100 ms flash + 700 ms pause       Unrecoverable state; restart imminent
  ESPNOW_FLASH        40 ms ON overlay then revert          Brief ESP-NOW packet TX/RX indicator
  LOCATE              10 × 200 ms (4 s) then revert         From cmd/locate — physical-locate flash
  OFF                 dark                                  Unused state


States the WS2812 strip settles into between events
---------------------------------------------------
Source: include/ws2812.h::LedState
The WS2812 strip has its own state machine. Between events (RFID
read, OTA, MQTT connect/disconnect, etc.) it settles into one of
these resting states:

  State              Pattern                Meaning
  -----              -------                -------
  BOOT_INDICATOR     Colour-coded by phase  Boot phase indicator
                                            (bootstrap / wifi / ap_mode)
  IDLE               Slow blue breathing    Default resting state
                                            (WiFi up, MQTT not yet connected
                                             OR MQTT recently dropped)
  MQTT_HEALTHY       Slow green breathing   WiFi + MQTT both connected
                                            (operational heartbeat — visible
                                             on Alpha when fleet is healthy)
  OTA                Orange chasing         Mid-OTA download
  RFID_OK            Solid green (~2 s)     Authorised card UID match
  RFID_FAIL          Solid red (~2 s)       Unauthorised card
  MQTT_OVERRIDE      Per cmd/led payload    Operator-driven via MQTT
  MQTT_PIXELS        Per-pixel direct       Operator-driven per-pixel mode
  SELF_TEST          RGBW walk + chase      Installation diagnostic (~6 s)
  OFF                Dark                   All LEDs off


Visual quick-reference (most → least alarming)
-----------------------------------------------
The most useful at-a-glance state is the onboard GPIO 2 status LED:

  FAST blinking (5 Hz)        →  WiFi-stuck — alarm
  MEDIUM blinking (1 Hz)      →  WiFi up, MQTT down
  STEADY GLOW with brief blip →  Healthy and operational
  DOUBLE-BLINK + pause        →  AP-mode portal active
  3 flashes + pause           →  Unrecoverable; restarting
  Solid ON                    →  Either booting OR mid-OTA flash

If you have a WS2812 strip on the device, that adds:

  Slow GREEN breathing        →  MQTT_HEALTHY operational
  Slow BLUE breathing         →  IDLE (WiFi-only or MQTT-just-dropped)
  ORANGE chasing              →  OTA download in flight
  GREEN solid (~2 s)          →  RFID accepted
  RED solid (~2 s)            →  RFID rejected


History
-------
v0.4.31 (2026-04-29) — Onboard LED waveforms retuned for visual
                       distinctiveness across the bench (#99). Old
                       patterns were time-correct but too similar
                       visually; the 4-stuck-2-healthy fleet from
                       the afternoon's #98 incident looked identical
                       in heartbeat blinking.
v0.4.27 (2026-04-29) — WS2812 LedEventType::MQTT_LOST added (#94)
                       so dropping MQTT while WiFi remains up reverts
                       the strip to IDLE blue breathing.
v0.4.26 (2026-04-28) — WS2812 cmd/led MQTT command suite shipped
                       (#19/#20/#21/#22/#23). See LED_COMMANDS.md
                       for the operator-driven side.
v0.4.13 (2026-04-27) — MQTT_HEALTHY green breathing pattern
                       introduced via deferred-flag (#56) to avoid
                       the v0.4.10 TWDT crash shape.
