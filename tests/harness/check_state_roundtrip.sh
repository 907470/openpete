#!/usr/bin/env bash
# tests/harness/check_state_roundtrip.sh - savestate restore-at-yield gates.
#
# Rows come from games/<slug>/corpus/state-roundtrip.manifest, fields
#   source | card | boot flags | save key | deferral | load key | rewind
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
#                                 the clean run; the leg arms only in
#                                 gameplay, so it must replay at least one
#                                 tick (zero is VACUOUS FAIL) unless the row
#                                 is marked `no-gameplay`, which asserts it
#                                 replays none
# and, on a row with a save key, a fourth (and, with a load key, a fifth and
# sixth):
#   same-key                    - --state-save-on-frame K and
#                                 --state-load-on-frame K on one run: the
#                                 save is queued before the load, so the run
#                                 saves and reloads one state; the hash log
#                                 must be BYTE-identical to clean. A row
#                                 marked `>0` must report a nonzero deferred
#                                 snapshot count and load count: K falls on a
#                                 yield that is not a VSync wait's final one.
#                                 A row marked `loop` must also report a
#                                 nonzero count deferred at an emitted
#                                 polling-loop delivery, with the loop's
#                                 header address: K falls inside a loop the
#                                 recompiler emitted a delivery point in.
#   load-key                    - --state-save-on-frame K and
#                                 --state-load-on-frame L (L > K) on one
#                                 run: the run returns to the state it saved
#                                 L - K VBLs earlier, so host state the state
#                                 omits is left on the later timeline
#   fresh-load                  - a second process loads that state file at
#                                 L, with no host state of the saving run;
#                                 on both, every row matches the clean row
#                                 of its (frame, vbl), at least one row
#                                 after the load is shared with clean, and
#                                 the log shows the save and the load; on a
#                                 card row the second process holds the
#                                 saving run's card path, re-copied from the
#                                 fixture, and both logs print the card
#                                 slots held and the FLAG bytes
#   card-refused                - on a card row, a third process holding
#                                 another card path asks to load the state
#                                 at L: the log names the state's card in a
#                                 refusal and the hash log is byte-identical
#                                 to clean (nothing was restored)
#
# Usage: tests/harness/check_state_roundtrip.sh [--game SLUG] [--row NAME]
#                                                [movie.bk2 ...]
#   (default: the manifest's rows; a movie argument is the row `movie|-|-|-`;
#   --row runs only the manifest rows whose source file name, or `boot-<i>`
#   for the movie-less row at index i, is NAME, and none of the game's
#   runahead, transition or rewind-driver legs)
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
ROW_ONLY=""
skip_next=""
for a in "$@"; do
    case "$skip_next" in
        game) skip_next=""; continue ;;
        row)  skip_next=""; ROW_ONLY="$a"; continue ;;
    esac
    case "$a" in
        --game)   skip_next=game ;;
        --game=*) ;;
        --row)    skip_next=row ;;
        --row=*)  ROW_ONLY="${a#--row=}" ;;
        *) MOVIES+=("$a") ;;
    esac
done
if [ -n "$ROW_ONLY" ]; then RA_MOVIE=""; XL_LEGS=(); fi

