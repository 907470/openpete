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
cd "$(dirname "$0")/.."

BIN=build/linux-release/games/spyro-1/openpete-spyro1
DISC=games/spyro-1/spyro-1/build/SCUS-94228.cue
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

run() { # $1 movie  $2 csv  $3 extra-args...
    local movie="$1" csv="$2"; shift 2
    timeout 600 "$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
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
