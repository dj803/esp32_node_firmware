# ESP-NOW Tracking — UX Review
**Date:** 2026-04-24  
**Perspective:** Senior UX Designer reading the Technical Specification  
**Spec reviewed:** `docs/TECHNICAL_SPEC.md` (updated 2026-04-24 against v0.4.04)

---

## What the system is

The ESP-NOW tracking system is a **passive indoor distance-measurement network**
between ESP32 nodes. Each node continuously broadcasts a small radio beacon
(~3 s, randomised timing). Every node that hears any broadcast — from any node,
for any reason — silently records how strong the signal was. That signal strength
is converted into a distance estimate using physics (weaker signal = further
away), smoothed to reduce noise, and published to a dashboard every 2 seconds.

No external infrastructure is required. No GPS. No beacons. No cameras. The
nodes measure each other.

---

## What an operator can actually do

The experience breaks into four distinct operator tasks.

---

### 1. Watch — live peer distance monitoring

**What it gives you:**  
A real-time chart and table showing which nodes can "see" each other and how
far apart they are estimated to be. Up to 8 peers per node, refreshed every
2 seconds. Each entry shows raw signal strength, smoothed signal strength,
estimated distance in metres, and a count of how many noisy readings were
discarded.

**What it is good for:**
- Confirming that devices are communicating with each other
- Spotting if a device has moved, gone offline, or is being obstructed
- Getting a rough live distance reading without any external infrastructure

**How to use it:**  
Open the ESP-NOW Tracking tab. Devices that have heard each other appear in
the Peer Table automatically. Tick the checkboxes next to the devices you
want to chart, then click **Track Selected**. The distance chart draws one
line per selected device and keeps updating every 2 seconds.

**Current friction:**  
If Node-RED is redeployed for any reason, the device selection is wiped and
the chart goes blank. The operator must re-tick and re-click after every
deploy. This is a known software limitation — see Section on UX gaps.

---

### 2. Calibrate — tune accuracy for your physical environment

**What it gives you:**  
A three-step wizard to measure the actual signal behaviour in your specific
room or building, rather than relying on generic factory defaults.

**Why this matters:**  
Accuracy with the defaults (−59 dBm reference at 1 m, path-loss exponent
2.5) is roughly correct in an open-plan office. In a room with brick walls,
server rack metal, or unusual antenna orientation, the defaults can be off
by 50–100%. Calibration can bring this down to 10–20% error in a controlled
environment.

**The three steps:**

| Step | What you do | What the firmware does |
|---|---|---|
| **measure_1m** | Place the node exactly 1 m from the target. Click. | Collects 30 raw RSSI samples, takes the median. Publishes the reference signal level. |
| **measure_d** | Move the node to a known distance (e.g. 4 m). Click. | Collects 30 more samples. Computes how quickly signal drops off in your environment. |
| **commit** | Review the two numbers. If they look reasonable, click Commit. | Saves both values to on-device storage. Survives reboots. Takes effect immediately. |

A **reset** command restores factory defaults at any time.

**Physical requirements:**
- Line-of-sight or representative path between the two nodes during
  measurement
- The distance must be measured accurately (tape measure, not estimated)
- Two-person job if nodes are ceiling-mounted
- Devices must stay still during the 30-sample collection window (~30 s
  per step)

**Current friction:**
- No remote calibration is possible — requires physical presence
- If MQTT disconnects during collection, progress is silently lost and the
  step times out after 2 minutes with no operator feedback
- No dashboard indicator shows whether a device has ever been calibrated
  or is running on defaults

---

### 3. Position — 2-D map of where mobile nodes are

**What it gives you:**  
If at least 3 nodes have known fixed positions (anchors), Node-RED
triangulates where the remaining (mobile) nodes are on a 2-D floor plan.
The Map tab shows this as a live dot-on-grid visualisation, updated as
distances change.

