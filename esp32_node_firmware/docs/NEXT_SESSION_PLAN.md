# Next session plan

Refreshed 2026-04-29 morning after the Phase 2 R1 ESP-NOW ranging session
(2026-04-28 evening) and the overnight power-failure cascade event
(2026-04-29 morning). The previous A/B/C menu is superseded — #78 is now
bench-debuggable for the first time, which makes it the priority.

## State at end of session (2026-04-29 ~08:40 SAST)

| | |
|---|---|
| Master HEAD | unchanged from end of 2026-04-28 PM session (no commits in the Phase 2 R1 work yet) |
| Fleet | 5/5 production on **v0.4.26**. Charlie sticky on **v0.4.20.0 canary, 34.6 h uptime**, survived TWO independent #78 cascades without firing the stack-canary. |
| Backlog | OPEN **27**, RESOLVED **53**, WONT_DO **11** (up from 20/53/11 at last session start; +7 from Phase 2 R1: #86-#91, +1 from morning cascade #92) |
| Two unstaged doc commits worth of edits | `git diff esp32_node_firmware/docs/` shows ~470 lines added across SUGGESTED_IMPROVEMENTS.md + ARCHIVE.md (docs only, no firmware changes) |
| Two events | Phase 2 R1 cascade 2026-04-28 evening (USB-power-cycling trigger). Power-failure → AP-restart cascade 2026-04-29 morning. Both triggered #78 → fleet-wide multi-task-family panic. |

## Bench state (current)

- **Charlie on COM** — sticky canary v0.4.20.0, 34.6 h uptime, MQTT_HEALTHY. **DO NOT DISTURB** — its survival across the cascades is the positive-evidence-for-not-stack-overflow datapoint that closes #54.
- **Alpha** — power on laptop USB. v0.4.26. Retained `cmd/espnow/ranging "1"`.
- **Bravo / Echo / Delta** — were on battery during the overnight outage (now on mains again presumably). v0.4.26. **Delta has NVS-persisted per-peer cal entry** for Foxtrot (tx=-73, n=1.61) — this survived the morning panic + reboot, validating the persistence path.
- **Foxtrot** — just power-cycled by operator after staying offline through the cascade. Up since ~08:40 SAST.

## What we learned (yesterday evening + this morning)

**Per-peer calibration pipeline is verified end-to-end on hardware.** First time. cal_entries 0→1 + calibrated:true confirmed. Pipeline is sound; numerical acceptance bounds (R²≥0.95, n∈[2,4], etc.) didn't pass on this rig because of indoor multipath, not firmware issues.

**Orientation is the dominant ranging-asymmetry factor.** Quantified at 9-14 dB rotation swing on the figure-of-8 antenna pattern. Through-wall pairs average over lobes via multipath (Alpha→Echo flat at -84 dB across all 4 Echo rotations). **Echo's hardware exonerated** — earlier "weak TX" was an unlucky-orientation snapshot.

**#78 is reproducible.** Two independent cascades within 24 h, both triggered by Wi-Fi association cycling (USB-power-cycle of devices on day 1, AP/power restoration on day 2). 4 distinct exc_task families now confirmed: async_tcp, tiT/lwIP, loopTask, wifi (NEW today). int_wdt joins panic as a #78 boot_reason. Battery operation does NOT mitigate.

**#86 is a manifestation of #78.** Bravo's silent-collect failure cleared by full power-cycle but not by panic-reboot — heap corruption residue. Fixing #78 fixes #86.

**#54 has positive evidence: #78 is NOT a stack overflow.** Charlie's canary build with `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` survived BOTH cascades without firing. Strong confirmation of heap-corruption hypothesis.

## Recommended next session — #78 bench-debug session (~3-4 h)

This is now the highest-yield work in the backlog. The #78 reproduction recipe means we can capture the cascade in real time on serial and root-cause it. Promoted from "C surgical patch" (was speculative) to "primary next-session candidate".

### Pre-session operator action
1. Rotate **Bravo OR Delta** onto a serial-attached COM port (whichever is most convenient). Both have shown new exc_pcs in recent cascades. Charlie stays on its COM port — preserves canary forensics.
2. Pull Charlie's serial backlog and save it to a session note. With 34.6 h of uptime and TWO cascades survived, the serial log will show what Charlie's stack-canary checks were doing during the events. Either:
   - No canary fire log lines → confirms #78 is not a stack issue (strongest evidence)
   - Canary fire log lines that didn't propagate to the dashboard → bug in the canary report path (informs #54 disposition differently)

### Steps in session
1. **Serial-attached fleet baseline** — start `pio device monitor -p COMx -b 115200 | tee docs/SESSIONS/<device>_PRE_CASCADE_<date>.txt` on the attached device. **Critical: logging must be running BEFORE the cascade fires.** 2026-04-29 morning capture confirmed production v0.4.26 (and v0.4.20.0 canary) are SILENT on serial during steady state — emit nothing unless something abnormal happens. Post-event captures miss the panic dump entirely. Bring fleet up to steady-state with the log already running.
2. **Pull v0.4.26 ELF artifact from CI** for symbol resolution. Specifically need to symbolize:
   - Bravo wifi `0x401d2c66` LoadProhibited (NEW today)
   - Alpha async_tcp `0x00000019` InstFetchProhibited (NEW today — null-region jump)
   - Plus the 4 historic exc_pcs from the 2026-04-28 evening cascade (in #92 archive)
3. **Reproduce the cascade**: kill the WiFi AP for 30+ seconds, restore, watch the serial output as the panic happens. Capture the panic backtrace before it hits the watchdog and reboots.
4. **Decode panic** with `addr2line` against the ELF (per [COREDUMP_DECODE.md](COREDUMP_DECODE.md)).
5. **Root-cause analysis** — with the symbolic backtrace, identify whether the corruption is:
   - AsyncTCP's `_error` path passing a freed pointer downstream (original hypothesis)
   - lwIP's pbuf or PCB lifetime mismanagement under reconnect storm
   - WiFi driver's TX-queue cleanup racing with reconnect
   - Something else entirely
6. **Implement targeted fix** — either vendored AsyncTCP patch, lwIP-config tweak, or defensive null-out somewhere in the reconnect path. Likely 50-100 lines.
7. **Re-run reproduction recipe** post-fix to verify the cascade no longer fires.
8. **Tag v0.4.27 / v0.5.0** if fix lands clean.

### Acceptance
- Reproduction recipe is run, cascade captured on serial, exc_pc symbolized
- Root cause identified with reasonable confidence
- Either fix shipped OR clear next-step plan documented
- Any net-new exc_pcs added to #78 archive entry

## Action items from morning analysis (2026-04-29)

These are notes for the next session — not work to do mid-summary.

### 3. #78 reproduction-recipe diagnostic session
**Owner:** next session. Plan above.

### 4. Symbolize new exc_pcs against v0.4.26 ELF
**Owner:** next session, as part of the bench-debug session prep. The ELF artifact lives in CI build output. Two specific PCs to resolve first:
- `0x401d2c66` (Bravo, wifi task, LoadProhibited)
- `0x00000019` (Alpha, async_tcp, InstFetchProhibited — likely indicates a vtable / function-pointer call through a null pointer)

### 5. Operator install guide cascade-recovery note
**Owner:** done in this session — see new section in [OPERATOR_INSTALL_GUIDE.md](OPERATOR_INSTALL_GUIDE.md) below.

## Other queued work (not next-session priority)

- **v0.5.0 hardware bring-up** (relay + Hall on Bravo): blocked on operator re-attaching Bravo with relay/Hall wiring. Plan unchanged in [PLAN_RELAY_HALL_v0.5.0.md](PLAN_RELAY_HALL_v0.5.0.md).
- **#54 disposition**: mark RESOLVED with positive evidence next session (Charlie's 34.6 h cascade-survival).
- **#46 + #76 closure**: was waiting on a 24 h soak; the soak was inconclusive due to power event. Re-set the soak window once #78 fix lands; the post-fix soak is the right time to close.
- **#90 systematic orientation test** in cleaner-RF environment: queued, low priority while #78 is open.
- **#91 procurement**: ESP32-WROOM-32U + antennas, ~$15-30. Order anytime; will inform a dedicated #91 session weeks from now.
- **#40 install-guide content for orientation + cascade-recovery + power-cycle-if-silent**: items 5 below + #90 + #86 workaround. Bundle for one install-guide refresh.

## Won't do at next-session start

- New firmware release without operator request — there's no ship pressure right now.
- Auto-clear retained coredumps — operator decision (still useful for forensics).
- Anything DO-NOT-for-autonomous (workflow surgery, dashboard work, secrets-handling).
- Touch Charlie's canary — it's contributing positive evidence on every soak hour.
