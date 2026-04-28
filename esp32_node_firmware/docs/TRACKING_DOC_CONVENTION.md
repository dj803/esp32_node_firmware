# Tracking-document convention

How to manage long-lived "improvements / backlog / progress" documents in
this repo. Born from the 2026-04-27 sweep that split the 3070-line
`SUGGESTED_IMPROVEMENTS.md` into a short index + a full archive.

Apply this pattern to any document that:

- Accumulates entries across many sessions.
- Mixes open/active items with resolved/historical items.
- Has grown long enough that "what's still open?" requires scrolling past
  large resolved-entry narratives.

Examples in this repo today: `SUGGESTED_IMPROVEMENTS.md` +
`SUGGESTED_IMPROVEMENTS_ARCHIVE.md`. Future candidates: `ROADMAP.md`
(already partially follows this with "Now / Next / Month / v0.5.0+"
sections). Re-evaluated against this convention 2026-04-28:
`ESP32_FAILURE_MODES.md` and `memory_budget.md` are reference
catalogues without an open/resolved item lifecycle, so they do NOT
fit (see #82 in the backlog archive).

---

## The pattern

**Two files:**

1. **`<NAME>.md` — INDEX** (short, scan-friendly).
   - One line per entry: `#NN  <title>` plus optional date/version hint.
   - OPEN section at the top.
   - RESOLVED section at the bottom (last 30-50 entries; older still in archive).
   - WONT_DO section between OPEN and RESOLVED for items intentionally
     parked (cross-referenced to `docs/WONT_DO.md` which holds the rationale).
   - Header explains the convention so a new contributor doesn't need to
     read this convention file to understand the structure.

2. **`<NAME>_ARCHIVE.md` — ARCHIVE** (full text, append-only).
   - Every entry's full body — observation, proposal, severity, status.
   - Stable entry numbers — never renumber, gaps are fine.
   - Resolved entries stay here with appended `STATUS: RESOLVED YYYY-MM-DD in vX.Y.Z`.
   - WONT_DO entries get `STATUS: WONT_DO YYYY-MM-DD — moved to docs/WONT_DO.md entry N.`

---

## Workflow

**To add a new entry:**

1. Pick the next entry number (= max existing + 1, regardless of resolved gaps).
2. Append the full entry to the archive file.
3. Add a one-line summary to the INDEX under OPEN.

**To resolve an entry:**

1. Append a `STATUS: RESOLVED YYYY-MM-DD in vX.Y.Z` line to the entry's
   body in the archive file. Don't delete the original observation/proposal —
   future contributors searching for the same problem need the context.
2. In the INDEX, move the summary line from OPEN to RESOLVED. Keep the
   number unchanged.

**To resolve and stop tracking** (rare — the resolution is fully baked into
code/process and unlikely to recur):

1. Same as resolve, but optionally remove from the INDEX after a quarter
   of dwell time. The archive entry remains forever.

**Periodic sweeps** (~every 30 entries or every quarter):

1. Re-read open entries; if any have been silently addressed (e.g. a
   refactor obviated them), append a STATUS line and move to RESOLVED.
2. Truncate the INDEX's RESOLVED section to the last ~30 entries; older
   ones still searchable in archive.
3. Bump the "Last sweep:" date in the INDEX header.

---

## When to start a fresh tracking doc vs extend an existing one

**Start a fresh doc** when:
- The new doc covers a clearly different *kind* of work (e.g.
  `CHAOS_TESTING.md` for tests is separate from `SUGGESTED_IMPROVEMENTS.md`
  for backlog).
- You're starting a new high-traffic concept (a refactor with many
  subtasks; a multi-version release plan).

**Extend an existing one** when:
- The new entry would naturally have been added there 6 months ago.
- Splitting would dilute the searchability ("which of the 3 backlog files
  is this in?").

---

## Numbering hygiene

Stable entry numbers are the lookup key. Treat them like primary keys:

- Don't reuse numbers when an entry is removed (the archive keeps it).
- If two contributors add #N concurrently and collide on a merge,
  RENUMBER the later one to N+1, N+2, ... and update both files. Don't
  leave duplicates.
- Today's lesson: if the gap between sessions is long, **check max(N)
  before adding**. The 2026-04-27 cascade session collided with the
  2026-04-26 audit session because the agent added new entries from #63
  upward without checking that the audit had already used #63-#70.

---

## When NOT to apply this pattern

- **Documents under 200 lines.** Splitting adds friction without payoff.
- **Documents with no resolution lifecycle** (e.g. design notes, architecture
  decisions). Those just grow chronologically; no resolved/open distinction.
- **Documents read top-to-bottom** (a TWDT_POLICY explanation, an analysis
  doc). The index is most useful for entries that are independently
  searchable units.

---

## Folder layout (after 2026-04-28 sweep)

The `docs/` directory is organised by lifecycle, not by topic:

```
docs/
├── README.md              — index of every doc, grouped by purpose
├── *.md                   — active reference + active backlog (top level)
├── SESSIONS/              — point-in-time session reports (ESPNOW review,
│                            RF config test, BLE coexistence analysis,
│                            cascade-fix daily logs, etc.)
├── archive/               — pre-v0.4.x plans superseded by current docs
│                            (DEFERRED_IMPROVEMENTS, IMPROVEMENT_PLAN,
│                            FIXES_LOG). Search-only; no active maintenance.
└── BusinessDocs/          — business-case writeups (separate concern)
```

When adding a doc, decide: is it ACTIVE (read for current decisions, top
level), POINT-IN-TIME (a snapshot of one session's findings, `SESSIONS/`),
or HISTORICAL (superseded reference, `archive/`)?

Most docs start at top level. Move to `SESSIONS/` if the doc's relevance
expires when the underlying session/incident closes. Move to `archive/`
if the doc has been replaced by a different active doc and the original
is kept only for grep/audit reasons.

### Date-stamped audits and incident reports go in `SESSIONS/`

If the filename includes a date (e.g. `WDT_AUDIT_2026_04_28.md`,
`RF_CONFIG_TEST_2026_04_25.md`, `BLE_COEXISTENCE_ANALYSIS.md`) it is
almost certainly a point-in-time artefact. The findings are valid as
of the audit date; future contributors will need a fresh audit to
establish current state. Put it in `SESSIONS/` from the start.

The opposite — convention docs, policy docs, subsystem reference,
and architecture spec stay at top level and are amended in place.
TWDT_POLICY.md is a pattern doc; WDT_AUDIT_2026_04_28.md is a snapshot
that *uses* TWDT_POLICY's pattern.

### When `*.md` at top level becomes a SESSIONS/ candidate

If a top-level doc was originally written for the current state but
is now historically dated — e.g. a "v0.4.10 stability investigation"
that's been superseded by the closure of #51 — treat it the same as
a stale-from-the-start session report. `git mv` to `SESSIONS/` and
update the index in `docs/README.md` + any cross-references.

## Reference implementation

See:
- `docs/SUGGESTED_IMPROVEMENTS.md` (INDEX after 2026-04-27 split)
- `docs/SUGGESTED_IMPROVEMENTS_ARCHIVE.md` (ARCHIVE)
- Commit 81b70e1 (the split itself)
- Commit c111f61 (`.txt` → `.md` rename for the three tracking docs)
- Commit (this one) — the SESSIONS/ + archive/ subfolder reorg.
