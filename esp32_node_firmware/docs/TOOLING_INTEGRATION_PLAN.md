# Tooling Integration Plan

How Claude Code, GitHub, PlatformIO, Mosquitto, Node-RED, and the operator's workflow could be set up to work together better. Drawn from the friction observed during the v0.4.10 / Phase A diagnostic session 2026-04-25/26.

Tiered by impact ÷ cost. Do Tier 1 first; Tier 3 only if the underlying need surfaces.

---

## Tier 1 — High-impact, ≤ 1 hour each

### T1.1 Node-RED file logging (already #52)
Add `logging.file` block to `~/.node-red/settings.js` with `flushInterval: 10`. Restart Node-RED. Live-tail with `Get-Content -Wait`. Eliminates the "log frozen at 20:30" failure mode that hit us this morning.

### T1.2 Mosquitto log rotation
The current log is **87 MB** and growing (~30 MB/day at this fleet size). Will eventually fill the disk silently.

Mosquitto has no native `log_max_size` directive. Use Windows Task Scheduler with a daily PowerShell rotation script:

```
C:\ProgramData\mosquitto\rotate-log.ps1
```

Renames `mosquitto.log` to `mosquitto.log.YYYY-MM-DD`, keeps 5 generations, restarts the service. Register elevated:
```
schtasks /create /tn "MosquittoLogRotate" /tr "powershell -File C:\ProgramData\mosquitto\rotate-log.ps1" /sc daily /st 02:00 /ru SYSTEM
```

### T1.3 Move `.dummy/` scripts into `tools/`
The patches and orchestrators that were one-shot during this session (`patch_stagger.py`, `patch_canary.py`, `patch_boot_reason.py`, `fleet_ota.sh`) are useful on paper but get lost in `.dummy/` (ignored). Promote the keepers to `tools/node_red/`, commit them, document `tools/README.md`. Future sessions can call `python tools/node_red/patch_stagger.py` without re-discovering the API.

**STATUS (2026-04-27):** `tools/` directory and `tools/fleet_ota.sh` created in v0.4.11. `.dummy/` cleanup (promote keepers → `tools/node_red/`, delete junk) completing this session.

### T1.4 Run `/less-permission-prompts`
Already-installed Claude Code skill. Scans the session transcripts for read-only Bash and MCP tool calls that we're approving repeatedly, generates a project-level `.claude/settings.json` allowlist. Cuts the prompt-spam during diagnostic sessions like this morning's.

### T1.5 Standardise pio + esptool invocation
The encoding/path issues we hit (`pio run` truncating Unicode; `esptool` failing relative-path lookups when not in the firmware dir) are recoverable but slow us down. Two-line fix in repo:

- Add `tools/flash_dev.sh` that wraps the right `--port`, `--baud`, `PYTHONIOENCODING=utf-8`, and resolves bin paths relative to repo root.
- Add `tasks.json` (already exists per v0.4.x commits) with VS Code Tasks for "Build", "Flash COM5", "Monitor COM5" — saves typing.

Document both in `CLAUDE.md` under "Build & Test" so the next session uses them.

**STATUS (2026-04-27):** `tools/flash_dev.sh` and `.vscode/tasks.json` shipped in v0.4.11. CLAUDE.md already documents them. Verify-only step this session — no changes expected.

---

## Tier 2 — Medium-impact, ≤ a day each

### T2.1 GitHub Pages availability check (in /daily-health)
Today's Pages-disabled event went undetected until daily-health ran and reported RED. Move it to the foreground: `gh api repos/dj803/esp32_node_firmware/pages` should return `status: built`; if it returns 404 or `building`, surface as YELLOW. The existing `daily_health_check.py` already checks the manifest URL — add an `gh api .../pages` probe alongside.

### T2.2 Heap-trajectory dashboard tile (depends on #53 firmware)
Once v0.4.11 ships heap_free + heap_largest in heartbeats, add a Node-RED Dashboard 2.0 tile (Vue.js template) that plots a rolling 24-hour line per device. Slow downward trend = leak. Sudden drop + recovery = transient pressure. Pairs with the existing `boot_history` tile.

### T2.3 GitHub branch protection + signed commits
Currently `master` accepts any push. Three checkboxes in repo Settings → Branches:
- Require a pull request before merging (off if solo-dev → keep direct push)
- Require status checks to pass (CI) before merging
- Require signed commits (would prevent accidental Claude-only commits without your GPG key)

For solo development, just enabling "require status checks" prevents merging without CI green. Low cost, high signal.