**Setup:**  
Designate fixed nodes as anchors and enter their real-world X/Y coordinates
(in metres) via the dashboard. These coordinates are stored on the device
and included in every status heartbeat, so Node-RED always knows where
the anchors are.

**Requirements for positioning:**

| Anchors | Result |
|---|---|
| 0–2 | No position estimate possible |
| 3 (non-collinear) | 2-D estimate, lower accuracy |
| 4 (convex polygon around tracking area) | Recommended minimum for reliable 2-D |

**Current friction:**
- High setup cost: measure the room, place anchors at known positions,
  calibrate each link
- No placement guidance in the dashboard (no "is this a good anchor
  layout?" check)
- No map preview before committing anchor coordinates
- The position estimate jitters visibly even for stationary devices —
  RSSI noise causes the dot to drift by 0.2–0.5 m continuously
  (Kalman motion smoothing on the map position is a planned improvement)

---

### 4. Tune — adjust noise filtering

**What it gives you:**  
Two runtime parameters that control how aggressively the firmware filters
noisy signal readings. Applied per device, persisted to on-device storage.

| Parameter | Default | Range | Effect |
|---|---|---|---|
| **EMA alpha** | 0.30 | 0.01 – 0.99 | How much weight the latest reading gets vs. history. Low = very smooth but slow to respond. High = responsive but noisier. |
| **Outlier gate** | 15 dB | 0 – 30 dB | A reading that deviates more than this from the current average is discarded. Low = aggressive rejection. High = permissive. |

**How to use it:**  
Change a value, watch the distance chart for 30–60 seconds, and assess
whether the line is smoother or more responsive. Repeat.

**Current friction:**  
No visualisation of the trade-off. No live preview. No guidance on what
"good" looks like for a given use case. Entirely trial-and-error.

---

## Capability ceiling

| Capability | Status | Notes |
|---|---|---|
| Real-time distance between any pair of nodes | ✅ | 2 s refresh, up to 8 peers per node |
| Distance trend chart over time | ✅ | Kalman-filtered in Node-RED |
| Bidirectional pair distance with asymmetry margin | ✅ | Pair Chart tab |
| 2-D position estimate (mobile nodes) | ✅ | Requires ≥ 3 calibrated anchors |
| Per-installation accuracy calibration | ✅ | Manual 3-step wizard |
| Filter: show only selected devices on chart | ✅ | Persisted per-device in firmware |
| Friendly device names | ✅ | Stored on-device, shown in all payloads |
| Works without GPS or external infrastructure | ✅ | Self-contained, LAN only |
| Typical accuracy (calibrated, office) | ~10–20% | e.g. ±0.3–0.5 m at 3 m |
| Typical accuracy (factory defaults, office) | ~30–60% | Highly environment-dependent |
| Maximum simultaneous peers per node | 8 | Hard limit; oldest evicted when full |
| Works through walls | Partially | Degrades accuracy; calibrate in situ |
| Sub-1-second update rate | ❌ | Minimum 2 s; RSSI averaging requires time |
| Works when WiFi channel changes | ❌ | ESP-NOW is channel-pinned to the WiFi AP |
| GPS-class absolute accuracy | ❌ | RSSI-based methods are inherently noisy |

---

## UX gaps — confirmed by spec

These are not opinions. Each is documented as a known limitation in the spec
or the rebuild notes.

### Gap 1 — Chart resets on every Node-RED redeploy *(high impact)*

The device selection ("Track Selected") is stored in Node-RED's in-memory
flow context. Every Node-RED deploy — even a developer pushing a small
unrelated change — wipes this state. The chart goes blank. The operator
must re-open the tab, re-tick the device checkboxes, and click Track
Selected before data appears again.

In an active development environment this happens multiple times per day.
It is the single biggest source of operator confusion ("the chart stopped
working").

**What would fix it:** Auto-reload the tracked MAC list from the retained
MQTT message (`cmd/espnow/track`) that the firmware broadcasts on startup.
Node-RED already has the message — it just needs a startup inject node to
read it. One flow change, no firmware change.

---

### Gap 2 — No calibration status visible *(medium impact)*

There is no way to tell from the dashboard:
- Whether a given device has been calibrated or is using factory defaults
- What the stored calibration constants are
- When calibration was last performed
- Whether the calibration is likely to be accurate (no sanity-check output)

An operator inheriting a deployed system has no way to audit calibration
state without querying the device directly via MQTT.

**What would fix it:** Include `espnow_tx_power_dbm` and
`espnow_path_loss_n` in the `status` heartbeat (they are already in NVS).
Node-RED could then show a "calibrated ✓" badge on the peer table, or
highlight devices still on defaults.

---

### Gap 3 — Map jitter makes positioning feel broken *(medium impact)*

A node sitting completely still on a table will appear to drift 0.2–0.5 m
continuously on the Map tab. This is normal RSSI variance, but to an
operator it looks like the system is broken or inaccurate.

**What would fix it:** Kalman smoothing on the 2-D position estimate in
the Node-RED map function node (no firmware change needed). Already listed
as a future improvement in the spec.

---

### Gap 4 — No alert when a tracked peer goes offline *(medium impact)*

When a node goes silent for 15 seconds, it is evicted from the peer table
and its line disappears from the chart. There is no notification, no status
change, no alert. The operator only notices if they are watching the chart
at that moment.

**What would fix it:** Add a Node-RED `trigger` node on the espnow topic:
if a previously-seen peer stops sending for > 20 s, emit a dashboard
notification or change the peer table row to a "last seen X seconds ago"
state.

---

### Gap 5 — Calibration wizard has no in-dashboard UI *(medium impact)*

The spec describes a three-step calibration wizard as a dashboard feature.
In the current implementation the wizard exists as MQTT command handling
in the firmware — but the Node-RED UI for driving it step-by-step is not
yet built. An operator who wants to calibrate must send raw MQTT messages
manually (via `mosquitto_pub` or similar), which requires technical
knowledge most operators do not have.

**What would fix it:** A Node-RED dashboard panel with three buttons
(Measure 1m / Measure at distance / Commit), an input for the known
distance, and a live progress bar reading from the `response` topic.

---

### Gap 6 — Anchor setup has no visual validation *(low impact)*

When setting anchor coordinates, the operator types X/Y numbers into a
form. There is no preview of where the anchor would appear on the map,
no check that the placement forms a valid convex polygon, and no warning
if two anchors are given the same coordinates.

**What would fix it:** Show anchor positions on the map in a "setup mode"
before committing, with a polygon overlay and a ≥ 3 anchors / convex
shape indicator.

---

### Gap 7 — Filter and `peer_count` field disagree *(low impact, data)*

When the MAC tracking filter is active, the MQTT payload shows
`peer_count: 8` (all devices in range) but `peers: [...]` contains only
the 2 filtered devices. Any consumer using `peer_count` to interpret the
data will misread the state. The correct approach is to use
`peers.length`.

**What would fix it:** Firmware change to publish `tracked_count`
(filtered) alongside `peer_count` (total). Planned for a future revision.

---

## Summary for rebuild prioritisation

If the ESP-NOW Tracking Node-RED tab is being rebuilt from scratch, the UX
gaps in priority order:

1. **Auto-reload tracked MACs on startup** — eliminates the chart-goes-blank
   frustration after every deploy. One inject node + one function node.
2. **Calibration wizard panel** — makes calibration accessible to non-technical
   operators. A form + 3 buttons + progress display.
3. **Peer offline alert** — simple trigger node; high operator value.
4. **Calibration status badge in peer table** — requires firmware to include
   cal constants in heartbeat first.
5. **Map position Kalman smoothing** — pure Node-RED function change.
6. **Anchor placement validator** — nice-to-have for commissioning UX.
