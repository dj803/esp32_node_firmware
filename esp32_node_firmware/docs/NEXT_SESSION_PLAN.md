# Next session plan

Drafted 2026-04-28 evening (post-PM-soak wrap-up). Today shipped three
releases in the morning (v0.4.24 / v0.4.25 / v0.4.26), then the
afternoon was a no-release backlog-cleanup + 3 h observation soak.

## State at end of session (2026-04-28 ~17:50 SAST)

| | |
|---|---|
| Master HEAD | `chore(memory): record afternoon backlog cleanup + soak window` (TBD) |
| Fleet | 5/5 production on **v0.4.26** (Alpha, Bravo, Delta, Echo, Foxtrot); **Charlie** sticky on v0.4.20.0 canary, 19 h uptime |
| Soak | 3 h observed clean: zero abnormal boots since 14:46 baseline, zero new coredumps, zero mqtt_disconnects increments. Heap fragmentation reaches stable steady-state at 40-43 KB largest contiguous (well above v0.4.22's 8 KB publish threshold). |
| Backlog | OPEN **20**, RESOLVED **53**, WONT_DO **11** (down from 29/47/9 at session start) |
| One incident | Alpha brownout 14:44:58 SAST, recovered clean. Almost certainly USB-hub current limit on the LED rig (8× WS2812 @ full white draws ~480 mA + ESP32 baseline). NOT firmware. Logged in #46 archive — second one would escalate to a hardware sub-item. |

## Bench state (unchanged from earlier today)

- **Alpha on COM4** — WS2812-equipped, serial-accessible. Just rebooted at 14:44 (brownout); 3 h+ clean uptime since.
- **Charlie on COM5** — sticky canary on v0.4.20.0 (do not disturb — preserves #78 forensics). Memory says 19 h+, no new panics.
- **Bravo off-bench** — operator will re-attach for v0.5.0 relay + Hall hardware bring-up.

## Soak windows still ticking

- **#46 closure** — needs ≥24 h fleet-soak on v0.4.22+ with zero abnormal-bad_alloc panics. Started 14:46 SAST 2026-04-28; completes ~14:46 SAST 2026-04-29. 3 h logged so far, all clean.
- **#76 fleet-validation closure** — same window. All sub-items (A-I) code-shipped; just waits on the soak.
- **#78 AsyncTCP race** — separate question from #46. The 5 retained `/diag/coredump` payloads decoded this session are pre-v0.4.22-fix. To distinguish "v0.4.22 obviated the race" vs "still latent": **clear retained coredumps** and watch the 24 h window for new ones.
  - Operator command (intentionally NOT auto-run this session — deletes diagnostic data on shared broker):
    ```bash
    for uuid in 32925666-... ece1ed31-... 2fdd4112-... 2ff9ddcf-... c1278367-... ; do
      mosquitto_pub -h 192.168.10.30 \
        -t "Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/$uuid/diag/coredump" -n -r
    done
    ```
  - Resolve UUIDs from live MQTT first (CLAUDE.md "Diagnostic process") rather than hardcoding from this doc.

## Recommended next session — A/B/C menu

### A. v0.5.0 hardware bring-up (relay + Hall on Bravo) — ~2 h

Still the natural next big milestone, blocked only on operator
re-attaching Bravo. Plan unchanged: see
[PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md).

Pre-session operator action:
1. Re-attach Bravo to the bench (will likely reassign COM4 ↔ Alpha — note the COM port verification recipe in CLAUDE.md before flashing).
2. Wire relay (GPIO 25/26) + Hall (GPIO 32/33) per PLAN_RELAY_HALL.

Steps once Bravo is wired:
1. Capture Bravo's pre-flash state (firmware, uptime, boot_reason).
2. USB-flash `esp32dev_relay_hall` to Bravo (COM port verified).
3. Validate `cmd/relay` (click + retained state + NVS restore).
4. Validate `cmd/hall/zero` + `cmd/hall/config` + `telemetry/hall` + `telemetry/hall/edge`.
5. Run `tools/dev/release-smoke.sh` (M1 + M2 + M4) to confirm no new failure surface.
6. Tag **v0.5.0** if validation lands clean. The accumulated `cmd/led "test"` from the prior session rides along.

### B. #46 / #76 closure pickup — ~30 min (low effort, high yield)

Tomorrow when the 24 h soak completes:
1. Run `tools/fleet_status.sh` (now 75 s window — fixed today).
2. Capture any new `/diag/coredump` payloads — there should be none.
3. If clean: move #46 + #76 to RESOLVED in the index, add STATUS lines to the archive.
4. If new abnormal boot or coredump appears: investigate per [COREDUMP_DECODE.md](COREDUMP_DECODE.md).

Same closure pattern works for #78 if the operator chose to clear retained coredumps and watch.

### C. #78 AsyncTCP race surgical patch — ~3-4 h coding + validation

Drop into option D from the #78 archive entry: vendored AsyncTCP patch
focused on the `_s_fin` handler (4/5 of the observed fault paths). The
goal: refcount the AsyncClient* in the event-queue, defer free until
the queue drains.

**Only worth doing if** the soak post-clear (B above) shows new
coredumps. If the soak is clean, v0.4.22's heap-guard hardening
already obviated the race in practice and #78 can move to
"mitigated, monitoring" status without coding work.

Per the decode pattern (3 distinct AsyncTCP entry handlers — _s_poll,
_s_fin, _accepted — and 2/5 PCs in non-text memory) the bug is more
likely "general use-after-free across the event-dispatch path" than
"single UAF in the _error callback". The patch needs to address the
generic case, not just the historic _error path.

## Items in OPEN that DON'T match A/B/C — at-a-glance

- **Group A — RFID/NFC (6)**: hardware-blocked on Foxtrot. Unblock by re-attaching Foxtrot to the bench.
- **Group B — ESP-NOW ranging (6)**: needs Bravo + Charlie pair. Pairs naturally with v0.5.0's Bravo bring-up.
- **Group D — open stability investigations (3)**: #46 + #54 + #78. All in surveillance / soak. See B above.
- **Group F — bench / variants (1)**: #72 voltage stress rig — needs operator hardware purchase.
- **Group G — long-tail closure (4)**: #33 (deferred to v1.0 / fleet > 10), #40 (field-validation pending), #76 (soak pending — see B), #85 (multi-session validation of the new sweep tool).

## Tooling shipped this afternoon (PM session)

For the next session's planning grep:
- `tools/dev/release-smoke.sh` — pre-tag chaos M1+M2+M4 wrapper (--quick, --m3 variants).
- `tools/dev/end-of-session-sweep.sh` — 4-check session-close surfacer (#85 sub-B prototype).
- `docs/COREDUMP_DECODE.md` — addr2line workflow runbook with worked examples.
- `docs/CANARY_OTA.md` + `docs/MONITORING_PRACTICE.md` — operational practice docs.
- `tools/fleet_status.sh` — capture window 7 s → 75 s (the LWT-shadow gotcha).
- `.github/workflows/build.yml` — secrets-scan (trufflehog) job + variant-build matrix.

## Won't do at session start

- New firmware release without operator request — fleet just had three releases today.
- Auto-clear retained coredumps — operator decision.
- Modify `~/.claude/` user-config files outside the repo.
- Anything DO-NOT-for-autonomous (workflow surgery, dashboard work, secrets-handling).
