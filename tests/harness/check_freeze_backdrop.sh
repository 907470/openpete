#!/usr/bin/env bash
# check_freeze_backdrop.sh — the freeze backdrop has a source frame to freeze.
#
# Pause, the inventory and the level-exit screens draw the frozen world behind
# their panels. That backdrop is not re-rendered: the live compose path picks
# one presented frame as the freeze source, snapshots it, and every later
# present of the frozen state samples that snapshot. The picking is a
# predicate over the presented scene — the game's own triangles, the freeze bit
# clear, world geometry present, terrain from either the selected retained
# state or non-host-added WORLD triangles in the scene. When the predicate
# rejects every present of a run, nothing is ever snapshotted and the freeze
# overlay samples a cleared target: the backdrop is a flat fill of the clear
# colour instead of the world.
#
# The class is windowed-only. The freeze source is chosen in compose_variant,
# which runs on the live present path; a headless run emits no composed present
# at all and takes its snapshot at capture time by another route. So a headless
# capture, and with it the presentation golden, holds the frozen world whether
# or not the live predicate would ever have fired, and cannot separate a build
# that carries the defect from one that does not. This harness therefore drives
# a real window.
#
# Input: games/<slug>/corpus/freeze-backdrop-corpus.manifest, rows
#   <bk2> | <frame-limit> | <launch tokens> | world_min=<n>
# The bk2 replays to the frame limit, which is past the frame the backdrop is
# entered on. `launch` is whitespace-separated tokens, the same convention the
# presentation corpus manifest uses: a token spelled KEY=VALUE with an
# upper-case key is an environment entry, every other token a command-line
# argument. It carries the levers that turn the predicate's terrain clause
# hostile (a vertex-precision mode that disables the retained cache) and the
# ones that make the machine able to read the clause at all
# (OPENPETE_TERRAIN_CACHE_FORCE=1, without which a software rasteriser never
# initialises the cache and the clause passes trivially). Keys:
#   world_min=<n>     floor on the presents that qualified as the freeze source
#   presents_min=<n>  engagement floor on the composed presents (default 10)
#
# Both counts scale with how many presents the run got through, which is the
# host's render throughput over a fixed number of VBLs and not a property of
# the build: the same build on one machine spans a factor of four between runs.
# So the floors are set several times below the lowest measurement of the row,
# not near it. What separates a defective build is structural rather than
# marginal — its predicate rejects every present, so its count is zero however
# many presents the host managed.
#
# Instrument (stderr), OPENPETE_FREEZE_DBG=1, one line per composed present:
#   [freeze f=<n> stamp=<hex>] world=<0|1> n=<n> W=<n> A=<n> freeze=<hex>
#     tc_src=<n> drawn=<n> | <terrain globals and slots>
# The metric is the number of those lines with world=1, counted by
#   grep -cE '^\[freeze f=[0-9]+ stamp=[0-9a-f]+\] world=1 '
# Pass criterion, per row: that count is at least the row's world_min, which is
# itself above zero. Engagement: the total `[freeze` line count must reach the
# row's presents_min, else the run composed too few presents for the count to
# mean anything (a run that never brought a window up, a binary without the
# instrument, a host that starved the present thread) and the row is VACUOUS,
# never PASS and never FAIL.
#
# Every launch passes --background 1: the window comes up at its normal size
# and visible but takes no focus, so an unattended run does not steal the
# keyboard from whoever is at the machine. The engine prints its own window
# state and the harness asserts background=1 on it, so a run whose window did
# take the desk cannot be read as this check's result.
#
# Verdict line per row:
#   [freeze-backdrop] game=<g> bk2=<b> presents=<n> world=<n> world_min=<n>
#                     focus=<0|1> verdict=PASS|FAIL
#
# Usage:
#   tests/harness/check_freeze_backdrop.sh [--game SLUG] [--build-dir=DIR]
#                                    [--filter=SUBSTR] [--out DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest (the message
# names the path), 64 SKIP (no display, binary, disc or bk2 missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

# Unattended launcher: nobody is holding the pad and the buzzing appears in no
# log, so every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

FILTER=""
OUT=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
        --out=*)       OUT="${a#--out=}";;
    esac
done
prev=""
for a in "$@"; do
    case "$prev" in
        --filter) FILTER="$a";;
        --out)    OUT="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

# The window the class was measured in. Small enough that a software
# rasteriser reaches the pinned frame in minutes, large enough that the
# backdrop is a real frame.
WIDTH=664
HEIGHT=448
# Per-row wall-clock budget. The engine's own budget stays under the outer
# timeout so it exits first and leaves a readable log.
ROW_RUNTIME=900
ROW_TIMEOUT=960

MANIFEST="$(pg_corpus_manifest "$GAME" freeze-backdrop-corpus.manifest)"
pg_manifest "$MANIFEST" pipe
# A live window is the whole point of the check; a session with no display has
# nothing to measure and skips rather than reporting a verdict it did not earn.
[[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]] ||
    { echo "freeze-backdrop: SKIP (no display)"; exit 64; }
