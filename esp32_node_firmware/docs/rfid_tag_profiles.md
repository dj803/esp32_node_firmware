# RFID Tag Profiles

Single source of truth for the **RFID Playground** feature introduced in
v0.3.17. Firmware (`include/rfid_types.h`, `include/rfid.h`) and Node-RED
(`node-red/flows.json` → RFID Playground tab) must agree on every value in
this document. Update this file **before** touching either side.

---

## Profile dictionary

| profile                 | Family              | Block/page size | Auth              | Sector trailers | MFRC522 call family            |
| ----------------------- | ------------------- | --------------: | ----------------- | --------------- | ------------------------------ |
| `mifare_classic_1k`     | MIFARE Classic 1K   | 16 B block      | Key A (+ Key B)   | blocks 3,7,…,63 | `PCD_Authenticate` + `MIFARE_Write` |
| `mifare_classic_4k`     | MIFARE Classic 4K   | 16 B block      | Key A (+ Key B)   | up to block 255 | `PCD_Authenticate` + `MIFARE_Write` |
| `ntag21x` *(future)*    | NTAG213/215/216     | 4 B page        | none (optional PWD) | none          | `MIFARE_Ultralight_Write`     |
| `mifare_ul` *(future)*  | MIFARE Ultralight C | 4 B page        | 3DES (optional)   | none            | `MIFARE_Ultralight_Write` + auth |
| `ntag424_dna` *(future)* | NTAG424 DNA        | variable        | AES secure messaging | n/a          | NTAG secure-channel frames     |
| `unknown`               | unrecognised SAK    | —               | —                 | —               | read-only passthrough          |

The firmware's `rfidPiccToProfile()` (in `rfid.h`) maps the MFRC522v2
`PICC_Type` enum (decoded from SAK on SELECT) to one of these strings and
publishes it alongside the existing `card_type` field on `.../telemetry/rfid`.

**v0.3.17 write support: `mifare_classic_1k` and `mifare_classic_4k` only.**
NTAG paths are wired through the state machine (so profile parsing works) but
rejected by the write executor with `status:"write_failed"` until a later
release adds page-level support.

---

## Default keys

| profile               | Factory Key A      |
| --------------------- | ------------------ |
| `mifare_classic_1k`   | `FFFFFFFFFFFF`     |
| `mifare_classic_4k`   | `FFFFFFFFFFFF`     |

Used when the `cmd/rfid/program` / `cmd/rfid/read_block` payload omits
`key_a_hex`. Default-key use is allowed but logged as a WARN in the device
Serial because a freshly programmed tag should eventually have a site-specific
key (out of scope for v0.3.17 — see `docs/SUGGESTED_IMPROVEMENTS.md`).

---

## MQTT topics

All topics live under the device's ISA-95 path:
`Enigma/JHBDev/Office/Line/Cell/ESP32NodeBox/<uuid>/…`

### Firmware subscribes

| Topic                   | QoS | Retain | Purpose                                                      |
| ----------------------- | :-: | :----: | ------------------------------------------------------------ |
| `cmd/rfid/whitelist`    | 1   | no     | Pre-existing add/remove/clear/list of authorised UIDs        |
| `cmd/rfid/program`      | 1   | **no** | Arm: next scanned tag gets written (block-level, raw hex)    |
| `cmd/rfid/read_block`   | 1   | **no** | Arm: next scanned tag gets one block/page read               |
| `cmd/rfid/cancel`       | 1   | no     | Clear any pending arm                                        |

> Retain = **false** on the arm topics because arming is a one-shot action.
> A retained payload would re-arm every node that reconnects.

### Firmware publishes

| Topic             | Retain | Purpose                                                        |
| ----------------- | :----: | -------------------------------------------------------------- |
| `telemetry/rfid`  | no     | Scan event — now carries `profile` alongside `card_type`       |
| `response`        | no     | Program / read / cancel outcomes, paired by `request_id`       |
| `status`          | boot only | Heartbeat + boot — now carries `rfid_enabled` capability flag |

---

## Request / response schemas

### `cmd/rfid/program`

```json
{
  "profile":    "mifare_classic_1k",
  "writes": [
    { "block": 4, "data_hex": "00112233445566778899AABBCCDDEEFF",
      "key_a_hex": "FFFFFFFFFFFF" }
  ],
  "timeout_ms": 15000,
  "request_id": "rfid-prog-1729876543210"
}
```

- `writes[]` — up to `RFID_MAX_WRITE_BLOCKS` (8) entries.
- `data_hex` length must equal the profile block size × 2 (32 chars for
  MIFARE Classic, 8 chars for NTAG21x). The firmware rejects the whole batch
  if any row fails this check.
- `key_a_hex` is optional — omit to use the factory default.
- `timeout_ms` 0 → use `RFID_PROGRAM_TIMEOUT_MS` (15 000).

### `cmd/rfid/read_block`

```json
{
  "profile":    "mifare_classic_1k",
  "block":      4,
  "key_a_hex":  "FFFFFFFFFFFF",
  "timeout_ms": 15000,
  "request_id": "rfid-read-1729876543999"
}
```

### `cmd/rfid/cancel`

```json
{}
```

Body is ignored — publishing anything to the topic clears the pending arm.

### Response on `.../response`

```json
{
  "event":          "rfid_program",
  "request_id":     "rfid-prog-1729876543210",
  "uid":            "AB:CD:EF:01",
  "profile":        "mifare_classic_1k",
  "status":         "ok",
  "blocks_written": [4]
}
```

- `event` ∈ { `rfid_program`, `rfid_read_block` }.
- `status` ∈ { `ok`, `auth_failed`, `write_failed`, `trailer_guard`,
  `timeout`, `cancelled` }.
- Read responses add `"data_hex": "…"` on `status:"ok"`.
- A request that times out before any card is scanned returns `uid:""`.

---

## Sector-trailer guard

MIFARE Classic sector trailers sit at `block % 4 == 3` (blocks 3, 7, 11, …).
They hold Key A, access bits, Key B — a wrong write can permanently brick
the tag.

The firmware **refuses** any write targeting a trailer **before touching the
reader**, returning `status:"trailer_guard"` without touching any other
block. The guard is hard-wired — there is no MQTT or config override. If
trailer writes are ever needed they will land as a separate, explicitly
opted-in `cmd/rfid/set_key` handler (not in scope for v0.3.17).

See host unit tests in `test/test_native/test_main.cpp` —
`test_rfid_trailer_guard_*`.

---

## Heartbeat capability flag

Every `.../status` publish (boot, heartbeat, etc.) now carries
`"rfid_enabled": true|false` reflecting the compile-time `RFID_ENABLED` in
`config.h`. The Node-RED RFID Playground UI uses this to populate its
reader dropdown — nodes without an MFRC522 are hidden so the operator can't
arm a program on a reader-less device.
