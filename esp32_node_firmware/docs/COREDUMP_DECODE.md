# Coredump backtrace decode — runbook

When `/diag/coredump` shows a panic on a fleet device, decode the
backtrace with `xtensa-esp32-elf-addr2line` against a matching ELF
to turn raw addresses into source lines. This recipe was used twice
on 2026-04-28 — once for #46 (Alpha v0.4.20 bad_alloc cascade) and
once for #78 (full-fleet async_tcp coredump decode). Codifying so
the next operator doesn't have to reverse-engineer it from archive
entries.

## When to run

Trigger any time `/fleet-status` or `silent_watcher.sh` surfaces a
new retained `/diag/coredump` payload. The payload looks like:

```json
{
  "event": "coredump",
  "exc_task": "async_tcp",
  "exc_pc": "0x40122dab",
  "exc_cause": "StoreProhibited",
  "core_dump_version": 258,
  "app_sha_prefix": "3836653339336431",
  "backtrace": ["0x40122dab", "0x400e1d24", "0x400e1feb", "0x4008ff31"]
}
```

The decode recovers function name + file + line for each PC in
`backtrace`. That tells you which task crashed where, which
caller-frame chain led there, and (often) what to fix.

## Capture the coredump

```bash
mosquitto_sub -h 192.168.10.30 \
  -t 'Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/+/diag/coredump' \
  -v -W 25 2>&1 | head -40
```

`-W 25` ends after 25 s. The `-R` flag (skip retained) is NOT used —
we want the existing retained payload. Each device has at most one
retained coredump (the most recent panic).

`app_sha_prefix` is hex-encoded ASCII. Decode with
`echo "<hex>" | xxd -r -p` or in Python:

```python
bytes.fromhex("3836653339336431").decode()  # → "86e3993d1"
```

That ASCII is the SHA-256 prefix of the firmware.bin that was
running when the panic fired. Different devices may show different
prefixes (different firmware versions), so each panic needs the
matching ELF — not just any nearby version.

## Get a matching ELF

The CI release artefact at GitHub only contains `firmware.bin`, not
`firmware.elf`. Build the ELF from source at the right tag:

```bash
# Pick the right tag — match app_sha_prefix to firmware_version
# from the device's boot announcement or heartbeat.
git worktree add /c/Users/drowa/v04-XX-decode v0.4.XX
cd /c/Users/drowa/v04-XX-decode/esp32_node_firmware
pio run -e esp32dev
# ELF lands at .pio/build/esp32dev/firmware.elf
```

If the worktree from a previous decode session is still present
(check `/c/Users/drowa/v04-*-decode/`), you can re-use it. The ELF
doesn't change unless you re-run `pio run` against modified source.

**Verify the SHA matches.** Take the sha256 of the .pio
firmware.bin, prefix-compare against the device's app_sha_prefix:

```bash
sha256sum .pio/build/esp32dev/firmware.bin | cut -c1-8
# Compare first 8 chars to: echo <app_sha_prefix> | xxd -r -p
```

If they don't match, the ELF won't decode reliably — addresses may
land in wrong functions because of link-order drift. Frames in
stable libraries (AsyncTCP, ESP-IDF) decode close-to-right even on
a mismatched ELF; frames in our app code can be off by a function.
Note the mismatch in any analysis you write up.

## Decode

```bash
ADDR2LINE='/c/Users/drowa/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line.exe'
ELF=/c/Users/drowa/v04-XX-decode/esp32_node_firmware/.pio/build/esp32dev/firmware.elf

"$ADDR2LINE" -pfiaC -e "$ELF" \
  0x4008ec14 0x4008ebd9 0x400954ad 0x401ae04b \
  0x401ae080 0x401ae15f 0x401ae1f2 0x400e4b0d
```

Flags:
- `-p` — pretty-print, one line per address
- `-f` — show function names
- `-i` — show inlined frames (essential for AsyncTCP, where
  `_handle_async_event` and `_async_service_task` are inlined into
  each other)
- `-a` — show the address alongside the symbol
- `-C` — demangle C++ names