[[ -x "$PG_BIN" ]] || { echo "freeze-backdrop: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "freeze-backdrop: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/freeze-backdrop/$GAME"
mkdir -p "$OUT"

# One windowed replay of a row. The log is deleted before the run and asserted
# after it, so a stale log from an earlier build can never be read as this
# run's result. stdin is closed: the row loop reads the manifest on
# its own stdin, and a child that inherits it eats the rows that have not run.
run_row() {  # tag movie frame-limit
    local tag="$1" movie="$2" flim="$3"
    local log="$OUT/$tag.log"
    rm -f "$log"
    local -a bk2_arg=(); [[ -n "$movie" ]] && bk2_arg=(--bk2 "$movie")
    env OPENPETE_VIBRATION=0 OPENPETE_FREEZE_DBG=1 "${ROW_ENV[@]}" \
        timeout "$ROW_TIMEOUT" "$PG_BIN" \
        --no-config --disc "$PG_DISC" --disc-cache 0 --no-card --mute 1 \
        --vibration 0 --input-lock --background 1 \
        --renderer vk --window "${WIDTH}x${HEIGHT}" --vsync 0 \
        --perf-stats off --osd off \
        "${PG_RUN_ARGS[@]}" "${bk2_arg[@]}" \
        --frame-limit "$flim" --max-runtime-seconds "$ROW_RUNTIME" \
        "${ROW_ARGS[@]}" </dev/null >"$log" 2>&1
    local rc=$?
    [[ -s "$log" ]] || { echo "  row $tag produced no log (rc=$rc)"; return 1; }
    return 0
}

rows=0; failed=0; missing=0
while IFS='|' read -r bk2 flim launch keys; do
    bk2="${bk2//[[:space:]]/}"
    [[ -z "$bk2" || "${bk2:0:1}" == "#" ]] && continue
    flim="${flim//[[:space:]]/}"
    launch="${launch:-}"
    keys="${keys:-}"
    [[ -n "$FILTER" && "$bk2" != *"$FILTER"* ]] && continue

    if [[ "$bk2" == *.bk2 ]]; then
        movie="$PG_BK2_DIR/$bk2"
        [[ -f "$movie" ]] || { echo "freeze-backdrop: SKIP ($movie missing)"; exit 64; }
    else
        movie=""    # plain boot: the title screen and the attract demo
    fi
    rows=$((rows + 1))

    world_min=0; presents_min=10
    for tok in $keys; do
        case "$tok" in
            world_min=*)    world_min="${tok#world_min=}";;
            presents_min=*) presents_min="${tok#presents_min=}";;
        esac
    done
    if ! [[ "$world_min" =~ ^[0-9]+$ ]] || (( world_min == 0 )); then
        echo "freeze-backdrop: FAIL (row $bk2: world_min must be a positive integer, got '$world_min')"
        exit 1
    fi

    # The tag keys the log by the row's launch field as well as its movie, so
    # two rows replaying one bk2 under different levers do not overwrite each
    # other's evidence.
    tag="${bk2%.bk2}"
    if [[ -n "${launch// /}" ]]; then
        tag="$tag-$(printf '%s' "$launch" | md5sum | cut -c1-6)"
    fi

    pg_split_launch "$launch"
    if ! run_row "$tag" "$movie" "$flim"; then
        echo "[freeze-backdrop] game=$GAME bk2=$bk2 verdict=NO-RUN"
        missing=$((missing + 1)); continue
    fi
    log="$OUT/$tag.log"

    presents=$(grep -cE '^\[freeze f=[0-9]+ stamp=[0-9a-f]+\]' "$log")
    world=$(grep -cE '^\[freeze f=[0-9]+ stamp=[0-9a-f]+\] world=1 ' "$log")
    # The engine's own report of the window it created. --background is asked
    # for on every launch, so the line is always printed; its absence means the
    # window never came up.
    wstate=$(grep -o 'window state ([^)]*): background=[01] input_focus=[01]' "$log" | tail -1)
    focus="${wstate##*input_focus=}"; focus="${focus:-?}"

    if [[ "$wstate" != *"background=1"* ]]; then
        echo "[freeze-backdrop] game=$GAME bk2=$bk2 presents=$presents world=$world" \
             "world_min=$world_min verdict=FAIL reason=no --background window state in $log"
        failed=$((failed + 1)); continue
    fi
    if (( presents < presents_min )); then
        echo "[freeze-backdrop] game=$GAME bk2=$bk2 presents=$presents world=$world" \
             "world_min=$world_min focus=$focus VACUOUS: floor presents>=$presents_min" \
             "(see $log)"
        failed=$((failed + 1)); continue
    fi

    verdict=PASS
    (( world >= world_min )) || { verdict=FAIL; failed=$((failed + 1)); }
    echo "[freeze-backdrop] game=$GAME bk2=$bk2 presents=$presents world=$world" \
         "world_min=$world_min focus=$focus verdict=$verdict"
done <"$MANIFEST"

echo "[freeze-backdrop] game=$GAME rows=$rows failed=$failed noRun=$missing"
if (( rows == 0 )); then
    echo "freeze-backdrop: FAIL ($MANIFEST has no rows)"; exit 1
fi
(( missing || failed )) && exit 1
exit 0
