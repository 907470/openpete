#!/usr/bin/env bash
# check_s3_portals.sh - Spyro 3 homeworld level entries from a 117% save.
#
# Every row replays a committed bk2 from the title on a scratch copy of
# games/spyro-3/cards/117-percent.mcr: the movie loads slot 1 (the
# s3-load-save.bk2 input, Midnight Mountain live), then drives the game to
# one level entry in a homeworld and through it:
#   portal   pause -> Atlas -> warp (Square) into the destination level, pause
#            -> Exit Level (state 7 glide back to the hub, Spyro in front of
#            the destination's portal), then the stick steers Spyro into the
#            portal: state 7 into the destination.
#   trigger  the same route to a trigger moby that calls fWarpToLevel
#            (func_800584BC) type 0: state 5 into the destination.
#   balloon  Spyro walks to the Midnight Mountain balloonist (class 516),
#            bumps it, picks the destination in its menu: state 12
#            (GAMESTATE_LOADING_VEHICLE) into the destination.
# The inputs are the movie's own rows (frame-keyed); the movies were
# recorded closed-loop against the portal records of the hub's portal
# table (D_8006C530, D_8006C5E0 records of 0x44 bytes: five vertices,
# +0x40 state, +0x42 destination level).
#
# Instrument: the OPENPETE_S3_STATE_LOG lines (game.state, loadStage,
# currentLevel, live per change).
# Pass: after a live line in the row's hub that follows the movie's state-5
# load of the destination (the atlas warp; for a balloon row, the save's
# load), a line with the row's entry state (7, 5 or 12) and
# currentLevel = destination, then a live=1 line in the destination; the
# run exits 0 (the movie ends at the bk2's last row, --bk2-stop-on-end).
# The verdict names the frame of the destination's live line. Engagement:
# a row whose run never reaches its hub after that load is VACUOUS,
# which fails the row. A watchdog trip (exit 2), a crash or a timeout fails
# the row with the state-log tail.
#
# Card: the committed card is copied per row into the output directory and
# hashed before the first run; after every run the committed card is
# compared with that hash, and a change fails the harness. Runs never open
# the default cards (--card1 names the copy, port B is empty).
#
# Manifest games/spyro-3/corpus/portal-entry.manifest, pipe-separated:
#   name | kind | hub | dest
# The bk2 is games/spyro-3/bk2/<name>.bk2.
#
#   tests/harness/check_s3_portals.sh [--build-dir=DIR] [--filter=NAME]
#
# Exit 0 PASS, 2 any FAIL (or a missing manifest, bk2 or card), 64 SKIP
# (binary or disc missing). At most S3_PORTALS_JOBS (default 6) game
# processes run at once.
set -uo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
export OPENPETE_VIBRATION=0

BUILD="build/linux-release"; FILTER=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
    esac
done
GAME=spyro-3
PG_BUILD_DIR="$BUILD" pg_resolve "$GAME" || exit 64
if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "check_s3_portals: SKIP ($PG_BIN or $PG_DISC missing)"; exit 64
fi
MANIFEST="games/spyro-3/corpus/portal-entry.manifest"
CARD="games/spyro-3/cards/117-percent.mcr"
[[ -f "$MANIFEST" ]] || { echo "check_s3_portals: FAIL (no manifest $MANIFEST)"; exit 2; }
[[ -f "$CARD" ]] || { echo "check_s3_portals: FAIL (no card $CARD)"; exit 2; }
OUT="${S3_PORTALS_OUT:-$BUILD/s3-portals}"
JOBS="${S3_PORTALS_JOBS:-6}"
# The game's boot flags without --no-card: the rows need the card in port A.
RUN_ARGS=()
for a in "${PG_RUN_ARGS[@]}"; do [[ "$a" == --no-card ]] || RUN_ARGS+=("$a"); done

mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/*.rc "$OUT"/*.mcr "$OUT"/*.cardcheck

CARD_SHA="$(sha256sum < "$CARD")"

NAMES=(); KINDS=(); HUBS=(); DESTS=()
while IFS='|' read -r name kind hub dest; do
    name="${name//[[:space:]]/}"
    [[ -z "$name" || "$name" == \#* ]] && continue
    [[ -n "$FILTER" && "$name" != "$FILTER" ]] && continue
    NAMES+=("$name"); KINDS+=("${kind//[[:space:]]/}")
    HUBS+=("${hub//[[:space:]]/}"); DESTS+=("${dest//[[:space:]]/}")
done < "$MANIFEST"
if (( ${#NAMES[@]} == 0 )); then
    echo "check_s3_portals: FAIL (no rows in $MANIFEST${FILTER:+ matching $FILTER})"; exit 2
fi

# run NAME: one headless replay of the row's bk2 on its own card copy; log,
# exit code and the committed card's post-run hash check under $OUT.
run() {
    local name="$1" bk2="games/spyro-3/bk2/$1.bk2"
    if [[ ! -f "$bk2" ]]; then echo "missing $bk2" >"$OUT/$name.log"; echo 66 >"$OUT/$name.rc"; return; fi
    cp "$CARD" "$OUT/$name.mcr"
    env OPENPETE_S3_STATE_LOG=1 timeout -k 10 900 "$PG_BIN" --headless --no-config --disc "$PG_DISC" \
        "${RUN_ARGS[@]}" --card1 "$OUT/$name.mcr" --no-card2 --mute 1 --mods "" \
        --bk2 "$bk2" --bk2-stop-on-end --frame-limit 20000 >"$OUT/$name.log" 2>&1
    echo $? >"$OUT/$name.rc"
    if [[ "$(sha256sum < "$CARD")" == "$CARD_SHA" ]]; then echo same >"$OUT/$name.cardcheck"
    else echo CHANGED >"$OUT/$name.cardcheck"; fi
}

# classify NAME KIND HUB DEST: prints "PASS <frame>" or "FAIL <reason>".
classify() {
    local name="$1" kind="$2" hub="$3" dest="$4" log="$OUT/$1.log" rc want
    [[ -f "$log" && -f "$OUT/$name.rc" ]] || { echo "FAIL no output"; return; }
    rc="$(cat "$OUT/$name.rc")"
    case "$kind" in portal) want=7;; trigger) want=5;; balloon) want=12;; *) echo "FAIL kind $kind"; return;; esac
    grep '^\[s3-state\]' "$log" | awk -v hub="$hub" -v dest="$dest" -v want="$want" -v rc="$rc" '
        { for (i = 2; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] } }
        !loaded && v["state"] == 5 && (want == 12 || v["level"] == dest) { loaded = 1 }
        loaded && !athub && v["level"] == hub && v["live"] == 1 && v["state"] == 0 { athub = 1; next }
        athub && !entry && v["state"] == want && v["level"] == dest { entry = v["frame"] }
        entry && !live && v["level"] == dest && v["live"] == 1 && v["state"] == 0 { live = v["frame"] }
        { last = $0 }
        END {
            if (!athub) { print "FAIL VACUOUS: never live in hub " hub " (exit " rc "); last: " last; exit }
            if (!entry) { print "FAIL no state " want " load into " dest " (exit " rc "); last: " last; exit }
            if (!live) { print "FAIL " dest " never live after the entry at frame " entry " (exit " rc "); last: " last; exit }
            if (rc != 0) { print "FAIL exit " rc " after " dest " live at frame " live; exit }
            print "PASS entry frame " entry ", " dest " live at frame " live
        }'
}

t0=$SECONDS
running=0
for name in "${NAMES[@]}"; do
    run "$name" &
    running=$((running + 1))
    if (( running >= JOBS )); then wait -n; running=$((running - 1)); fi
done
wait

status=0
for i in "${!NAMES[@]}"; do
    n="${NAMES[$i]}"
    verdict="$(classify "$n" "${KINDS[$i]}" "${HUBS[$i]}" "${DESTS[$i]}")"
    echo "${verdict%% *} $n: ${verdict#* }"
    if [[ "${verdict%% *}" != PASS ]]; then
        status=2
        grep '^\[s3-state\]' "$OUT/$n.log" | tail -4 | sed 's/^/    /'
        grep -m1 'watchdog: hang detected' "$OUT/$n.log" | sed 's/^/    /'
    fi
    if [[ "$(cat "$OUT/$n.cardcheck" 2>/dev/null)" != same ]]; then
        echo "FAIL $n: the committed card $CARD changed during the run"; status=2
    fi
done
echo "check_s3_portals: $(( ${#NAMES[@]} )) rows, $(( SECONDS - t0 )) s, $( (( status == 0 )) && echo PASS || echo FAIL)"
exit $status
