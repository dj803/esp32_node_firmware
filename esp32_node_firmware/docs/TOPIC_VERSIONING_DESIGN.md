# Versioned MQTT topic prefixes — design proposal

> Design reference for #33 in [SUGGESTED_IMPROVEMENTS](SUGGESTED_IMPROVEMENTS.md).
> NOT yet shipped. Captured 2026-04-28 so the strategy is documented when
> the time comes (tied to a major firmware bump, fleet > 10 devices, or a
> known-breaking schema change). No firmware change today.

## The problem

Today's topics are unversioned:

```
Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/status
Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/cmd/...
Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/diag/coredump
Enigma/JHBDev/Office/Line/Cell/health/daily
```

The JSON payload schema on `/status` has drifted across firmware
versions — every release that adds a heartbeat field (`mqtt_disconnects`
in v0.4.23, `relay_enabled`/`hall_enabled` in v0.5.0 Phase 1,
`restart_cause` in v0.4.21, `last_restart_reasons` in v0.4.23) is a
schema change that any consumer (Node-RED dashboard, daily-health
script, Grafana ingester) has to tolerate.

So far we've stayed strictly additive: new fields are added with safe
defaults, existing fields keep their semantics. Consumers using
`payload.get("field", default)` style parsing don't break. But that
discipline is operator-enforced not protocol-enforced; a future
breaking change would silently propagate to every consumer the moment
any one device boots the new firmware.

The asymmetry is also painful: an old consumer reading a new device's
heartbeat sees more fields than it knows how to render (benign — just
ignored). A new consumer reading an old device's heartbeat sees fewer
fields than it expects (potentially breaking — null-pointer / undefined
references). Without a version marker on the wire, the consumer has to
sniff the firmware_version field on every payload to decide which
schema to apply.

## What "versioned topic prefix" buys us

A version segment in the topic path:

```
v1/Enigma/JHBDev/.../status      ← v1 schema (current additive baseline)
v2/Enigma/JHBDev/.../status      ← future breaking-change rev
```

…lets a consumer subscribe to exactly one schema version it understands.
Devices on v1 publish to `v1/...`; consumers handling v1 subscribe to
`v1/...`; the broker handles the routing transparently. No payload
sniffing required.

Crucially, BOTH versions can coexist on the broker during a rollout:
- Stage 1: All devices + consumers on v1
- Stage 2: A canary device flashes v2 firmware → publishes to v2/...
- Stage 3: A canary consumer subscribes to v2/...
- Stage 4: When canary is happy, rollout v2 firmware to fleet
- Stage 5: When fleet is on v2, retire v1 consumers

Without versioning, stages 2-5 collapse into a single hot-cut where the
fleet flips schemas synchronously and any consumer that hasn't been
updated breaks immediately.

## Strategy options

### Option A — top-level version segment (`v<N>/<existing-tree>`)

```
v1/Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/status
```

**Pros:**
- Cleanest separation — one MQTT subscribe filter per schema version
- Topic-tree wildcards (`v1/+/+/+/+/+/+/status`) match cleanly
- No payload schema change

**Cons:**
- Every device-topic + every broadcast-topic gets the prefix
- The site-wide `Enigma/JHB-Dev/broadcast/cred_rotate` becomes
  `v1/Enigma/JHBDev/broadcast/cred_rotate` (or stays unversioned —
  broadcast schemas change rarely)
- Requires NVS migration: `gAppConfig.mqtt_enterprise` is the current
  top-level segment; we'd add `gAppConfig.topic_version_prefix`
  defaulting to `v1` for new builds and empty for legacy
- Existing retained messages are retained on the OLD topic and
  invisible to v2 subscribers. Either drain old retained or accept
  a one-time loss of retained state during the cut.

### Option B — sub-tree version segment (`<root>/v<N>/<rest>`)

```
Enigma/v1/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/status
```

**Pros:**
- Top-level enterprise segment stays put; ACLs / cross-site routing
  unchanged
- Site-aware migrations (different sites can be on different schema
  versions during rollout)

**Cons:**
- Less natural — version is a global protocol concern, not a per-site
  concern
- Wildcard `Enigma/+/JHBDev/...` works but is awkward

### Option C — payload-side version field, keep topics unversioned

```jsonc
{
  "schema_version": 2,
  "device_id": "...",
  ...
}
```

**Pros:**
- Zero topic-tree changes
- Easy to add today (one extra field)

**Cons:**
- Every consumer still has to sniff and dispatch by version
- Doesn't solve the "broker can't route by version" problem — broker
  serves all schemas mixed together, consumer's filter is a runtime
  payload check, not a subscribe-time MQTT filter
