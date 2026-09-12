#!/usr/bin/env bash
# scripts/check_state_roundtrip.sh — savestate P3 restore-at-yield gates.
#
# For each corpus movie, three headless runs:
#   clean                       — baseline --gameplay-hash-log
#   --state-stress 1            — in-memory save + immediate quiet restore
#                                 EVERY tick; hash log must be BYTE-identical
#                                 to clean (restore-at-yield is lossless)
#   --state-stress-rewind 25    — save, run 25 iters, roll back, replay
#                                 (the runahead rollback shape); replayed
#                                 frames must self-match pass 1 AND match
#                                 the clean run
#
# Usage: scripts/check_state_roundtrip.sh [movie.bk2 ...]
#   (default: a representative corpus subset, incl. a level transition)
# Exit 0 = all gates PASS. Runs from the repo root.

set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/.."

BIN=${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}
DISC=${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}

# Exit 64 = SKIP (the CTest SKIP_RETURN_CODE), so a checkout without a built
# binary or without the disc reports "not run" rather than a failure.
if [ ! -x "$BIN" ]; then echo "SKIP: no binary at $BIN"; exit 64; fi
if [ ! -f "$DISC" ]; then echo "SKIP: no disc at $DISC"; exit 64; fi

OUT="${TMPDIR:-/tmp}/openpete-state-roundtrip.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

