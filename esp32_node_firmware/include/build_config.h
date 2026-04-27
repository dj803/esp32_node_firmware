#pragma once

// Build-time feature flags. Toggle via `build_flags` in platformio.ini
// (see [env:esp32dev], [env:esp32dev_ble_bench], [env:esp32dev_canary]).
// This header centralises the flag namespace so docs/TECHNICAL_SPEC.md §10.1
// has a real target to reference.
//
// Flags are commented out — uncomment a line, or define on the build_flags
// command line, to opt in.
//
//   ENABLE_SERIAL_DEBUG  — verbose Serial.print throughout the boot path
//                          and MQTT/BLE/ESPNOW state machines.
//   SKIP_BOOTSTRAP_WAIT  — bypass the ESP-NOW bootstrap response window
//                          (development-only; never ship).

// #define ENABLE_SERIAL_DEBUG
// #define SKIP_BOOTSTRAP_WAIT