- This is the "additive only, document the rules" approach we've been
  doing implicitly. Making `schema_version` explicit gives consumers a
  cleaner discriminator but doesn't unblock breaking changes.

### Recommendation: **Option A** when the time comes.

Top-level prefix is the Kubernetes / GitHub API / AWS API convention
for a reason — it's the one place a consumer can split routing by
schema version with a single subscribe filter and zero payload parsing.
The deployment cost (NVS migration, retained drain, dual-publish
window) is the price of admission and is paid once.

## Migration plan (Option A)

1. **Add `topic_version_prefix` to AppConfig** (NVS-backed). Default
   value depends on firmware version:
   - Pre-v1.0 firmware: empty string (current unversioned behaviour)
   - v1.0+: `"v1"`
   - On NVS load, devices that don't have the field default to empty
     so existing fleet keeps publishing to existing topics until they
     OTA into a build that knows about the field.

2. **Add `mqttTopic()` builder support** in `mqtt_client.h`. Today
   it returns `<enterprise>/<site>/.../<uuid>/<prefix>`. New behaviour:
   if `gAppConfig.topic_version_prefix` is non-empty, prepend it.

3. **Dual-publish during rollout**: a transitional firmware build
   publishes BOTH to `v0/<old>/...` (legacy unversioned consumers) and
   `v1/<old>/...` (new consumers). Adds wire bandwidth + heap pressure
   per publish — only run dual-publish during the migration window,
   not in steady state. Gate with build flag `TOPIC_DUAL_PUBLISH`.

4. **Node-RED bridge** subscribes to legacy `Enigma/...` and republishes
   to `v1/Enigma/...`. Deploy at the start of the migration window
   so v1-only consumers see a populated v1 tree even before any device
   has flashed the dual-publish firmware. Retire after the fleet OTA
   is complete and consumers have been migrated.

5. **Site-wide broadcast topics** (cred_rotate, OTA URLs) — version
   them too but with a longer overlap window since broadcast
   subscribers are harder to enumerate. Leave the legacy broadcast
   topic active for at least one full release cycle after the v1
   broadcast topic ships.

6. **Retained-message drain**: at end of migration, manually clear
   retained messages on the legacy topic tree (publish empty payload
   with retained flag). Schedule for a planned maintenance window —
   fleet should be quiet, all devices publishing on v1 only.

## When to ship

Triggers (any one):

- **Fleet > 10 devices** — the rollout cost grows linearly but the
  schema-drift risk grows quadratically with the number of consumers.
- **First known-breaking schema change** — a field rename, type
  change, or removal where additive-only doesn't cover. Today every
  release has been additive-only; the first non-additive change is
  the right moment to bundle topic versioning with it.
- **Major firmware version bump (v1.0)** — natural cut point even if
  the schema isn't changing. Operator expectation aligns with topic
  versioning at major-version boundaries.

Until one of those triggers fires, the implicit "additive-only" rule
keeps working. Document this rule in `docs/MQTT_SCHEMA_DISCIPLINE.md`
(future doc; not in scope today) so contributors don't accidentally
land a breaking change.

## Variant-build interaction (#71)

The variant infra (`esp32dev_minimal`, `esp32dev_relay_hall`,
`esp32dev_ble_bench`) compiles different feature sets. Heartbeat
fields are gated by `#ifdef RFID_ENABLED` etc. so a variant publishes
fewer fields — that's an additive-style variation today.

If a variant needs a topic-tree change (e.g. relay-controller variant
publishes to `<root>/relay/<uuid>/cmd/...` instead of
`<root>/ESP32NodeBox/<uuid>/cmd/...`), that is a topic-tree change
and warrants its own version bump independent of schema versioning.
The topic_version_prefix mechanism above generalises cleanly to that
case — variant builds set their own prefix at compile time via build
flag.

## What this doc does NOT propose

- A specific schema migration. There's no breaking schema change
  pending today — this is forward-planning infrastructure.
- A timeline. The "when to ship" triggers above are conditions, not
  dates.
- A `docs/MQTT_SCHEMA_DISCIPLINE.md` companion doc. That's the next
  doc to write, but only when there's a non-trivial schema rule to
  capture beyond "additive only".

## Reading list (for whoever picks this up)

- AWS IoT Core's reserved topic-prefix scheme (`$aws/things/...`) — same
  problem class, different solution
- GitHub API's `Accept: application/vnd.github.v3+json` — header-based
  versioning, doesn't translate to MQTT
- Stripe's API versioning — date-stamped versions, opt-in per consumer.
  Closest analogue if we ever need consumer-driven schema choice.
