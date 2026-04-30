#!/usr/bin/env bash
# tools/dev/end-of-session-sweep.sh — checklist surfacer for autonomous
# session close. Walks five checks from SUGGESTED_IMPROVEMENTS #85
# sub-B/sub-D and prints a single-screen TODO that the agent works
# through before declaring the session done.
#
# This is a SURFACER, not a fixer. It identifies candidates; the agent
# decides whether each is real work or a false positive.
#
# Usage (from repo root or anywhere — script resolves the repo via the
# script's own location):
#   tools/dev/end-of-session-sweep.sh
#
# Exit 0 means "no candidates surfaced" (clean session close). Exit 1
# means "review the printed list".

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FW_DIR="$REPO_ROOT/esp32_node_firmware"
DOCS_DIR="$FW_DIR/docs"
ROOT_CLAUDE="$REPO_ROOT/CLAUDE.md"
INDEX_FILE="$DOCS_DIR/SUGGESTED_IMPROVEMENTS.md"
ARCHIVE_FILE="$DOCS_DIR/SUGGESTED_IMPROVEMENTS_ARCHIVE.md"
ROADMAP_FILE="$DOCS_DIR/ROADMAP.md"
README_FILE="$DOCS_DIR/README.md"

CANDIDATES=()

# ── Check 1: CLAUDE.md fleet-table version vs latest release tag ─────────────
check_fleet_version() {
  local fleet_ver tag_ver
  fleet_ver="$(grep -oE 'firmware v[0-9]+\.[0-9]+\.[0-9]+' "$ROOT_CLAUDE" | head -1 | sed 's/firmware //')"
  tag_ver="$(git -C "$REPO_ROOT" tag --list 'v*' --sort=-creatordate | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1)"
  if [ -z "$fleet_ver" ] || [ -z "$tag_ver" ]; then
    CANDIDATES+=("CLAUDE.md fleet-version or git tag could not be read — manually verify")
    return
  fi
  if [ "$fleet_ver" != "$tag_ver" ]; then
    CANDIDATES+=("CLAUDE.md fleet-table is on $fleet_ver but latest tag is $tag_ver — bump the heading or confirm fleet hasn't been OTA'd yet")
  fi
}

