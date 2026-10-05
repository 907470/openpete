#!/usr/bin/env bash

# tests/harness/check_surface_dump.sh - OPENPETE_SURFACE_DUMP and the
# declared-defaults selfcheck.
#
# Legs:
#   dump      the dump exits 0, writes JSON, and its selfcheck reports a
#             checked count above zero and no failure for each checkable
#             population (flag, switch, row); the entries hold every
#             namespace (flag, knob, menu, key, directive, vocab).
#   control   OPENPETE_SURFACE_SELFCHECK_BREAK=fov replaces --fov's declared
#             default with another value; the selfcheck must fail it (exit 1,
#             a FAIL line naming --fov). Without this the dump leg could pass
#             on a selfcheck that compares nothing.
#
# Usage: tests/harness/check_surface_dump.sh [--game SLUG] [--build-dir=DIR]
# Exit:  0 pass, 1 fail, 2 vacuous (the negative control did not fail),
#        64 skip (no binary).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

for a in "$@"; do
    case "$a" in --build-dir=*) export PG_BUILD_DIR="${a#--build-dir=}" ;; esac
done
ARGS=()
for a in "$@"; do
    case "$a" in --build-dir=*) ;; *) ARGS+=("$a") ;; esac
done

# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game ${ARGS[@]+"${ARGS[@]}"})" || exit 64
pg_resolve "$GAME" || exit 64
[[ -x "$PG_BIN" ]] || { echo "[surface-dump] SKIP: no binary $PG_BIN"; exit 64; }

OUT="${SURFACE_DUMP_OUT:-${TMPDIR:-/tmp}/openpete-check-surface-dump}/$PG_GAME"
mkdir -p "$OUT"
rm -f "$OUT/dump.json" "$OUT/dump.err" "$OUT/control.json" "$OUT/control.err"

rc=0
OPENPETE_SURFACE_DUMP="$OUT/dump.json" "$PG_BIN" 2>"$OUT/dump.err" >/dev/null || rc=$?
if [[ "$rc" != 0 || ! -s "$OUT/dump.json" ]]; then
    echo "[surface-dump] FAIL: dump exited $rc or wrote no file ($PG_GAME)"
    cat "$OUT/dump.err"
    exit 1
fi

python3 - "$OUT/dump.json" <<'PY' || exit 1
import json, sys
d = json.load(open(sys.argv[1]))
sc = d["selfcheck"]
bad = 0
for pop in ("flag", "switch", "row"):
    if sc[pop]["checked"] <= 0 or sc[pop]["failed"] != 0:
        print("[surface-dump] FAIL: %s population %s" % (pop, sc[pop])); bad = 1
ns = {e["id"].split(":", 1)[0] for e in d["entries"]}
missing = {"flag", "knob", "menu", "key", "directive", "vocab"} - ns
if missing:
    print("[surface-dump] FAIL: no entries in", sorted(missing)); bad = 1
for pop in ("flag", "switch", "row", "env"):
    print("[surface-dump] %-6s checked %d unchecked %d failed %d"
          % (pop, sc[pop]["checked"], sc[pop]["unchecked"], sc[pop]["failed"]))
sys.exit(bad)
PY

rc=0
OPENPETE_SURFACE_SELFCHECK_BREAK=fov OPENPETE_SURFACE_DUMP="$OUT/control.json" \
    "$PG_BIN" 2>"$OUT/control.err" >/dev/null || rc=$?
if [[ "$rc" != 1 ]] || ! grep -q 'FAIL: --fov ' "$OUT/control.err"; then
    echo "[surface-dump] VACUOUS: a broken --fov default did not fail the selfcheck (exit $rc)"
    exit 2
fi
echo "[surface-dump] PASS ($PG_GAME)"
