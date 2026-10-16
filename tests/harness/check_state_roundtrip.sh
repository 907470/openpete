#!/usr/bin/env bash
# tests/harness/check_state_roundtrip.sh - savestate restore-at-yield gates.
#
# Rows come from games/<slug>/corpus/state-roundtrip.manifest, fields
#   source | card | boot flags | save key | deferral
# (field values: the Spyro 1 manifest's header). A row's source is a bk2,
# replayed with --bk2-stop-on-end, or `-`, a boot with no movie; both run to
# --frame-limit 6000. For each row, three headless runs:
#   clean                       - baseline --gameplay-hash-log
#   --state-stress 1            - in-memory save + immediate quiet restore
#                                 EVERY tick; hash log must be BYTE-identical
#                                 to clean (restore-at-yield is lossless)
#   --state-stress-rewind 25    - save, run 25 iters, roll back, replay
#                                 (the runahead rollback shape); replayed
#                                 frames must self-match pass 1 AND match
#                                 the clean run
# and, on a row with a save key, a fourth:
#   same-key                    - --state-save-on-frame K and
#                                 --state-load-on-frame K on one run: the
#                                 save is queued before the load, so the run
#                                 saves and reloads one state; the hash log
#                                 must be BYTE-identical to clean. A row
#                                 marked `>0` must report a nonzero deferred
#                                 snapshot count and load count: K falls on a
#                                 yield that is not a VSync wait's final one.
#
# Usage: tests/harness/check_state_roundtrip.sh [--game SLUG] [movie.bk2 ...]
#   (default: the manifest's rows; a movie argument is the row `movie|-|-|-`)
# Exit 0 = all gates PASS. Runs from the repo root.

set -u

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME" || exit 64
BIN="$PG_BIN"
DISC="$PG_DISC"

# Each game's legs beside its rows: the runahead and rewind-driver movie, the
# rewind hold point (the game's feed key) and the transition legs, each
# `movie:expect` (see the runahead-transition section). Spyro 3 has no
# runahead or rewind-driver leg. Every other slug is refused rather than
# replayed against movies recorded for a different game.
RA_MOVIE=""; HOLD=""; XL_LEGS=()
case "$PG_GAME" in
    spyro-1)
        RA_MOVIE=games/spyro-1/bk2/beast_makers_wall.bk2
        XL_LEGS=(games/spyro-1/bk2/exit-level.bk2:clean
                 games/spyro-1/bk2/town-square-portal-entry.bk2:carve-out)
        HOLD=1700:40 ;;
    spyro-2)
        RA_MOVIE=games/spyro-2/bk2/shady-oasis-flame.bk2
        XL_LEGS=(games/spyro-2/bk2/autumn-plains-skelos-badlands-portal-entry.bk2:rollback)
        HOLD=1500:40 ;;
    spyro-3)
        XL_LEGS=(games/spyro-3/bk2/sunrise-spring-sunny-villa-portal-entry.bk2:rollback) ;;
    *) echo "SKIP: $PG_GAME has no savestate corpus"; exit 64 ;;
esac
MANIFEST="$(pg_corpus_manifest "$PG_GAME" state-roundtrip.manifest)"

# Exit 64 = SKIP (the CTest SKIP_RETURN_CODE), so a checkout without a built
# binary or without the disc reports "not run" rather than a failure.
if [ ! -x "$BIN" ]; then echo "SKIP: no binary at $BIN"; exit 64; fi
if [ ! -f "$DISC" ]; then echo "SKIP: no disc at $DISC"; exit 64; fi

OUT="${TMPDIR:-/tmp}/openpete-state-roundtrip.$$"
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

# --game and its slug are consumed above; what remains is the movie list.
MOVIES=()
skip_next=0
for a in "$@"; do
    if [ "$skip_next" = 1 ]; then skip_next=0; continue; fi
    case "$a" in
        --game)   skip_next=1 ;;
        --game=*) ;;
        *) MOVIES+=("$a") ;;
    esac