# ── Check 2: ROADMAP "Now" section covers every release tag ──────────────────
check_roadmap_coverage() {
  # Extract the section between "## Now" and the next H2.
  local now_section
  now_section="$(awk '/^## Now/,/^## Next/' "$ROADMAP_FILE")"
  # Find every tag in repo and check whether the Now section mentions it.
  local missing=()
  while IFS= read -r tag; do
    if ! echo "$now_section" | grep -qF "$tag"; then
      missing+=("$tag")
    fi
  done < <(git -C "$REPO_ROOT" tag --list 'v0.4.*' --sort=-creatordate | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -8)
  if [ ${#missing[@]} -gt 0 ]; then
    CANDIDATES+=("ROADMAP 'Now' section missing tags: ${missing[*]} — add a DONE entry per tag or confirm older tags are intentionally aged-out")
  fi
}

# ── Check 3: Archive RESOLVED entries that OPEN-index still lists ────────────
check_resolved_in_open() {
  # Pull every entry that has 'STATUS:.*RESOLVED' in the archive — these
  # are the entry numbers we expect to NOT appear under '## OPEN' in the
  # index. Walk the archive sequentially: track the current entry number
  # via lines matching ^N\. ; flag the number if its block contains a
  # RESOLVED status line.
  #
  # CAVEAT: archive entries have narrative bodies that sometimes
  # cross-reference other items via inline "STATUS:" lines (e.g. a
  # parent entry mentioning a sub-finding). To reduce false positives,
  # only flag the entry if the OPEN-index line is BARE — no
  # parenthetical that already acknowledges partial resolution
  # ("shipped", "resolved", "kept open until X", "validation pending").
  local resolved_nums
  resolved_nums="$(awk '
    /^[0-9]+\. / { if (cur && hit) print cur; cur=$1; sub(/\./, "", cur); hit=0; next }
    /STATUS:.*RESOLVED/ { hit=1 }
    END { if (cur && hit) print cur }
  ' "$ARCHIVE_FILE")"
  local open_section
  open_section="$(awk '/^## OPEN/,/^## WONT_DO/' "$INDEX_FILE")"
  local stale=()
  for num in $resolved_nums; do
    local line
    line="$(echo "$open_section" | grep -E "^\s+#${num}\s" || true)"
    [ -z "$line" ] && continue
    # If the line already has a parenthetical signalling acknowledged
    # partial resolution, don't re-flag.
    if echo "$line" | grep -qiE 'shipped|resolved|kept open until|validation pending|pending fleet|pending first|partial fix|deferred'; then
      continue
    fi
    stale+=("$num")
  done
  if [ ${#stale[@]} -gt 0 ]; then
    CANDIDATES+=("Archive marks RESOLVED but OPEN index still lists (with no acknowledgement parenthetical): #${stale[*]} — move to RESOLVED section in index, or add a parenthetical explaining why it's still open")
  fi
}

# ── Check 4: docs/README.md indexes every doc under docs/ ────────────────────
check_readme_coverage() {
  # Every .md under docs/ (excluding README.md itself, archive subfolder,
  # and SESSIONS subfolder which has its own grouped entry).
  local docs_files readme_text missing=()
  docs_files="$(find "$DOCS_DIR" -maxdepth 1 -type f -name '*.md' ! -name 'README.md' -exec basename {} \;)"
  readme_text="$(cat "$README_FILE")"
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    if ! echo "$readme_text" | grep -qF "$f"; then
      missing+=("$f")
    fi
  done <<< "$docs_files"
  if [ ${#missing[@]} -gt 0 ]; then
    CANDIDATES+=("docs/README.md doesn't mention: ${missing[*]} — add to the appropriate section")
  fi
  # Same check for SESSIONS/ but only if the SESSIONS section in README
  # mentions session files individually.
  if [ -d "$DOCS_DIR/SESSIONS" ]; then
    local session_files missing_sessions=()
    session_files="$(find "$DOCS_DIR/SESSIONS" -maxdepth 1 -type f -name '*.md' -exec basename {} \;)"
    while IFS= read -r f; do
      [ -z "$f" ] && continue
      if ! echo "$readme_text" | grep -qF "$f"; then
        missing_sessions+=("$f")
      fi
    done <<< "$session_files"
    if [ ${#missing_sessions[@]} -gt 0 ]; then
      CANDIDATES+=("docs/README.md SESSIONS group missing: ${missing_sessions[*]}")
    fi
  fi
}

# ── Check 5: archive completeness — every #N referenced in the index ────────
# Filed 2026-04-29 after a drift pass found #54/#78/#96/#97 with index lines
# but no archive entry / no STATUS line. Convention: every #N in the index
# has a `^N\. ` entry in ARCHIVE.md, and every entry under "## RESOLVED" /
# "## WONT_DO" sections of the index has a `STATUS:` line in its archive
# entry. Catches both "filed-but-never-archived" and "resolved-in-index-
# only" drift.
check_archive_completeness() {
  # Pull the set of all #N tokens that appear at the start of a line in
  # the index (sorted, unique). These are the entries the index claims
  # exist.
  local index_nums archive_nums missing_archive=()
  index_nums="$(grep -oE '^\s+#[0-9]+\b' "$INDEX_FILE" | grep -oE '[0-9]+' | sort -un)"
  for num in $index_nums; do
    if ! grep -qE "^${num}\. " "$ARCHIVE_FILE"; then
      missing_archive+=("$num")
    fi
  done
  if [ ${#missing_archive[@]} -gt 0 ]; then
    CANDIDATES+=("Index references but ARCHIVE.md has no entry for: #${missing_archive[*]} — append the full-text entry per the convention header")
  fi

  # For every entry under the index's "## RESOLVED" or "## WONT_DO"
  # sections, the archive entry should have a STATUS line. (Same caveat
  # as Check 3: archive bodies sometimes mention STATUS for cross-
  # references, so we look for STATUS at column 4 (the entry's own
  # status footer convention) AND require it within ~80 lines of the
  # `^N\. ` line.)
  local resolved_section status_missing=()
  resolved_section="$(awk '/^## RESOLVED/,EOF' "$INDEX_FILE")"
  while IFS= read -r line; do
    local n
    n="$(echo "$line" | grep -oE '^\s+#[0-9]+' | grep -oE '[0-9]+' || true)"
    [ -z "$n" ] && continue
    # Find the archive block for entry $n and check the next 200 lines
    # for a STATUS line that's a direct entry footer (4-space indented).
    if grep -qE "^${n}\. " "$ARCHIVE_FILE"; then
      local block
      block="$(awk -v n="$n" '
        $0 ~ "^"n"\\. " { hit=1; print; next }
        hit && /^[0-9]+\. / { hit=0 }
        hit { print }
      ' "$ARCHIVE_FILE")"
      # Accept EITHER the new STATUS: footer convention OR the
      # legacy "(addressed|resolved YYYY-MM-DD ...)" parenthetical
      # at the entry title line. Both are valid acknowledgements.
      #
      # (2026-04-30 fix) Allow ANY leading-whitespace indent on the
      # STATUS line. Different archive entries use different body
      # indents (3-space for the audit-finding-style entries, 4-space
      # for the recent ones, 5-space for sub-list / Roman-numeral
      # bodies, occasionally 0-space when appended without re-indent).
      # Forcing all entries to a single indent rule would mangle the
      # entries' visual structure; loosening the regex is the right
      # fix. The `STATUS: ` token is unique enough — there's no
      # legitimate non-status line that starts with `STATUS:`.
      if ! echo "$block" | grep -qE '^[[:space:]]*STATUS: ' \
         && ! echo "$block" | head -1 | grep -qiE '\((addressed|resolved|wont_do|won.t do|moot|deferred|partial)\b'; then
        status_missing+=("$n")
      fi
    fi
  done <<< "$resolved_section"
  if [ ${#status_missing[@]} -gt 0 ]; then
    CANDIDATES+=("Index marks RESOLVED/WONT_DO but ARCHIVE entry has no STATUS footer for: #${status_missing[*]} — append 'STATUS: RESOLVED YYYY-MM-DD in vX.Y.Z' to the archive entry")
  fi
}

# ── Run all checks ───────────────────────────────────────────────────────────
check_fleet_version
check_roadmap_coverage
check_resolved_in_open
check_readme_coverage
check_archive_completeness

# ── Output ───────────────────────────────────────────────────────────────────
if [ ${#CANDIDATES[@]} -eq 0 ]; then
  echo "[end-of-session-sweep] all 4 checks clean — session close looks complete"
  exit 0
fi

echo "[end-of-session-sweep] ${#CANDIDATES[@]} candidate(s) for review:"
echo
for i in "${!CANDIDATES[@]}"; do
  echo "  $((i+1)). ${CANDIDATES[$i]}"
done
echo
echo "Each is a candidate — review and decide whether to act or dismiss."
echo "False positives are expected (e.g. an in-flight release where CLAUDE.md"
echo "and the tag don't yet match by design). The script's job is to surface,"
echo "not to fix."
exit 1
