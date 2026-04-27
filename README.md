# NodeFirmware

ESP32-based bootstrap-and-OTA firmware for the JHB office sensor fleet.
Each node self-discovers its broker, exchanges credentials with siblings
via ESP-NOW, and pulls firmware updates from a manifest hosted on GitHub
Pages.

## Layout

- [`esp32_node_firmware/`](esp32_node_firmware/) — firmware source tree
  (PlatformIO project; Arduino-ESP32 3.x). Open VS Code at this folder so
  the IDE picks up `platformio.ini`.
- [`esp32_node_firmware/docs/`](esp32_node_firmware/docs/) — design docs,
  failure-mode investigations, audit trails, suggested-improvements
  backlog.
- [`racetrack/`](racetrack/) — scratchpad.

## Pointers

- [`CLAUDE.md`](CLAUDE.md) — repo and fleet ground truth: build commands,
  COM-port verification, MQTT topology, diagnostic process.
- OTA manifest: <https://dj803.github.io/esp32_node_firmware/ota.json>
  (served from the `gh-pages` branch).
- GitHub: <https://github.com/dj803/esp32_node_firmware>. Tags `v*.*.*`
  trigger CI release builds.

All rights reserved. See [LICENSE](LICENSE).
