# Questions for operator — autonomous session 2026-04-28 afternoon

Session goal: chip away at OPEN items while waiting for relay hardware.

## Q5 — v0.4.24 scope expanded slightly while you were away — OK to tag?

After your "Q3 bundle is good" approval I added two tightly-thematic
items to the v0.4.24 scope:

- **#34 Phase 1 captive-portal DNS hijack** — small (~8 KB flash,
  53 lines), gated by `AP_CAPTIVE_DNS_ENABLED`, makes sub-D's AP-mode
  fallback usable from a phone without typing `192.168.4.1`. Phase 2
  (port-80 redirector for the captive sheet) deferred. Commit
  `256fc91`.
- **#77 closure** — re-audit confirmed sub-A/B/C all already shipped
  under #79's `tools/dev/ota-rollout.sh`. Doc-only move from OPEN to
  RESOLVED. Commit `d986a32`.

If both are OK, v0.4.24 is ready to tag once you confirm — fleet is
5/6 back (see UPDATE below). If you'd rather drop one, easy revert.

## UPDATE — Fleet 5/6 recovered (~12:25 SAST)

5/6 production devices back: Alpha rebooted (int_wdt) and is healthy
again (uptime 23 min, heap 122912); Bravo / Delta / Echo / Foxtrot
all survived the outage with `mqtt_disconnects=1` and no reboot (uptime
~76 min preserved). Charlie stays offline per your Q1 decision.

Heap fragmentation worth noting — Bravo / Delta / Echo all show
heap_largest ~40-43 KB out of ~122 KB free (~33% fragmentation).
That's the surviving-reconnect tax. Will recover over time as the
allocator coalesces; no firmware action needed today.

Detailed log: [SESSIONS/AP_OUTAGE_2026_04_28.md](AP_OUTAGE_2026_04_28.md).

## URGENT — Fleet went offline mid-session (~10:50 SAST)

At session start (~09:30 SAST) the live retained showed every device with
fresh boot announcements + a healthy 30 min heartbeat (heap 118-123 KB).
By ~10:50 SAST every device's retained `/status` is the LWT
`{"online":false,"event":"offline"}`.

What I verified:
- Broker is healthy: `mosquitto` service Running, `mosquitto.log`
  mtime is current (10:50:30), publishing works.
- This is fleet-side, not broker-side. Most likely Wi-Fi AP issue —
  fleet-wide loss of association would LWT every device simultaneously.
- Charlie's pre-existing `/diag/coredump` (Q1 above) is unrelated; that
  was retained from earlier and shows on top of the offline LWT.

What I did NOT do:
- Did NOT run any chaos scenario (would have made it worse).
- Did NOT trigger any blip / restart command.
- The new firmware code (sub-C/D/I) is committed but not yet flashed,
  so this outage is independent of any code I wrote.

Recommended operator action:
1. Check the AP — is it associated with anything? Power-cycle if uncertain.
2. Once devices reassociate, run `/daily-health` to see how they recovered.
3. After fleet is back, OTA to v0.4.24 (code is committed but not tagged
   yet — see Q3 below).

If devices DO NOT come back after AP recovery, that's a more serious
issue that the new sub-C 10-minute time-based unrecoverable trigger
would catch and surface via `restart_cause:"mqtt_unrecoverable"` in
boot announcements. Without v0.4.24 deployed, the existing fleet uses
the count-based trigger which can take much longer.

## Pending answers (please respond when you check in)