done

# One entry per row: source, card, boot flags, save key, deferral; an empty
# field reads as `-`.
R_SRC=(); R_CARD=(); R_FLAGS=(); R_KEY=(); R_DEF=()
trim() { local v="$1"; v="${v#"${v%%[![:space:]]*}"}"; v="${v%"${v##*[![:space:]]}"}"; echo "${v:--}"; }
if [ ${#MOVIES[@]} -gt 0 ]; then
    for m in "${MOVIES[@]}"; do
        R_SRC+=("$m"); R_CARD+=(-); R_FLAGS+=(-); R_KEY+=(-); R_DEF+=(-)
    done
else
    pg_manifest "$MANIFEST" pipe
    while IFS='|' read -r f_src f_card f_flags f_key f_def; do
        [[ -z "${f_src//[[:space:]]/}" ]] && continue
        f_src="$(trim "$f_src")"
        [[ "$f_src" != - ]] && f_src="$PG_BK2_DIR/$f_src"
        R_SRC+=("$f_src"); R_CARD+=("$(trim "${f_card:-}")"); R_FLAGS+=("$(trim "${f_flags:-}")")
        R_KEY+=("$(trim "${f_key:-}")"); R_DEF+=("$(trim "${f_def:-}")")
    done < <(sed 's/#.*//' "$MANIFEST")
fi
[ ${#R_SRC[@]} -gt 0 ] || { echo "FAIL: no rows in $MANIFEST"; exit 1; }

for m in "${R_SRC[@]}"; do
    [ "$m" = - ] && continue
    if [ ! -f "$m" ]; then echo "SKIP: no movie at $m"; exit 64; fi
done

# A row's launch: ROW_BASE replaces PG_RUN_ARGS for the row's runs (a card
# fixture drops --no-card from it), ROW_ARGS follows it, and ROW_FLAGS, the
# row's boot flags, follow the harness's own (a boot flag such as
# --frame-limit overrides the harness default). A fixture is copied to a
# scratch card under $OUT, deleted first: the run writes through to it.
row_launch() { # $1 row index  $2 scratch card file name
    local i="$1" card="${R_CARD[$1]}" t
    ROW_BASE=("${PG_RUN_ARGS[@]}"); ROW_ARGS=(); ROW_FLAGS=()
    if [ "${R_SRC[$i]}" != - ]; then ROW_ARGS+=(--bk2 "${R_SRC[$i]}" --bk2-stop-on-end); fi
    case "$card" in
        -) ;;
        none)
            local has=0
            for t in "${ROW_BASE[@]}"; do [ "$t" = --no-card ] && has=1; done
            [ $has = 1 ] || ROW_ARGS+=(--no-card) ;;
        *)
            local fx="tests/golden/$PG_GAME/$card"
            [ -f "$fx" ] || { echo "  SKIP: no card fixture $fx"; return 64; }
            ROW_BASE=()
            for t in "${PG_RUN_ARGS[@]}"; do [ "$t" = --no-card ] || ROW_BASE+=("$t"); done
            rm -f "${OUT:?}/$2" "${OUT:?}/$2.modset"
            cp "$fx" "$OUT/$2"
            ROW_ARGS+=(--card1 "$OUT/$2" --no-card2) ;;
    esac
    # shellcheck disable=SC2206
    if [ "${R_FLAGS[$i]}" != - ]; then ROW_FLAGS=(${R_FLAGS[$i]}); fi
    return 0
}

# pg_capture deletes the hash log and the run log first and asserts the hash
# log after, so a leg can never compare the previous leg's csv.
run() { # $1 movie  $2 csv  $3 extra-args...
    local movie="$1" csv="$2"; shift 2
    local PG_CAPTURE_TIMEOUT=600
    pg_capture "$csv.log" "$csv" -- \
        --bk2 "$movie" --bk2-stop-on-end --frame-limit 6000 \
        --unlock-frames 0-6000 \
        --gameplay-hash-log "$csv" "$@"
}

# One run of a row: the row's launch in place of the movie.
row_run() { # $1 row index  $2 csv  $3 extra-args...
    local i="$1" csv="$2"; shift 2
    row_launch "$i" "$(basename "$csv").mcr" || return $?
    local -a PG_RUN_ARGS=("${ROW_BASE[@]}")
    local PG_CAPTURE_TIMEOUT=600
    pg_capture "$csv.log" "$csv" -- \
        "${ROW_ARGS[@]}" --frame-limit 6000 \
        --unlock-frames 0-6000 \
        --gameplay-hash-log "$csv" "${ROW_FLAGS[@]}" "$@"
}

# A snapshot or load requested below a yield other than a VSync wait's final
# one waits for the next final yield; each leg reports the count.
deferred() { # $1 run log
    grep -o 'deferred to a final yield: .*' "$1" | tail -1
}

fail=0
for i in "${!R_SRC[@]}"; do
    src="${R_SRC[$i]}"
    if [ "$src" = - ]; then name="boot-$i"; else name=$(basename "$src" .bk2); fi
    echo "== $name"
    row_run "$i" "$OUT/$name-clean.csv"            || { echo "  clean run FAILED"; fail=1; continue; }
    row_run "$i" "$OUT/$name-stress.csv" --state-stress 1 \
                                                   || { echo "  stress run FAILED"; fail=1; continue; }
    row_run "$i" "$OUT/$name-rw.csv" --state-stress-rewind 25 \
                                                   || { echo "  rewind-stress run FAILED"; fail=1; continue; }

    echo "  stress(1) $(deferred "$OUT/$name-stress.csv.log")"
    echo "  rewind(25) $(deferred "$OUT/$name-rw.csv.log")"
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

    # Same-key one-shot: the save and the load armed at one feed key. Both
    # are queued in the same iteration, the save first, and run at the next
    # final yield; the run continues from the state it just saved, so its
    # hash log must equal the clean run's. Engagement: the run logs both the
    # save and the load of the state file.
    key="${R_KEY[$i]}"
    [ "$key" = - ] && continue
    row_run "$i" "$OUT/$name-sk.csv" \
        --state-save-on-frame "$key" --state-save-output "$OUT/$name-sk.state" \
        --state-load-on-frame "$key" --state-load-input "$OUT/$name-sk.state" \
        || { echo "  same-key run FAILED"; fail=1; continue; }
    sk_def="$(deferred "$OUT/$name-sk.csv.log")"
    echo "  same-key($key) $sk_def"
    sk_saves=$(sed -n 's/.*deferred to a final yield: \([0-9]*\) snapshot.*/\1/p' <<<"$sk_def")
    sk_loads=$(sed -n 's/.* \([0-9]*\) load request.*/\1/p' <<<"$sk_def")
    sk_eng=0
    grep -q "saved $OUT/$name-sk.state" "$OUT/$name-sk.csv.log" &&
        grep -q "loaded $OUT/$name-sk.state" "$OUT/$name-sk.csv.log" && sk_eng=1
    if [ $sk_eng = 0 ] || [ -z "$sk_saves" ] || [ -z "$sk_loads" ]; then
        echo "  same-key($key): VACUOUS FAIL (save/load not both logged, or no deferral line)"; fail=1
    elif [ "${R_DEF[$i]}" = ">0" ] && { [ "$sk_saves" -eq 0 ] || [ "$sk_loads" -eq 0 ]; }; then
        echo "  same-key($key): key is not below an intermediate yield (deferred $sk_saves/$sk_loads) FAIL"; fail=1
    elif cmp -s "$OUT/$name-clean.csv" "$OUT/$name-sk.csv"; then
        echo "  same-key($key): byte-identical PASS"
    else
        echo "  same-key($key): DIVERGED FAIL"; fail=1
    fi
done

# Runahead determinism gate: with --runahead 2 the presented timeline
# leads, but the COMMITTED timeline must be byte-identical to clean -
# gameplay state is never allowed to feel the speculation.
#
# The same run carries the PREDICTION-EXACTNESS oracle
# (OPENPETE_RA_PREDICT_CHECK=1, read-only): the led RAM+scratch hash
# recorded at each speculation must byte-match the committed state when
# the real timeline reaches that frame (edge-crossing predictions are
# invalidated - the branch legitimately changed). This is the
# anti-stealth-rand() gate: a thunk reading host state that behaves
# differently under speculation than in the real future corrupts
# predictions while every determinism gate stays green. A nonzero check
# count also proves speculation actually engaged (a driver regression
# that never speculates would pass the cmp trivially).
if [ -n "$RA_MOVIE" ]; then
echo "== runahead ($(basename "$RA_MOVIE" .bk2), --runahead 2)"
run "$RA_MOVIE" "$OUT/ra-clean.csv" \
    || { echo "  clean run FAILED"; fail=1; }
OPENPETE_RA_PREDICT_CHECK=1 \
run "$RA_MOVIE" "$OUT/ra-on.csv" --runahead 2 \
    || { echo "  runahead run FAILED"; fail=1; }
if cmp -s "$OUT/ra-clean.csv" "$OUT/ra-on.csv"; then
    echo "  runahead(2): real timeline byte-identical PASS"
else
    echo "  runahead(2): real timeline DIVERGED FAIL"; fail=1
fi
echo "  runahead(2) $(deferred "$OUT/ra-on.csv.log")"
pc=$(grep -o '\[ra-predict\] checked=[0-9]* mismatch=[0-9]* invalidated=[0-9]*' \
     "$OUT/ra-on.csv.log" | tail -1)
pc_checked=${pc#*checked=}; pc_checked=${pc_checked%% *}
pc_mism=${pc#*mismatch=};   pc_mism=${pc_mism%% *}
if [ -n "$pc" ] && [ "${pc_checked:-0}" -ge 50 ] && [ "${pc_mism:-1}" -eq 0 ]; then
    echo "  predict-check: $pc PASS"
else
    echo "  predict-check: ${pc:-no summary emitted} FAIL"; fail=1
fi

fi

# Runahead x transitions. A speculation that leaves gameplay (a portal, a
# level exit) rolls back when the game fiber parks at the snapshot's park
# site (the psyq_VSync call it was taken under) and is adopted when the
# fiber parks at another site, since the fiber's host stack is not
# snapshotted. An adopted speculation's VBLs ran on held input, so its
# movie rows are not played and the hash log has no row for them. Each
# leg's run reports `left gameplay: R rolled back, A adopted` and the
# adoptions; its expectation:
#   rollback  - byte-identical to clean, 0 adoptions, R > 0 (engaged);
#   clean     - byte-identical to clean, 0 adoptions;
#   carve-out - adoptions counted and reported (the transition moves the
#               park site); every shared row equal to clean, and the level
#               after the transition loads.
for leg in "${XL_LEGS[@]}"; do
    xl_movie="${leg%:*}"; xl_expect="${leg##*:}"
    xl_name=$(basename "$xl_movie" .bk2)
    echo "== runahead-transition ($xl_name, --runahead 2, $xl_expect)"
    [ -f "$xl_movie" ] || { echo "  no movie at $xl_movie FAIL"; fail=1; continue; }
    xl_clean="$OUT/$xl_name-clean.csv"
    if [ ! -f "$xl_clean" ]; then
        run "$xl_movie" "$xl_clean" || { echo "  clean run FAILED"; fail=1; continue; }
    fi
    run "$xl_movie" "$OUT/xl-$xl_name.csv" --runahead 2 \
        || { echo "  runahead run FAILED"; fail=1; continue; }
    xl_log="$OUT/xl-$xl_name.csv.log"
    xl_line="$(deferred "$xl_log")"
    xl_adopt=$(sed -n 's/.* \([0-9]*\) adopted at another park site.*/\1/p' <<<"$xl_line")
    xl_rolled=$(sed -n 's/.*left gameplay: \([0-9]*\) rolled back.*/\1/p' <<<"$xl_line")
    if [ -z "$xl_adopt" ] || [ -z "$xl_rolled" ]; then
        echo "  no runahead summary line FAIL"; fail=1; continue
    fi
    echo "  left gameplay: $xl_rolled rolled back; adopted: $xl_adopt"
    xl_ident=0
    cmp -s "$xl_clean" "$OUT/xl-$xl_name.csv" && xl_ident=1
    case "$xl_expect" in
        rollback|clean)
            if [ $xl_ident = 1 ] && [ "$xl_adopt" -eq 0 ] &&
               { [ "$xl_expect" = clean ] || [ "$xl_rolled" -gt 0 ]; }; then
                echo "  $xl_name under runahead(2): byte-identical, 0 adopted PASS"
            elif [ $xl_ident = 0 ]; then
                echo "  $xl_name under runahead(2): DIVERGED FAIL"; fail=1
            elif [ "$xl_adopt" -ne 0 ]; then
                echo "  $xl_name under runahead(2): $xl_adopt adopted FAIL"; fail=1
            else
                echo "  $xl_name under runahead(2): no speculation left gameplay VACUOUS FAIL"; fail=1
            fi ;;
        carve-out)
            python3 - "$xl_clean" "$OUT/xl-$xl_name.csv" "$xl_adopt" "$xl_name" <<'EOF' || fail=1
import csv, sys
clean = {r[0]: r[1:] for r in csv.reader(open(sys.argv[1])) if r and r[0] != 'frame'}
ra = {r[0]: r[1:] for r in csv.reader(open(sys.argv[2])) if r and r[0] != 'frame'}
missing = sorted(int(f) for f in clean if f not in ra)
div = sorted(int(f) for f in ra if f in clean and clean[f] != ra[f])
adopt = int(sys.argv[3])
ok = adopt > 0 and not div
span = f"{missing[0]}-{missing[-1]}" if missing else "none"
print(f"  {sys.argv[4]} under runahead(2): CARVE-OUT {adopt} adopted, {len(missing)} "
      f"row(s) absent (frames {span}), {len(div)} shared row(s) diverged -> "
      + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
EOF
            if grep -q "level_10 -> level_13" "$xl_log"; then
                echo "  $xl_name under runahead(2): level loaded PASS"
            else
                echo "  $xl_name under runahead(2): LEVEL NEVER LOADED FAIL"; fail=1
            fi ;;
    esac
done

if [ -n "$RA_MOVIE" ]; then
# Rewind driver gate: hold rewind for 40 iterations mid-gameplay via
# --rewind-hold; the scrub must be a single backward jump and the
# post-release timeline must match the clean run bit-for-bit (movie
# inputs are frame-locked, so resuming re-runs the same future).
echo "== rewind-driver ($(basename "$RA_MOVIE" .bk2), hold@${HOLD%%:*} x${HOLD##*:})"
run "$RA_MOVIE" "$OUT/drv-clean.csv" \
    || { echo "  clean run FAILED"; fail=1; }
run "$RA_MOVIE" "$OUT/drv-rw.csv" \
    --rewind 30 --rewind-hold "$HOLD" \
    || { echo "  driver run FAILED"; fail=1; }
echo "  driver $(deferred "$OUT/drv-rw.csv.log")"
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
fi

[ $fail -eq 0 ] && echo "ALL STATE ROUNDTRIP GATES PASS" || echo "STATE ROUNDTRIP GATES FAILED"
exit $fail