The first PC in the `backtrace` array is the exception PC (where
the fault fired). Subsequent PCs are caller frames. The last PC
should be `vPortTaskWrapper` (FreeRTOS task entry).

## Interpreting common results

### Valid PC + sane backtrace (e.g. #46 bad_alloc)

```
0x4008ec14: panic_abort at panic.c:477
0x401ae15f: __cxa_throw at eh_throw.cc:98
0x401ae1f2: operator new at new_op.cc:54
0x400e4b0d: AsyncMqttClient::publish at AsyncMqttClient.cpp:742
0x400ee19e: mqttPublish at mqtt_client.h:230
0x400f8bc2: espnowRangingLoop at espnow_ranging.h:650
...
```

Read top-to-bottom: panic_abort is the IDF crash handler, then the
library call chain that invoked it. The actionable site is usually
the highest frame in our code — here `mqttPublish` at
`mqtt_client.h:230`, the topic-build path that exhausted the heap.

### PC in non-text memory

`exc_pc=0x01420004` (invalid range) or `0x3f409271` (DRAM range
0x3F400000-0x3FFFFFFF). The CPU was executing data, not code.
Diagnosis: corrupted function-pointer dispatch — a vtable, callback
slot, or task-switch context got overwritten before the indirect
call/jump. Typical causes:

- Stack overflow → `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` canary
  build catches it ([env:esp32dev_canary])
- Use-after-free of a struct that owns the function pointer
- Heap overrun corrupting an adjacent struct's vtable

The single-frame Charlie 0x3f409271 case from #78 is a classic:
no caller frame because the trampoline that loaded the bad pointer
was already long gone.

### Backtrace mostly in lwIP / async_tcp

```
0x40122123: raw_netif_ip_addr_changed at lwip/raw.c:667
0x400e1d08: AsyncServer::_accepted at AsyncTCP.cpp:1633
0x400e1fcf: AsyncClient::_s_fin at AsyncTCP.cpp:1491
0x4008ff31: vPortTaskWrapper
```

The lwIP function at `exc_pc` is often a **red herring** — addr2line
lands there because the corrupted code-pointer happened to point
inside that function's bytes. The real bug is usually one frame up,
in AsyncTCP's event-dispatch path. See #78 archive entry for the
full pattern (5/5 fleet panics in `_async_service_task`,
3 distinct entry handlers, multiple fault PCs converging on
"corrupted dispatch").

## Recording findings

Put the decode in the SUGGESTED_IMPROVEMENTS_ARCHIVE entry for the
relevant tracking item, not in chat scrollback. The next operator
should be able to grep the archive for `addr2line` and find the
worked examples (#46, #78). Include:

- Capture date + time
- Each device's app_sha_prefix + which release it maps to (if known)
- Whether the ELF SHA matched the device's app_sha_prefix
- Full decoded backtrace (don't summarise — the lines matter)
- Pattern observation across multiple devices if applicable
- Hypothesis update with what's strengthened / weakened / ruled out

## Anti-patterns

- **Decoding against an arbitrary ELF.** Addresses don't translate
  cross-version reliably. Pick the closest tag whose SHA prefix
  matches, or note the mismatch explicitly.
- **Skipping `-i` (inlined frames).** AsyncTCP code in particular
  inlines `_handle_async_event` into `_async_service_task` and the
  per-handler dispatch. Without `-i` the backtrace looks 2-3 lines
  shorter than the real call chain.
- **Trusting the function name at `exc_pc` for crashes with PCs in
  non-text memory.** Cross-check exc_cause: `IllegalInstruction` /
  `InstructionFetchError` / `LoadStoreAlignment` against a PC that's
  in valid text — if both sides agree (valid PC + sane cause) the
  decode is real; if not, you're reading random bytes through the
  symbol table.
- **Skipping the worktree-build step because "we already have an ELF
  somewhere."** Verify the SHA. Past sessions have lost hours
  decoding against the wrong ELF — see #46 archive's "IllegalInstruction
  was wrong" correction for the cost.