### Q1. Charlie canary — re-trip on #78  **[ANSWERED 2026-04-28: continue canary test, leave Charlie undisturbed]**
Charlie's retained `/diag/coredump` shows another async_tcp `InstructionFetchError`
(PC=0x3f409271 — in DRAM, vtable-corruption shape consistent with #78).
Last heartbeat uptime was ~12.4 h, then LWT offline. So the long sticky soak
DID eventually trip — same shape as previous #78 captures, but on the
canary build (`CHECK_STACKOVERFLOW=2`) which would have halted-at-overflow
before this kind of generic vtable corruption.

**Implication:** strengthens the case that #78 is NOT a stack overflow,
it's a use-after-free / async-task race. Canary did its job — ruled out
stack overflow as a cause for THIS family of crash.

**Question:** OK to leave Charlie in its current offline state for you to
inspect on serial when you're back? Or want me to attempt a soft restart
(no-DTR) via mosquitto if it has WiFi+broker reach?

### Q2. Foxtrot UUID drift (informational)  **[ACK 2026-04-28]**
Live retained boot announce shows Foxtrot's UUID is now
`c1278367-21af-478d-8a8b-0b84a4de60df` (MAC `28:05:A5:32:50:44`),
not the `3b3b7342-80e7-43dd-afc7-78d0470861e2` listed in CLAUDE.md.
This is consistent with #48 (RNG-pre-WiFi) — Foxtrot must have been
NVS-wiped at some point.

**Plan:** I'll update the CLAUDE.md fleet table to reflect the live UUID
in the end-of-session sweep, and add a note that Foxtrot UUID has rotated.
The legacy UUID stays usable for retained lookups.

### Q3. v0.4.24 release scope  **[ANSWERED 2026-04-28: bundle is good — proceed with full sub-C/D/I + chaos framework + #24/#28 cleanup as v0.4.24 once fleet is back]**
I plan to bundle this batch into a v0.4.24 patch release after validation:
- #76 sub-C (time-based MQTT_RESTART_THRESHOLD)
- #76 sub-D (restart-loop cool-off → AP fallback mode)
- #76 sub-I (WDT vs SW_CPU_RESET separation in `/daily-health`)
- #28 string-lifetime audit (doc only, no firmware change unless audit
  finds an unsafe site)
- Index hygiene — move #24 to RESOLVED (already addressed v0.4.0 era,
  stale OPEN entry)

**Question:** any objection to cutting v0.4.24 with that bundle, or
prefer I park sub-D (the AP-fallback) as a separate release because it's
the most behaviour-changing item?

### Q4. Chaos framework promotion (#75) — scope check  **[ANSWERED 2026-04-28: future relay-controller ESP32 (post-v0.5.0) will switch power to the AP and to other bench devices via cmd/relay. wifi_cycle.ps1 stays a stub until then; updated header to document the planned hook.]**
Plan: create `tools/chaos/` with:
- `blip_short.ps1` (5s), `blip_long.ps1` (30s), `blip_burst.ps1` (3×10s)
- `wifi_cycle.ps1` (toggle the AP for 60s — needs your AP control script)
- `runner.sh` orchestrator
- `report_to_json.sh` for CI consumption

**Question:** the WiFi-cycle scenario needs a way to toggle the AP. Do
you have an existing PS script / Tasmota endpoint / smart plug for the
AP, or should I leave wifi_cycle as a stub with a `# TODO: needs operator
AP-control hook` and ship the rest?

## Items skipped this session — flagging why

- **#11/#12/#14-#17 RFID, #19-#23 LED** — feature work, want hardware to
  validate. Skipped.
- **#25 bootloader rollback** — BLOCKED on pioarduino upstream bug per
  archive. No movement possible without operator.
- **#27 lib-API regression test** — `.github/workflows/` change, on
  DO-NOT for autonomous mode.
- **#33 versioned MQTT topics** — design change with Node-RED bridge
  cost. Needs your call before any firmware change.
- **#34 captive portal DNS** — UX win, low priority, deferred.
- **#36 dashboard tile, #68 Node-RED auth** — operator-visible Node-RED
  changes, on DO-NOT.
- **#46 Recent Abnormal Reboots** — this session's ongoing. Charlie
  re-trip captured (Q1). Will close once #78 fix lands; no firmware
  change in scope today.
- **#49 OTA URL bootstrap propagation** — root fix targeted for v0.5.0
  per archive. Deferring.
- **#71 variant infra** — substantial; would benefit from your Q3
  scoping decision before I touch.
- **#72 bench-supply rig, #47 hardware verification** — need hardware.
- **#78 AsyncTCP fix attempt** — premature without more diagnostic
  data per NEXT_SESSION_PLAN. Charlie's re-trip (Q1) is one more data
  point but still not enough to commit to a fix path (patch vs replace).