### T2.4 Mosquitto WebSocket listener
Add to `mosquitto.conf`:
```
listener 9001
protocol websockets
```
Lets browser tools (e.g. MQTT.fx Web, MQTT Explorer) connect directly without a Node-RED proxy. Useful when Node-RED itself is being debugged.

### T2.5 Stack canary build env (already #54)
Add `[env:esp32dev_canary]` in `platformio.ini` with `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2`. One-time setup; flash to one device once we have a stable baseline post-#51.

### T2.6 Auto-tag GitHub release on version commit
Today's release pipeline required a manual `git tag v0.4.10 && git push origin v0.4.10`. A GitHub Action that triggers on push-to-master and parses `config.h`'s `FIRMWARE_VERSION` literal — if it ends in a non-`-dev` value AND the tag doesn't yet exist, auto-tag and push. Reduces "I forgot to tag" risk.

### T2.7 fleet snapshot one-liner
Promote the JSON-decoder block I've been hand-typing all session into `tools/fleet_status.sh`:
```bash
#!/usr/bin/env bash
mosquitto_sub -h 192.168.10.30 -t '...+/status' -F '%t %p' -W 7 | python tools/fleet_status_parse.py
```
Cuts every "what's the fleet doing" check from 30 seconds of typing to 1 command.

**STATUS (2026-04-27):** `tools/fleet_status.sh` already exists. Verify + document in `tools/README.md` this session.

---

## Tier 3 — Lower priority, ≥ 1 day each, do only if the need surfaces

### T3.1 Mosquitto auth + TLS
Currently anonymous + plaintext. Acceptable on the trusted LAN per the project's documented threat model, but if the deployment ever moves off the segment, this becomes Day 1 work. Plan only — do not implement until needed.

### T3.2 Node-RED HTTPS admin + auth
Same rationale as T3.1. Default Node-RED admin is open on `127.0.0.1:1880` but Wi-Fi-bridged devices on the LAN can reach it. Adding `adminAuth` + `https` to `settings.js` is ~10 lines but introduces operational complexity (cert rotation, password recovery).

### T3.3 Dependabot + Trivy SBOM
GitHub Action that runs Trivy on PR; Dependabot watches the Arduino lib_deps. Catches CVEs and stale libs. Currently #4 in SUGGESTED_IMPROVEMENTS, lower-priority because the threat model is internal-IoT.

### T3.4 Reproducible builds
Replace `FIRMWARE_BUILD_TIMESTAMP` with `SOURCE_DATE_EPOCH` (Reproducible Builds project standard). Lets two builds of the same source produce byte-identical firmware.bin. Useful for supply-chain auditing. Currently #3 in SUGGESTED_IMPROVEMENTS.

### T3.5 Remote scheduled agents (`/schedule`)
The `schedule` Claude skill creates remote agents that run on Anthropic's infrastructure. Useful for "every Sunday, run a fleet diff and tell me what changed" if there's such a need. Today the local `/loop` covers it.

### T3.6 Hardware lab discipline
- USB cable inventory: label each cable with its known-good current rating; the Foxtrot-bricked-from-thin-cable failure mode is real.
- ESP32 module roster: serial number, MAC, factory date, deployed-since date, replacement count. Helps spot per-module flakes (Charlie pattern).
- Bench-isolation rig: a separate desk with one ESP32, one USB cable, one power supply, no nearby radios. For #46 / Charlie-style chronic-flake diagnosis.

---

## Cross-tool wiring that already works (don't break it)

- **CI → gh-pages** auto-updates `ota.json` on tag push (proven yesterday).
- **Node-RED → Mosquitto** broker connection auto-recovers on broker restart.
- **Daily-health → MQTT retained summary** publishes a one-line JSON every run, dashboard renders it.
- **Claude → flash skill** wraps the canonical build+flash+monitor cycle.
- **Claude → release skill** handles the full version-bump → tag → push → CI → manifest-update flow.
- **CLAUDE.md** already documents diagnostic order (mosquitto.log → MQTT snapshot → live debug) and the fresh-boot-capture procedure (controlled RTS reset, no DTR).

---

## Rollout suggestion

This week:
- Tier 1 in any order (each is short)
- Add `tools/` directory, commit the keepers, delete `.dummy/`

Next week / when convenient:
- T2.1 (Pages probe), T2.2 (heap tile), T2.7 (fleet snapshot script) — all small, all add daily quality-of-life
- T2.5 (canary build) only if Phase A passes and we want the long-term safety net

Park / on-demand:
- All Tier 3 items. Revisit if a specific incident or scope change makes them load-bearing.