# One entry per row: source, card, boot flags, save key, deferral, load key,
# rewind; an empty field reads as `-`.
R_SRC=(); R_CARD=(); R_FLAGS=(); R_KEY=(); R_DEF=(); R_LKEY=(); R_RW=()
trim() { local v="$1"; v="${v#"${v%%[![:space:]]*}"}"; v="${v%"${v##*[![:space:]]}"}"; echo "${v:--}"; }
if [ ${#MOVIES[@]} -gt 0 ]; then
    for m in "${MOVIES[@]}"; do
        R_SRC+=("$m"); R_CARD+=(-); R_FLAGS+=(-); R_KEY+=(-); R_DEF+=(-); R_LKEY+=(-); R_RW+=(-)
    done
else
    pg_manifest "$MANIFEST" pipe
    while IFS='|' read -r f_src f_card f_flags f_key f_def f_lkey f_rw; do
        [[ -z "${f_src//[[:space:]]/}" ]] && continue
        f_src="$(trim "$f_src")"
        [[ "$f_src" != - ]] && f_src="$PG_BK2_DIR/$f_src"
        R_SRC+=("$f_src"); R_CARD+=("$(trim "${f_card:-}")"); R_FLAGS+=("$(trim "${f_flags:-}")")
        R_KEY+=("$(trim "${f_key:-}")"); R_DEF+=("$(trim "${f_def:-}")")
        R_LKEY+=("$(trim "${f_lkey:-}")"); R_RW+=("$(trim "${f_rw:-}")")
    done < <(sed 's/#.*//' "$MANIFEST")
fi
if [ -n "$ROW_ONLY" ]; then
    K_SRC=(); K_CARD=(); K_FLAGS=(); K_KEY=(); K_DEF=(); K_LKEY=(); K_RW=()
    for i in "${!R_SRC[@]}"; do
        if [ "${R_SRC[$i]}" = - ]; then rn="boot-$i"; else rn="$(basename "${R_SRC[$i]}")"; fi
        [ "$rn" = "$ROW_ONLY" ] || continue
        K_SRC+=("${R_SRC[$i]}"); K_CARD+=("${R_CARD[$i]}"); K_FLAGS+=("${R_FLAGS[$i]}")
        K_KEY+=("${R_KEY[$i]}"); K_DEF+=("${R_DEF[$i]}"); K_LKEY+=("${R_LKEY[$i]}")
        K_RW+=("${R_RW[$i]}")
    done
    R_SRC=("${K_SRC[@]}"); R_CARD=("${K_CARD[@]}"); R_FLAGS=("${K_FLAGS[@]}")
    R_KEY=("${K_KEY[@]}"); R_DEF=("${K_DEF[@]}"); R_LKEY=("${K_LKEY[@]}")
    R_RW=("${K_RW[@]}")
    [ ${#R_SRC[@]} -gt 0 ] || { echo "FAIL: no row $ROW_ONLY in $MANIFEST"; exit 1; }
fi
[ ${#R_SRC[@]} -gt 0 ] || { echo "FAIL: no rows in $MANIFEST"; exit 1; }
for i in "${!R_RW[@]}"; do
    case "${R_RW[$i]}" in
        -|no-gameplay) ;;
        *) echo "FAIL: rewind field '${R_RW[$i]}' of row ${R_SRC[$i]} is not - or no-gameplay"; exit 1 ;;
    esac
done

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
        unformatted)
            # A never-formatted card: 131072 zero bytes, written fresh.
            ROW_BASE=()
            for t in "${PG_RUN_ARGS[@]}"; do [ "$t" = --no-card ] || ROW_BASE+=("$t"); done
            rm -f "${OUT:?}/$2" "${OUT:?}/$2.modset"
            head -c 131072 /dev/zero > "$OUT/$2"
            ROW_ARGS+=(--card1 "$OUT/$2" --no-card2) ;;
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

# One run of a row: the row's launch in place of the movie. The scratch card
# is named after the csv unless ROW_CARD_NAME names it.
ROW_CARD_NAME=""
row_run() { # $1 row index  $2 csv  $3 extra-args...
    local i="$1" csv="$2"; shift 2
    row_launch "$i" "${ROW_CARD_NAME:-$(basename "$csv").mcr}" || return $?
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
    python3 - "$OUT/$name-clean.csv" "$OUT/$name-rw.csv" "${R_RW[$i]}" <<'EOF' || fail=1
import csv, sys
clean_p, rw_p, expect = sys.argv[1], sys.argv[2], sys.argv[3]
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
# The rollback arms only in gameplay (game_is_gameplay_live): a row whose
# movie never reaches gameplay replays nothing and declares so; on any other
# row zero replayed ticks means the leg tested nothing.
if expect == 'no-gameplay':
    verdict = ("no-gameplay row, none replayed PASS" if dup == 0 else
               "FAIL: row marked no-gameplay replayed ticks; drop the marker")
    ok = ok and dup == 0
elif dup == 0:
    verdict = "VACUOUS FAIL (no rollback replayed a tick)"
    ok = False
else:
    verdict = "PASS" if ok else "FAIL"
print(f"  rewind(25): {dup} replayed, {mism} self-mism, {xm}/{len(shared)} vs clean -> {verdict}")
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
    sk_loop_line="$(grep -o 'deferred at an emitted loop site: .*' "$OUT/$name-sk.csv.log" | tail -1)"
    sk_loop=$(sed -n 's/.*loop site: \([0-9]*\) request.*/\1/p' <<<"$sk_loop_line")
    echo "  same-key($key) ${sk_loop_line:-no loop-site line}"
    sk_saves=$(sed -n 's/.*deferred to a final yield: \([0-9]*\) snapshot.*/\1/p' <<<"$sk_def")
    sk_loads=$(sed -n 's/.* \([0-9]*\) load request.*/\1/p' <<<"$sk_def")
    sk_eng=0
    grep -q "saved $OUT/$name-sk.state" "$OUT/$name-sk.csv.log" &&
        grep -q "loaded $OUT/$name-sk.state" "$OUT/$name-sk.csv.log" && sk_eng=1
    if [ $sk_eng = 0 ] || [ -z "$sk_saves" ] || [ -z "$sk_loads" ] || [ -z "$sk_loop" ]; then
        echo "  same-key($key): VACUOUS FAIL (save/load not both logged, or no deferral line)"; fail=1
    elif { [ "${R_DEF[$i]}" = ">0" ] || [ "${R_DEF[$i]}" = loop ]; } &&
         { [ "$sk_saves" -eq 0 ] || [ "$sk_loads" -eq 0 ]; }; then
        echo "  same-key($key): key is not below an intermediate yield (deferred $sk_saves/$sk_loads) FAIL"; fail=1
    elif [ "${R_DEF[$i]}" = loop ] && [ "${sk_loop:-0}" -eq 0 ]; then
        echo "  same-key($key): no request deferred at an emitted loop site FAIL"; fail=1
    elif cmp -s "$OUT/$name-clean.csv" "$OUT/$name-sk.csv"; then
        echo "  same-key($key): byte-identical PASS"
    else
        echo "  same-key($key): DIVERGED FAIL"; fail=1
    fi

    # Load-key legs: the state saved at K is loaded at L, once by the run
    # that saved it and once by a fresh process. Host state a state file
    # omits differs from the saved timeline's in both, so a post-load row
    # diverges from clean.
    lkey="${R_LKEY[$i]}"
    [ "$lkey" = - ] && continue
    lk_state="$OUT/$name-lk.state"
    rm -f "${OUT:?}/$name-lk.state"
    row_run "$i" "$OUT/$name-lk.csv" \
        --state-save-on-frame "$key" --state-save-output "$lk_state" \
        --state-load-on-frame "$lkey" --state-load-input "$lk_state" \
        || { echo "  load-key run FAILED"; fail=1; continue; }
    ROW_CARD_NAME="$name-lk.csv.mcr"
    row_run "$i" "$OUT/$name-fl.csv" \
        --state-load-on-frame "$lkey" --state-load-input "$lk_state"
    fl_rc=$?
    ROW_CARD_NAME=""
    [ $fl_rc = 0 ] || { echo "  fresh-load run FAILED"; fail=1; continue; }
    for leg in lk fl; do
        if [ $leg = lk ]; then label="load-key($key->$lkey)"; else label="fresh-load($key->$lkey)"; fi
        if ! grep -q "loaded $lk_state" "$OUT/$name-$leg.csv.log" ||
           { [ $leg = lk ] && ! grep -q "saved $lk_state" "$OUT/$name-$leg.csv.log"; }; then
            echo "  $label: VACUOUS FAIL (save/load not logged)"; fail=1; continue
        fi
        grep -o "\(saved\|loaded\) $lk_state .*" "$OUT/$name-$leg.csv.log" |
            sed "s|$lk_state|state|; s/^/  $label: /"
        python3 - "$OUT/$name-clean.csv" "$OUT/$name-$leg.csv" "$label" <<'EOF2' || fail=1
import csv, sys
key = lambda r: (r[0], r[1])
clean = {key(r): r[2] for r in csv.reader(open(sys.argv[1])) if r and r[0] != 'frame'}
rows = [r for r in csv.reader(open(sys.argv[2])) if r and r[0] != 'frame']
# The load is the last row whose vbl does not follow its predecessor's.
cut = 0
for i in range(1, len(rows)):
    if int(rows[i][1]) - int(rows[i - 1][1]) not in (0, 1, 2, 3):
        cut = i
post = [r for r in rows[cut:] if key(r) in clean] if cut else []
div = [r for r in rows if key(r) in clean and clean[key(r)] != r[2]]
first = f", first at frame {div[0][0]} vbl {div[0][1]}" if div else ""
ok = cut > 0 and post and not div
print(f"  {sys.argv[3]}: {len(post)} post-load row(s) shared with clean, "
      f"{len(div)} diverged{first} -> " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
EOF2
    done

    # Card refusal leg: a process holding another card asks for the state,
    # which holds the saving run's card; the load is refused and the run
    # stays on the clean timeline.
    case "${R_CARD[$i]}" in -|none) continue ;; esac
    row_run "$i" "$OUT/$name-rf.csv" \
        --state-load-on-frame "$lkey" --state-load-input "$lk_state" \
        || { echo "  card-refused run FAILED"; fail=1; continue; }
    rf_n=$(grep -c "holds the card $OUT/$name-lk.csv.mcr in slot 1, which no slot holds; load refused" \
           "$OUT/$name-rf.csv.log")
    if [ "$rf_n" -eq 0 ]; then
        echo "  card-refused($lkey): no refusal logged VACUOUS FAIL"; fail=1
    elif grep -q "loaded $lk_state" "$OUT/$name-rf.csv.log"; then
        echo "  card-refused($lkey): the state loaded FAIL"; fail=1
    elif cmp -s "$OUT/$name-clean.csv" "$OUT/$name-rf.csv"; then
        echo "  card-refused($lkey): $rf_n refusal(s), byte-identical to clean PASS"
    else
        echo "  card-refused($lkey): $rf_n refusal(s), DIVERGED from clean FAIL"; fail=1
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
if [ ! -s "$OUT/ra-clean.csv" ] || [ ! -s "$OUT/ra-on.csv" ]; then
    echo "  runahead(2): not compared (a run wrote no trace) FAIL"; fail=1
elif cmp -s "$OUT/ra-clean.csv" "$OUT/ra-on.csv"; then
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
# Short-replay leg: OPENPETE_REWIND_FAULT_SHORT=1 ends the first rewind
# step's replay after its first replayed tick, as a yield that makes no
# tick ends it; the later steps of the hold rewind on from there. The
# input log must keep one entry per VBL up to the frame the timeline
# reached (offset moved: 0); at least one replay must end short of its
# target (engagement), and no row may differ from the clean run's row of
# its frame. The short step leaves the scrub at another frame than the
# driver leg's and its later steps repeat a frame, so the row sequence is
# not held to one backward jump.
PG_CAPTURE_ENV=(OPENPETE_REWIND_FAULT_SHORT=1)
run "$RA_MOVIE" "$OUT/drv-short.csv" \
    --rewind 30 --rewind-hold "$HOLD" \
    || { echo "  short-replay run FAILED"; fail=1; }
PG_CAPTURE_ENV=()
short_line="$(grep -o 'replays short of their target: [0-9]*; input log offset moved: [0-9]*' "$OUT/drv-short.csv.log" | tail -1)"
n_short="$(echo "$short_line" | awk '{print $6}' | tr -d ';')"
n_moved="$(echo "$short_line" | awk '{print $NF}')"
if [ -z "$short_line" ]; then
    echo "  short-replay: no rewind report line -> FAIL"; fail=1
elif [ "${n_short:-0}" -eq 0 ]; then
    echo "  short-replay: $short_line -> VACUOUS FAIL (no replay ended short)"; fail=1
elif [ "${n_moved:-1}" -ne 0 ]; then
    echo "  short-replay: $short_line -> FAIL"; fail=1
else
    echo "  short-replay: $short_line -> PASS"
fi
python3 - "$OUT/drv-clean.csv" "$OUT/drv-short.csv" <<'EOF' || fail=1
import csv, sys
clean = {r[0]: r[2] for r in csv.reader(open(sys.argv[1])) if r and r[0] != 'frame'}
rows = [r for r in csv.reader(open(sys.argv[2])) if r and r[0] != 'frame']
fr = [int(r[0]) for r in rows]
gaps = [(fr[i-1], fr[i]) for i in range(1, len(fr)) if fr[i] < fr[i-1]]
mism = [r[0] for r in rows if r[0] in clean and clean[r[0]] != r[2]]
ok = bool(gaps) and not mism
print(f"  short-replay driver: jumps={len(gaps)} mismatches={len(mism)} -> " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
EOF
fi

[ $fail -eq 0 ] && echo "ALL STATE ROUNDTRIP GATES PASS" || echo "STATE ROUNDTRIP GATES FAILED"
exit $fail