MOVIES=("$@")
if [ ${#MOVIES[@]} -eq 0 ]; then
    MOVIES=(
        games/spyro-1/bk2/artisans-lp-waterfall.bk2
        games/spyro-1/bk2/town-square-portal-entry.bk2
        games/spyro-1/bk2/exit-level.bk2
        games/spyro-1/bk2/beast_makers_wall.bk2
    )
fi

for m in "${MOVIES[@]}"; do
    if [ ! -f "$m" ]; then echo "SKIP: no movie at $m"; exit 64; fi
done

run() { # $1 movie  $2 csv  $3 extra-args...
    local movie="$1" csv="$2"; shift 2
    timeout 600 "$BIN" --headless --disc "$DISC" \
        --bk2 "$movie" --bk2-stop-on-end --frame-limit 6000 \
        --unlock-frames 0-6000 \
        --gameplay-hash-log "$csv" "$@" >"$csv.log" 2>&1
}

fail=0
for movie in "${MOVIES[@]}"; do
    name=$(basename "$movie" .bk2)
    echo "== $name"
    run "$movie" "$OUT/$name-clean.csv"            || { echo "  clean run FAILED"; fail=1; continue; }
    run "$movie" "$OUT/$name-stress.csv" --state-stress 1 \
                                                   || { echo "  stress run FAILED"; fail=1; continue; }
    run "$movie" "$OUT/$name-rw.csv" --state-stress-rewind 25 \
                                                   || { echo "  rewind-stress run FAILED"; fail=1; continue; }

    if cmp -s "$OUT/$name-clean.csv" "$OUT/$name-stress.csv"; then
        echo "  stress(1): byte-identical PASS"
    else
        echo "  stress(1): DIVERGED FAIL"; fail=1
    fi

    python3 - "$OUT/$name-clean.csv" "$OUT/$name-rw.csv" <<'EOF' || fail=1
import csv, sys
clean_p, rw_p = sys.argv[1], sys.argv[2]
rows = [r for r in csv.reader(open(rw_p)) if r and r[0] != 'frame']
first = {}; mism = dup = 0
for r in rows:
    f, h = r[0], r[2]
    if f in first:
        dup += 1
        if first[f] != h: mism += 1
    else:
        first[f] = h
clean = {r[0]: r[2] for r in csv.reader(open(clean_p)) if r and r[0] != 'frame'}
shared = [f for f in first if f in clean]
xm = sum(1 for f in shared if clean[f] != first[f])
ok = mism == 0 and xm == 0
print(f"  rewind(25): {dup} replayed, {mism} self-mism, {xm}/{len(shared)} vs clean -> "
      + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
EOF
done

# P5 runahead determinism gate: with --runahead 2 the presented timeline
# leads, but the COMMITTED timeline must be byte-identical to clean —
# gameplay state is never allowed to feel the speculation.
#
# The same run carries the PREDICTION-EXACTNESS oracle
# (OPENPETE_RA_PREDICT_CHECK=1, read-only): the led RAM+scratch hash
# recorded at each speculation must byte-match the committed state when
# the real timeline reaches that frame (edge-crossing predictions are
# invalidated — the branch legitimately changed). This is the
# anti-stealth-rand() gate: a thunk reading host state that behaves
# differently under speculation than in the real future corrupts
# predictions while every determinism gate stays green. A nonzero check
# count also proves speculation actually engaged (a driver regression
# that never speculates would pass the cmp trivially).
echo "== runahead (beast_makers_wall, --runahead 2)"
run games/spyro-1/bk2/beast_makers_wall.bk2 "$OUT/ra-clean.csv" \
    || { echo "  clean run FAILED"; fail=1; }
OPENPETE_RA_PREDICT_CHECK=1 \
run games/spyro-1/bk2/beast_makers_wall.bk2 "$OUT/ra-on.csv" --runahead 2 \
    || { echo "  runahead run FAILED"; fail=1; }
if cmp -s "$OUT/ra-clean.csv" "$OUT/ra-on.csv"; then
    echo "  runahead(2): real timeline byte-identical PASS"
else
    echo "  runahead(2): real timeline DIVERGED FAIL"; fail=1
fi
pc=$(grep -o '\[ra-predict\] checked=[0-9]* mismatch=[0-9]* invalidated=[0-9]*' \
     "$OUT/ra-on.csv.log" | tail -1)
pc_checked=${pc#*checked=}; pc_checked=${pc_checked%% *}
pc_mism=${pc#*mismatch=};   pc_mism=${pc_mism%% *}
if [ -n "$pc" ] && [ "${pc_checked:-0}" -ge 50 ] && [ "${pc_mism:-1}" -eq 0 ]; then
    echo "  predict-check: $pc PASS"
else
    echo "  predict-check: ${pc:-no summary emitted} FAIL"; fail=1
fi

# Runahead × transitions. Speculation that crosses out of gameplay-live
# is ADOPTED, not rolled back: the fiber parks at a transition-code yield
# whose host stack differs structurally from the gameplay yield the
# snapshot was taken at (the fiber stack is not snapshottable), and
# restoring under that continuation collapses the game's VSync pacing —
# the failure mode was a full tick's work every VBL and a level load that
# never completes. Gate both directions:
#   - portal entry (walk-in, speculation crosses mid-tick → adoption):
#     the level MUST still load under --runahead 2;
#   - level exit (crossing lands between ticks, no adoption): committed
#     timeline MUST stay byte-identical to clean.
echo "== runahead-transitions (town-square portal, exit-level)"
timeout 600 "$BIN" --headless --disc "$DISC" \
    --bk2 games/spyro-1/bk2/town-square-portal-entry.bk2 --frame-limit 12000 \
    --unlock-frames 0-12000 --runahead 2 \
    --gameplay-hash-log "$OUT/tsq-ra.csv" >"$OUT/tsq-ra.log" 2>&1 \
    || { echo "  portal runahead run FAILED"; fail=1; }
if grep -q "level_10 -> level_13" "$OUT/tsq-ra.log"; then
    echo "  portal entry under runahead(2): level loaded PASS"
else
    echo "  portal entry under runahead(2): LEVEL NEVER LOADED FAIL"; fail=1
fi
run games/spyro-1/bk2/exit-level.bk2 "$OUT/xl-ra.csv" --runahead 2 \
    || { echo "  exit runahead run FAILED"; fail=1; }
if cmp -s "$OUT/exit-level-clean.csv" "$OUT/xl-ra.csv"; then
    echo "  exit-level under runahead(2): byte-identical PASS"
else
    echo "  exit-level under runahead(2): DIVERGED FAIL"; fail=1
fi

# P4 driver gate: hold rewind for 40 iterations mid-gameplay via
# --rewind-hold; the scrub must be a single backward jump and the
# post-release timeline must match the clean run bit-for-bit (movie
# inputs are frame-locked, so resuming re-runs the same future).
echo "== rewind-driver (beast_makers_wall, hold@1700 x40)"
run games/spyro-1/bk2/beast_makers_wall.bk2 "$OUT/drv-clean.csv" \
    || { echo "  clean run FAILED"; fail=1; }
run games/spyro-1/bk2/beast_makers_wall.bk2 "$OUT/drv-rw.csv" \
    --rewind 30 --rewind-hold 1700:40 \
    || { echo "  driver run FAILED"; fail=1; }
python3 - "$OUT/drv-clean.csv" "$OUT/drv-rw.csv" <<'EOF' || fail=1
import csv, sys
clean = {r[0]: r[2] for r in csv.reader(open(sys.argv[1])) if r and r[0] != 'frame'}
rows = [r for r in csv.reader(open(sys.argv[2])) if r and r[0] != 'frame']
fr = [int(r[0]) for r in rows]
gaps = [(fr[i-1], fr[i]) for i in range(1, len(fr)) if fr[i] < fr[i-1]]
mism = [r[0] for r in rows if r[0] in clean and clean[r[0]] != r[2]]
ok = len(gaps) == 1 and not mism
print(f"  driver: jumps={gaps} mismatches={len(mism)} -> " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
EOF

[ $fail -eq 0 ] && echo "ALL STATE ROUNDTRIP GATES PASS" || echo "STATE ROUNDTRIP GATES FAILED"
exit $fail
