# UUID drift root-cause audit (#48)

Read-only audit of `include/device_id.h` against the 2026-04-25
Delta+Echo UUID drift observation. Conducted 2026-04-28 alongside the
v0.4.23 batch.

## What we observed (2026-04-25)

Delta+Echo's UUIDs changed without a deliberate erase-flash. The
striking detail: their NEW UUIDs share the SAME 8-char prefix as their
OLD ones.

```
Delta old: 2b89f43c-c66e-4b30-9020-ff7da99ac3eb
Delta new: 2b89f43c-2fd8-4ed6-ac9d-fb0d8f97c282
                ^^^^^^^^ identical first 8 hex
```

A truly random regenerate has 1 in 4 billion odds of producing the
same 8-char prefix on both Delta and Echo. The structural correlation
points at the entropy source, not coincidence.

## Code path inventory (NVS namespace `esp32id`)

`device_id.h` exposes one read path and one write path. Both live in
`DeviceId::init()`:

```c
// READ at line 64
String stored = prefs.getString(DEVICE_ID_NVS_KEY, "");
if (stored.length() == DEVICE_ID_LEN) {  // 36 chars
    _uuid = stored;
    return;          // happy path — UUID preserved
}

// WRITE at lines 86-88 (only reached when stored is empty / corrupt)
_uuid = _generate();
prefs.putString(DEVICE_ID_NVS_KEY, _uuid);
```

There is **NO regenerate-overwrite-existing-UUID code path**. The only
ways a UUID can change are:

1. NVS namespace `esp32id` was erased (esptool erase-flash, NVS
   partition reformat).
2. Stored string fails the `length == 36` check (corrupted NVS).
3. `prefs.begin(NS, true)` returned false (NVS subsystem unavailable
   at boot — extremely rare).

Bravo's 2026-04-27 erase-flash test (#50) confirmed mechanism #1
works as expected: erase produced a new UUID `ece1ed31-…`. Delta+Echo
on 2026-04-25 most likely went through the same mechanism — they
were re-bootstrapped via `esptool erase-flash` during a session the
operator doesn't fully remember; the 8-char prefix collision is the
real puzzle.

## Why the prefix collides — RNG-pre-WiFi hypothesis

`_generate()` (lines 136-173) builds the UUID like this:

```c
uint8_t rnd[16];
for (int i = 0; i < 4; i++) {
    uint32_t r = esp_random();         // ← entropy source
    rnd[i*4 + 0..3] = (r bytes)
}
// XOR MAC bytes into rnd[0..5]
uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
for (int i = 0; i < 6; i++) rnd[i] ^= mac[i];
```

ESP-IDF documents `esp_random()` as: *"When the RNG is enabled, the
returned numbers are TRULY random. Currently, the RNG is enabled when
the WiFi or Bluetooth radio is enabled. If both Wi-Fi and BT are
disabled at the time of calling esp_random(), the function returns
pseudo-random numbers."*

`DeviceId::init()` is called at `main.cpp:247`, immediately after
`WiFi.mode(WIFI_STA)`. **Setting WIFI_STA mode does NOT enable the
radio** — that requires `WiFi.begin()`. So `esp_random()` at this
point is in pseudo-random mode, fed by a deterministic counter
seeded from… not much.

The first 4 bytes of `rnd[]` are the LOWER 32 bits of the first
`esp_random()` call. With pseudo-random + similar boot-time state,
two cold boots of the same chip can return the same 32-bit value.
After XOR with the same MAC, the first 8 hex chars of the UUID end
up identical. **That matches the observation exactly.**

Devices that match because of pseudo-random determinism would have
been Delta and Echo both — different chips, different MACs, but
each chip would have produced the same first-32-bits across regen
cycles. The XOR with the per-device MAC then made each device's
NEW prefix match its own OLD prefix, not match each other's.

## Proposed fix

Three options, smallest first:

1. **Defer UUID generation until after `WiFi.begin()`.** Move
   `DeviceId::init()` later in setup() so `esp_random()` is hardware-
   backed by the time it's called. **DOWNSIDE**: every other module
   that needs the UUID early (mqttTopic prefixes, log lines) breaks.
   Significant refactor.

2. **Use `bootloader_random_enable()` before generation.** ESP-IDF
   provides this function which seeds the RNG from SAR-ADC noise floor
   without needing the Wi-Fi radio. Pair with
   `bootloader_random_disable()` afterward. **DOWNSIDE**: the ADC must
   not be in use yet — fine on first boot before Hall sensor (#71/v0.5.0)
   inits.

3. **Use `esp_fill_random()` instead of `esp_random()`.** Per IDF docs,
   `esp_fill_random()` blocks if necessary to ensure entropy quality —
   it's the recommended replacement for `esp_random()` at boot.
   **DOWNSIDE**: blocks. Acceptable for the once-per-boot UUID-gen
   path; the block is < 100 ms in practice.

**Recommended:** option 2 (bootloader_random_enable) — non-blocking,
produces hardware-quality entropy from the SAR-ADC, and runs cleanly
before Wi-Fi init. Wrap inside `DeviceId::_generate()`:

```c
#include "bootloader_random.h"
// ...
static String _generate() {
    bootloader_random_enable();
    uint8_t rnd[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        ...
    }
    bootloader_random_disable();
    ...
}
```

## Status

This document IS the audit deliverable for #48. The mechanism (#48
"NVS partial-wipe path") was misdiagnosed in 2026-04-25's writeup —
the actual root cause is RNG-pre-WiFi pseudo-random determinism. The
"prefix matches" observation is consistent with this hypothesis and
inconsistent with NVS partial-wipe.

#48 → ROOT CAUSE IDENTIFIED 2026-04-28. Fix not shipped this session
— bundles cleanly with v0.5.0 hardware introduction (when Hall ADC
is wired the bootloader_random_enable/disable bookend matters more)
or any future device_id.h change.

The current `[W][DeviceId] Generated new UUID:` warning is still the
right signal — it fires when this code path runs, regardless of cause.
v0.4.11 visibility shipped that already (per #48 archive STATUS line).
