# Node-RED ESP-NOW Tracking Tab — Rebuild Reference Notes

> **Status:** ESP-NOW Tracking tab (and Pair Chart tab) are being discarded and
> rebuilt from scratch. This document captures everything learned across
> multiple fix attempts so the rebuild avoids the same traps.
>
> Date of last fix attempt: 2026-04-24  
> Author: Claude (session summary for handoff)

---

## 1. What Was Requested

### Original bug report (user, 2026-04-24)

The user reported four distinct problems on the Node-RED **ESP-NOW Tracking**
tab, visible on the live dashboard with 3 ESP32 devices (Alpha, Bravo, Charlie)
ranging each other over ESP-NOW:

| # | Symptom | Observed |
|---|---------|----------|
| 1 | **6 chart lines instead of 3** | Each tracked device appeared twice — once labelled by friendly name (`ESP32-Charlie`) and once by MAC (`D4:E9:F4:60:1C:C4`) |
| 2 | **Gaps / zig-zag on the plot line** | Each duplicate series received only ~50% of samples; the line alternated between the two fake series, creating a visual sawtooth |
| 3 | **No range controls** | Time window hard-coded at 1 hour; Y-axis hard-coded at 0–auto; no UI for the user to change either |
| 4 | **Poor layout** | All 7 UI nodes (buttons, chart, peer table, rename form) crammed into one 12-wide `espnow_grp_scan` group |

