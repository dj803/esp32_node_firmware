# Canary OTA pattern

The canonical "deploy to one, watch, then deploy to everyone" workflow we
already use. Codifies what's been informal practice since the v0.4.06 →
v0.4.13 cascade made fleet-wide OTAs without a canary look reckless.
Tracking entry: SUGGESTED_IMPROVEMENTS #35.

## Why a canary

A canary is a single device flashed with a new build, soaked under live
traffic for hours-to-days before the rest of the fleet gets the same
build. Two failure modes it catches that bench-tests don't:

- **Slow leaks** — heap drift that only shows after ~hour-scale uptime
  (e.g. the v0.4.10 #51 bad_alloc cascade only manifested after ≥3 h on
  Alpha). Bench builds reboot too often to surface this.
- **Field-only conditions** — RF environment, Wi-Fi roaming, neighbour
  ESP-NOW chatter, real broker churn. The bench rig can simulate these
  but never quite matches the deployed reality.

If the canary survives the soak window without abnormal reboots, heap
drift, or coredump, the build is fleet-OK.

## Roles

- **Canary device** — runs the candidate build with `OTA_DISABLE` so it
  doesn't auto-pull-down to the released version. Currently:
  ESP32-Charlie on `[env:esp32dev_canary]` since 2026-04-27 (v0.4.20.0
  sticky). The `0.4.20.0` 4-component version (#80) sorts BEFORE
  released v0.4.20+, so without `OTA_DISABLE` the canary would silently
  upgrade itself on the next OTA cycle.
- **Production fleet** — every other device. Runs the latest released
  tag. Receives OTAs via `tools/dev/ota-rollout.sh` once the canary has
  passed.

The roles are decoupled: the canary can soak v0.4.20.0 indefinitely
while the fleet ships v0.4.21 / v0.4.22 / … past it. The build it tests
is not the build the fleet runs — it's the experimental / debug
variant whose stability gates the variant family (e.g. stack-canary
class checks in #54).

## Promote-to-canary checklist

1. Pick a non-production device (Charlie has been the long-running
   canary; rotate if needed). Verify it's on the bench and serial
   reachable: `Get-PnpDevice -Class Ports -PresentOnly`.
2. Build the candidate variant locally:
   `pio run -e esp32dev_canary` (or the variant under test).
3. USB-flash the canary device:
   `pio run -e esp32dev_canary -t upload --upload-port COMx`.
   Keep the `-DOTA_DISABLE` build flag — without it the device pulls
   down to released firmware on the next OTA cycle.
4. Verify-after-action (per CLAUDE.md "Verify-after-action discipline"):
   `mosquitto_sub -t '+/status' -W 60` for the boot announcement;
   confirm `firmware_version`, `boot_reason`, `restart_cause` are sane.
5. Arm `tools/silent_watcher.sh` as a Monitor task to alert on LWT
   offline + abnormal boot reasons during the soak.

## Soak-window decision rules

- **≥4 h clean uptime** required before the build can be released. Below
  4 h doesn't catch the slow-leak class (#51 manifested at ~3 h).
- **Zero abnormal boots** during the window: no `panic`, `task_wdt`,
  `int_wdt`, `brownout`. A `software` reboot with a known
  `restart_cause` is fine (cred_rotate, cmd/restart, OTA reboot).
- **No new `/diag/coredump` retained payload** for the canary device.
- **Heap trajectory bounded** — `heap_free` and `heap_largest` from the
  retained heartbeat must not be monotonically declining. Fluctuation
  ±10 KB is normal; a sustained downward slope is a leak.
- **mqtt_disconnects bounded** — single-digit count over the window.
  A spike correlates with broker / Wi-Fi events; runaway count means
  the firmware is in a reconnect-storm.

## After the canary passes — fleet rollout

```bash
tools/dev/ota-rollout.sh <version>
```

The rollout script triggers `cmd/ota_check` per device with a stagger
interval (#79), polls for the new firmware_version on each device, and
exits when all are upgraded or a per-device timeout fires (#84). For a
release that excludes the canary itself, set `EXCLUDE_UUIDS` in env to
the canary's UUID (added 2026-04-28).

## Reverting a canary that failed

1. Capture the panic context FIRST — coredump topic + `boot_reason` +
   `restart_cause` from the retained `/status`. Coredump can be decoded
   with addr2line against the worktree-built ELF for that tag (see
   #46 archive entry for the recipe).
2. Reflash the canary back to the last known-good production build:
   `pio run -e esp32dev -t upload --upload-port COMx` (omits the
   `_canary` suffix, so it picks up the released config).
3. File the failure under the relevant SUGGESTED_IMPROVEMENTS entry
   (or open a new one) before iterating on the fix.

## Anti-patterns

- **Skipping the canary because "this change is small"** — the v0.4.10
  one-line MQTT_HEALTHY hook (#51 root cause) was a small change.
- **Releasing while the canary still has uptime < 4 h** — there is no
  emergency that justifies bypassing the slow-leak window. Hot-fixes
  go through their own canary cycle on a fresh build, not a same-day
  fleet-rollout of an unsoaked variant.
- **Two canaries on different builds at once** — defeats the whole
  point. The fleet must have one definitive next-version-candidate
  under soak; running two parallel candidates means neither has a
  clean signal when the fleet-rollout decision arrives.