The **Pair Chart tab** was also flagged with the same Bug 1/2 pattern (pair key
flips between friendly name and MAC when one side's name is unknown).

User's execution scope: "Proceed with all planned phases. 3 ESPs connected.
Flash/OTA as needed. Push Node-RED API as needed. Verify as needed."

---

## 2. Architecture of the Broken Original

### Data flow (original, pre-any-fix)

```
MQTT broker
  └─ espnow_mqtt_in  (topic: Enigma/.../+/espnow)
        ├─► espnow_discover_fn   ──► espnow_peer_tpl   (table display only)
        └─► espnow_filter_fn     ──► espnow_kalman_fn  ──► espnow_dist_chart
```

**Raw MQTT payload shape** (what `espnow_mqtt_in` delivers):
```json
{
  "node_name": "ESP32-Charlie",
  "peer_count": 2,
  "peers": [
    {"mac": "F4:2D:C9:73:D3:CC", "rssi": -52, "rssi_ema": -52, "dist_m": "0.5", "rejects": 0},
    {"mac": "84:1F:E8:1A:CC:98", "rssi": -48, "rssi_ema": -48, "dist_m": "0.4", "rejects": 0}
  ]
}
```

Note: **no `name` field** in the raw peer objects. Firmware reports MAC only.

### `espnow_discover_fn` (original)

```js
var name = (roster[p.mac] && roster[p.mac].name) ? roster[p.mac].name : '';
disc[p.mac] = { mac: p.mac, name: name, rssi: ..., dist_m: ..., rejects: ... };
```

- Reads `espnow_roster` (populated from retained MQTT `status` messages).
- If roster hasn't loaded yet (asynchronous), `name = ''`.
- Writes to `flow.context.espnow_discovered` and emits enriched payload.
- **Output goes only to `espnow_peer_tpl` (the table) — NOT to the filter.**

### `espnow_filter_fn` (original)

```js
var label = p.name || p.mac;
msgs.push({ payload: parseFloat(p.dist_m) || 0, topic: label });
```

- **Receives raw MQTT (no name/label field).**  `p.name` is always `undefined`.
- Falls through to `p.mac` → topic is always MAC → should produce 3 series, not 6.

### Root cause of Bug 1 + 2 (actual)

The duplicate-series bug existed but by a different mechanism than first
assumed. The original `espnow_discover_fn` emits `{ peers: Object.values(disc) }`
**and** the raw MQTT fanout hits `espnow_filter_fn` simultaneously. Peer objects
from `disc` DO have a `name` field (set from roster); if the roster lookup
resolves on one packet but not the next, the chart series key (`p.name || p.mac`)
alternates between the friendly name and the MAC. That produces two series per
device and the sawtooth gap pattern.

The timing window is narrow but consistent: on startup, the first few MQTT
packets arrive before the retained `status` messages have rebuilt the roster, so
early packets emit with MAC labels and later packets emit with friendly-name
labels.

**Confirmed by**: screenshot in original bug report showing Blue series
`ESP32-Charlie` + Red series `D4:E9:F4:60:1C:C4` with interleaved sawtooth.

### Pair Chart additional bug

`pair_extract_fn` uses `roster[peer.mac] || peer.mac` as the peer name.
`pair_join_fn` sorts two names alphabetically to form the canonical pair key.
If one name is a MAC and later resolves to a friendly name, the key changes
→ two series per pair (same sawtooth pattern).

---

## 3. Fixes Designed and Attempted

### Fix A — Sticky per-MAC label in `espnow_discover_fn`

**Design:** Add a `label` field per discovered peer. `label` is set once and
never overwritten:

```js
var prev       = disc[p.mac] || {};
var rosterName = (roster[p.mac] && roster[p.mac].name) ? roster[p.mac].name : '';
var macCompact = (p.mac || '').split(':').join('').slice(-4);   // e.g. "CC98"
var label = prev.label || rosterName || macCompact;             // sticky
disc[p.mac] = { mac: p.mac, name: rosterName, label: label, ... };
```

Also patched `espnow_filter_fn` to use `p.label || p.name || p.mac`.

**Result:** Unit tests passed (3/3). Isolated Node.js replay of 60-second MQTT
capture confirmed exactly 3 unique topics per stream with no flipping.

**BUT — architectural mismatch:** The filter receives raw MQTT (not
discover_fn output). Raw peer objects have no `label` field. `p.label` is
always `undefined`, so the filter fell through to `p.mac` anyway. The sticky
label in discover_fn was correct logic but was written to a context object
that the filter never reads. Fix A had **no effect on the duplicate-series bug**.

**What was needed instead:** Either:
- Rewire so `espnow_mqtt_in → espnow_discover_fn → espnow_filter_fn` (serial, not parallel fanout), OR
- Have `espnow_filter_fn` read peer labels from `flow.get('espnow_discovered')` by MAC key rather than from `msg.payload`

### Fix B — Time-range and Y-range controls

**Design:**
- New `espnow_range_tpl` (ui-template form): time window dropdown + Y min/max
  inputs + auto toggles.
- New `espnow_range_filter_fn` inserted between Kalman and chart; drops
  out-of-range samples.
- New `espnow_range_apply_fn`: on form submit, writes `espnow_chart_range` to
  flow context; sends `{action:"clear"}` to chart to flush old data.
- `resendOnRefresh: false` on the form (initial version had `true` — see
  Bug C below).

**Result:** Node wiring was correct. The range filter logic was verified
syntactically. Chart visibility was **never confirmed** — the chart showed no
data throughout testing (see Section 4).

### Fix C — Layout reorganization

**Design:** Split `espnow_grp_scan` (one mega-group) into four purpose-focused
groups:

| Group | Contents | Width |
|---|---|---|
| `espnow_grp_controls` | Buttons (Ranging / Refresh / Reset) + Rename form | 12 |
| `espnow_grp_chart` | Distance chart only | 9 |
| `espnow_grp_chart_ctl` | Range controls form | 3 |
| `espnow_grp_peers` | Peer table | 12 |

**Result:** Push succeeded (204). Layout change itself was not visually
confirmed — chart was still empty so the visual state was never in a testable
condition.

### Fix D — Pair Chart mirroring

Same sticky-label and range-control pattern intended for the Pair Chart tab.
Not reached before cancellation.

---

## 4. Bugs Encountered During the Fix Attempt

### Bug C — `resendOnRefresh: true` on range form (my error)

Initial patch created `espnow_range_tpl` with `resendOnRefresh: true`.
Every dashboard page load sent the default form payload → `espnow_range_apply_fn`
triggered → emitted `{action: "clear"}` to `espnow_dist_chart` → chart wiped
on every refresh. Fixed to `resendOnRefresh: false` and re-pushed.

### Bug D — Self-loop in wiring patch script (my error)

The `patch_flows.py` script had a rewire loop that replaced `espnow_dist_chart`
as a target in ALL nodes' wires, including `espnow_range_filter_fn`'s own
output wire. This created a self-loop: filter → filter → filter. Symptom:
Node-RED accepted the push but the filter node would loop indefinitely on any
message. Fixed by adding `EXEMPT_FROM_REWIRE = {'espnow_range_apply_fn', 'espnow_range_filter_fn'}`.

### Bug E — Flow context cleared on every deploy (pre-existing, not my bug)

Node-RED clears in-memory flow context (`flow.get()`/`flow.set()`) on every
deployment — even with `Node-RED-Deployment-Type: nodes`. This means:

- `espnow_tracked_macs` (which MACs the user wants to track) is wiped every
  deploy.
- After each of my pushes, the filter received an empty tracked list and
  returned `null` for every message → chart stayed empty.
- The user had to click "Track Selected" in the Peer Table after every push.

**Confirmed via context API:**
```
Before push:  espnow_tracked_macs: ["D4:E9:F4:60:1C:C4","84:1F:E8:1A:CC:98","F4:2D:C9:73:D3:CC"]
After push:   espnow_tracked_macs: []
```

This was the primary reason the chart showed no data throughout testing. Each
push I made reset the tracking state, and the user was not always aware they
needed to re-click Track Selected.

The `espnow_roster` (device name map) survived because it is rebuilt from
retained MQTT `status` messages within seconds of deploy. But `tracked_macs`
has no such rebuild path.

### Bug F — `espnow_kalman_states` always empty

Even when `tracked_macs` was confirmed non-empty (3 MACs), `espnow_kalman_states`
remained `{}`. This means either:
- The filter was returning `null` (tracked_macs empty from Bug E), or
- Messages were reaching Kalman but the subflow's context key was scoped
  differently.

The subflow instance `espnow_kalman_fn` uses `STATES_KEY = espnow_kalman_states`
and reads `flow.get('espnow_kalman_states')`. The context API showed this key
under `espnow_flow` scope (correct). Root cause was never isolated before
cancellation — most likely Bug E (tracked empty → filter null → kalman never
receives anything).

---

## 5. What Actually Worked

| Component | Status |
|-----------|--------|
| Sticky label unit tests (isolated Node.js replay) | ✓ **Passed 3/3** |
| `espnow_discover_fn` accumulating with correct `label` field | ✓ **Confirmed via context API** |
| `espnow_roster` surviving deploys (rebuilt from retained MQTT) | ✓ **Confirmed** |
| Rollback to pre-edit state via `/flows` PUT | ✓ **Successful** |
| MQTT data flowing from all 3 devices | ✓ **Confirmed** (6 msgs/s on topic pattern) |
| `resendOnRefresh: false` fix | ✓ **Applied** |
| Self-loop wiring fix | ✓ **Applied** |

| Component | Status |
|-----------|--------|
| Chart showing any data | ✗ **Never confirmed** |
| Duplicate-series bug actually fixed (end-to-end) | ✗ **Not confirmed** |
| Range controls working | ✗ **Not confirmed** |
| Layout visually verified | ✗ **Not confirmed** |
| Pair chart fixes | ✗ **Not attempted** |

---

## 6. Correct Architecture for the Rebuild

The fundamental architectural problem is the **parallel fanout** from
`espnow_mqtt_in`:

```
espnow_mqtt_in ─┬─► espnow_discover_fn  (enriches with name/label)
                └─► espnow_filter_fn    (gets raw payload — no name/label)
```

The filter should receive **enriched** peer data (with stable labels),
not raw MQTT. Two valid approaches for the rebuild:

### Option A — Serial pipeline (simplest)

```
espnow_mqtt_in ──► espnow_discover_fn ──► espnow_filter_fn ──► espnow_kalman_fn ──► chart
                          │
                          └──► espnow_peer_tpl  (table — as second output)
```

`espnow_discover_fn` emits on output 0 to filter AND on output 1 to the table.
Filter now receives `msg.payload.peers` with `label` set — sticky labels work.

### Option B — Filter reads from context

Keep fanout as-is; change `espnow_filter_fn` to look up the label from
`flow.get('espnow_discovered')` by MAC key:

```js
var disc = flow.get('espnow_discovered') || {};
peers.forEach(function(p) {
    if (tracked.indexOf(p.mac) >= 0) {
        var entry  = disc[p.mac] || {};
        var label  = entry.label || p.mac;
        msgs.push({ payload: parseFloat(p.dist_m) || 0, topic: label });
    }
});
```

Option B is a smaller change but adds context reads per message. Option A
is architecturally cleaner.

### Persist tracked_macs across deploys

`espnow_tracked_macs` must survive deploys. Options (pick one):

1. **File-backed context** — set `contextStorage` in Node-RED `settings.js`
   to use `localfilesystem` for the default store. One config change, no flow
   change.
2. **Retained MQTT** — `espnow_set_tracked_fn` already publishes
   `cmd/espnow/track` as a retained MQTT message. Add a startup inject → MQTT
   subscribe → function that reads the retained value and sets `tracked_macs`.
3. **Auto-track all known devices** — on first MQTT tick, if `tracked_macs`
   is empty, auto-populate from all entries in `espnow_discovered`. Simplest
   for a single-user dashboard with a fixed fleet.

### Range controls (minimal viable rebuild)

Keep it simple:
- One `ui-slider` or `ui-number-input` for "show last N minutes" (default 60).
- On change: emit `{action:"clear"}` to chart AND update `removeOlder` via
  `msg.ui_update` if Dashboard 2.0 supports it; otherwise just clear and
  let it repopulate.
- Y-axis: Dashboard 2.0 does not support runtime `ymin`/`ymax` override
  reliably. Client-side filter (drop samples outside range) is safer.

---

## 7. Key Context for Rebuild

### Device fleet

| Device | UUID | MAC |
|--------|------|-----|
| ESP32-Alpha | `32925666-155a-4a67-bf50-27c1ffa22b11` | `84:1F:E8:1A:CC:98` |
| ESP32-Bravo | `6cfe177f-92eb-4699-a9a6-8a3603aae175` | `F4:2D:C9:73:D3:CC` |
| ESP32-Charlie | `2ff9ddcf-bf7e-4b51-ba6c-fa4bfcd80cdd` | `D4:E9:F4:60:1C:C4` |

MQTT broker: `192.168.10.30:1883`

### MQTT topic patterns

| Pattern | Contents |
|---------|----------|
| `Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/+/espnow` | ESP-NOW ranging telemetry (all peers) |
| `Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/status` | Retained status/heartbeat (source of roster) |
| `Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/+/cmd/espnow/track` | Retained — last MACs selected for tracking |

### Node-RED environment

- Dashboard 2.0 (Vue.js) — templates use `<template>`, `v-if`, `v-for`.
- Projects mode enabled; active project: `esp32-node-firmware`.
- Live flows: `C:\Users\drowa\.node-red\projects\esp32-node-firmware\flows.json`
- Admin API: `http://127.0.0.1:1880/flows` (no auth)
- Context storage: **in-memory only** (default — no persistence). Fix in
  `settings.js` if persistence needed.
- Use `Node-RED-Deployment-Type: nodes` for targeted redeploys, but note
  that **even nodes deploys clear flow.context for the affected tab**.

### Kalman filter subflow

Subflow ID: `sf_kalman_filter`. Instance env var `STATES_KEY` sets the flow
context key for per-topic state. The instance on the ESP-NOW tab uses
`STATES_KEY = espnow_kalman_states`.

Parameters: Q=0.1 (process noise), R=2.0 (measurement noise). Conservative —
smooths well but slow to follow fast changes. Tune R downward (e.g. 0.5) for
faster tracking response at the cost of more noise.

### Backup files (for reference during rebuild)

Located at `C:\Users\drowa\git\nodered-fix-backup\`:

| File | Contents |
|------|----------|
| `flows.live.pre-edit.json` | Last known-good snapshot (208 nodes, pre-fix attempt) |
| `espnow.capture.txt` | 60-second raw MQTT capture from all 3 devices |
| `test_functions.js` | Unit test harness (sticky-label logic) — passes 3/3 |
| `patch_flows.py` | The patch script that generated the broken attempts — keep for reference on what NOT to do |

---

## 8. What Not to Do in the Rebuild

1. **Do not** fan out `espnow_mqtt_in` to both discover and filter in parallel.
   The filter will never see enriched labels.

2. **Do not** rely on `flow.context` for any state that must survive a Node-RED
   deploy without configuring file-backed context storage first.

3. **Do not** set `resendOnRefresh: true` on any form that triggers a chart
   `{action:"clear"}`. This wipes the chart on every dashboard page load.

4. **Do not** push with `full` deployment type during iterative testing — it
   causes MQTT reconnect churn on all devices. Use `nodes` or `flows` type.
   But remember: even `nodes` deploys clear in-memory flow context.

5. **Do not** use a single mega `ui-group` for buttons + chart + table. Each
   Dashboard 2.0 group renders as its own card; mixing controls and data
   visualisation in one card gives poor layout on any viewport.
