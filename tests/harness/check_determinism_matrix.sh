#!/usr/bin/env bash

# check_determinism_matrix.sh - broaden the determinism gate from
# "title-screen demo at 4:3 vs 16:9" to a matrix of
# {scenario × aspect × fov} cells, all asserted bit-identical to the
# scenario's canonical (4:3, default-fov) cell.
#
# A single-axis title-screen comparison misses a coupling that only
# triggers in-level (via a moby AI path, say), or only at 21:9. This
# script sweeps the product of presentation knobs and the BK2 corpus.
#
# Pass/fail policy: tools/classify_hash_divergence.py classifies each
# cell against the game's channel registry
# (games/<game>/manifest/render_channels.toml). Divergence in an `open`
# channel is EXPECTED (the gate is documented red until those channels
# are closed). Divergence in any other channel, or in a region no
# channel claims, is a REGRESSION; a game without a registry has no
# channels, so every divergent region of it is a REGRESSION. A cell the
# classifier cannot compare (missing log, header or row-count mismatch,
# no rows, columns that name no region of the game) is BROKEN. Both fail
# the run, as does a run that compared zero rows.
#
# Defaults are tuned for a "PR-tier" sweep that runs in a handful of
# minutes locally. For a full nightly-style sweep, pass --full or
# override the lists explicitly.
#
# Usage:
#   tests/harness/check_determinism_matrix.sh
#     [--game SLUG]                  (default spyro-1; the bk2 corpus and the
#                                     disc follow it)
#     [--build-dir=build/linux-release]
#     [--frames=N]                   (default per game: 2000; spyro-2 2573;
#                                     spyro-3 3173, its longest default movie
#                                     is 3162 rows)
#     [--aspects=4:3,16:9,21:9]      (comma list; baseline 4:3)
#     [--fovs=default,90]            ("default" = no --fov flag)
#     [--scenarios=LIST]             ("title" = no --bk2; otherwise a stem
#                                     under the game's bk2 directory). Default
#                                     per game, each covering a new-game boot,
#                                     a level load and a portal entry:
#       spyro-1  title,stone-hill-entry,inventory
#       spyro-2  title,start-new-game,summer-forest-collect-orb,
#                autumn-plains-skelos-badlands-portal-entry
#       spyro-3  title,new-game-title,crystal-islands-flame,
#                sunrise-spring-sunny-villa-portal-entry
#                                     plus every row of the game's
#                                     antipiracy-scan.manifest (below)
#     [--out=/tmp/determinism-matrix]
#     [--full]                       (4:3,16:9,21:9,32:9 × default,50,90,120
#                                     × all *.bk2 scenarios + title)
#     [--soak]                       full BK2 corpus, long-replay mode:
#                                    all *.bk2 (minus --exclude) at default
#                                    fov, frames=30000, --bk2-stop-on-end.
#                                    Use to soak for slow-burn channel leaks
#                                    that only surface deep into a movie.
#     [--exclude=tas,...]            soak/full: comma list of BK2 stems to
#                                    skip (default: tas,frame-time-test).
#     [--keep-logs]                  (don't delete per-cell stdout)
#
# Anti-piracy verdicts (Spyro 3): a cell whose log carries an
# `antipiracy: FAIL` line (a failed level scan or a store-watch hit,
# games/spyro-3/features/antipiracy_scan.h) fails the run. Every Spyro 3
# cell arms the store watch (OPENPETE_S3_SCAN_WATCH=1,
# games/spyro-3/scan_watch.h) and must print its exit row. A scenario
# listed in games/<game>/corpus/antipiracy-scan.manifest runs to the row's
# VBL limit in place of --frames, and each of its cells must log at least
# the row's count of `complete: PASS` scan lines, so a scan that stops
# running fails the run too. Without --scenarios, every manifest row joins
# the default scenario list. A scan row runs its baseline cell (4:3, default
# fov) only: the scans read code and data, never the presentation knobs, so
# the other cells would repeat the same verdict.
#
# Native feed: every cell runs with OPENPETE_HEADLESS_NATIVE=1, so the
# native renderer is up surfaceless and the game-side scene feeders run
# every tick under the cell's --aspect and --fov, as under a window; a cell
# whose `native-feed:` exit row is missing or fed no scene is BROKEN, since
# its hashes never saw the native path the knobs configure.
#
# Exit codes:
#   0 - every cell either matches its baseline or diverges only in
#       open-status channels (documented expected red), and at least one
#       row was compared.
#   1 - a cell is a REGRESSION or BROKEN, a cell failed an anti-piracy
#       verdict, or no row was compared.
#   2 - setup error.

set -uo pipefail

cd "$(dirname "$0")/../.."
# A failed thread-role check (openpete_thread_roles.h) aborts the run, so a
# role or seal violation on a gated path fails the gate instead of a log line.
export OPENPETE_THREAD_ROLE_FATAL=1
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 2

BUILD_DIR="build/linux-release"
ASPECTS_CSV="4:3,16:9,21:9"
FOVS_CSV="default,90"
case "$GAME" in
    spyro-2)
        FRAMES=2573
        SCENARIOS_CSV="title,start-new-game,summer-forest-collect-orb,autumn-plains-skelos-badlands-portal-entry" ;;
    spyro-3)
        FRAMES=3173
        SCENARIOS_CSV="title,new-game-title,crystal-islands-flame,sunrise-spring-sunny-villa-portal-entry" ;;
    *)
        FRAMES=2000
        SCENARIOS_CSV="title,stone-hill-entry,inventory" ;;
esac
FRAMES_DEFAULT="$FRAMES"
OUT_DIR="/tmp/determinism-matrix"
KEEP_LOGS=0
FULL=0
SOAK=0
EXCLUDE_CSV="tas,frame-time-test"
BK2_STOP_ON_END=0
SCENARIOS_GIVEN=0

for arg in "$@"; do
    case "$arg" in
        --build-dir=*) BUILD_DIR="${arg#--build-dir=}" ;;
        --frames=*)    FRAMES="${arg#--frames=}" ;;
        --aspects=*)   ASPECTS_CSV="${arg#--aspects=}" ;;
        --fovs=*)      FOVS_CSV="${arg#--fovs=}" ;;
        --scenarios=*) SCENARIOS_CSV="${arg#--scenarios=}"; SCENARIOS_GIVEN=1 ;;
        --out=*)       OUT_DIR="${arg#--out=}" ;;
        --full)        FULL=1 ;;
        --soak)        SOAK=1 ;;
        --exclude=*)   EXCLUDE_CSV="${arg#--exclude=}" ;;
        --keep-logs)   KEEP_LOGS=1 ;;
        --game|--game=*) ;;   # consumed by pg_parse_game above
        spyro-*)       ;;     # the two-word --game SLUG spelling
        --help|-h)     sed -n '1,78p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)             echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME"

# Build a list of bk2 stems on disk, minus the EXCLUDE_CSV list, plus
# leading "title" (no --bk2). Used by --full and --soak.
list_bk2_scenarios() {
    local exclude_csv="$1"
    local exclude_pat
    exclude_pat=$(echo "$exclude_csv" | tr ',' '|')
    local stems
    stems=$(ls "$PG_BK2_DIR"/*.bk2 2>/dev/null \
            | xargs -n1 basename \
            | sed 's/\.bk2$//' \
            | { if [[ -n "$exclude_pat" ]]; then grep -Ev "^($exclude_pat)$"; else cat; fi; } \
            | tr '\n' ',' \
            | sed 's/,$//')
    echo "title${stems:+,$stems}"
}

if [[ $FULL -eq 1 ]]; then
    ASPECTS_CSV="4:3,16:9,21:9,32:9"
    FOVS_CSV="default,50,90,120"
    SCENARIOS_CSV=$(list_bk2_scenarios "$EXCLUDE_CSV")
fi

if [[ $SOAK -eq 1 ]]; then
    # Soak: long-replay mode for hunting slow-burn / late-frame channel
    # leaks that 2000-frame PR-tier cells miss. Trade matrix breadth for
    # per-cell depth - default fov only, default aspect axis, full corpus,
    # `--bk2-stop-on-end` so each cell exits as soon as its movie ends
    # rather than spinning out to FRAMES.
    [[ "$ASPECTS_CSV" == "4:3,16:9,21:9" ]] && ASPECTS_CSV="4:3,16:9"
    [[ "$FOVS_CSV"    == "default,90"    ]] && FOVS_CSV="default"
    [[ "$FRAMES"      == "$FRAMES_DEFAULT" ]] && FRAMES=30000
    SCENARIOS_CSV=$(list_bk2_scenarios "$EXCLUDE_CSV")
    BK2_STOP_ON_END=1
fi

BIN="$PG_BIN"
DISC="$PG_DISC"

[[ -x "$BIN"  ]] || { echo "missing binary: $BIN (run cmake --build --preset linux-release)" >&2; exit 2; }
# Hold the build directory against a build for the whole run (build_lock.sh).
# shellcheck source=scripts/lib/build_lock.sh
source scripts/lib/build_lock.sh
op_build_read_lock "$BIN" determinism-matrix || exit 75
[[ -f "$DISC" ]] || { echo "missing disc: $DISC" >&2; exit 2; }

# The anti-piracy scan rows: stem -> "frames min_pass", in manifest order.
# Without --scenarios (and outside --full/--soak, which take every bk2), each
# row joins the default scenario list.
declare -A SCAN_ROWS=()
SCAN_MANIFEST="games/$GAME/corpus/antipiracy-scan.manifest"
if [[ -f "$SCAN_MANIFEST" ]]; then
    while read -r _stem _frames _min _rest; do
        [[ -z "$_stem" || "$_stem" == \#* ]] && continue
        SCAN_ROWS[$_stem]="$_frames $_min"
        if [[ $SCENARIOS_GIVEN -eq 0 && $FULL -eq 0 && $SOAK -eq 0 \
              && ",$SCENARIOS_CSV," != *",$_stem,"* ]]; then
            SCENARIOS_CSV+=",$_stem"
        fi
    done < "$SCAN_MANIFEST"
fi

mkdir -p "$OUT_DIR"
IFS=',' read -ra ASPECTS   <<< "$ASPECTS_CSV"
IFS=',' read -ra FOVS      <<< "$FOVS_CSV"
IFS=',' read -ra SCENARIOS <<< "$SCENARIOS_CSV"

# Build a safe filename tag from a (scenario, aspect, fov) triple.
cell_tag() {
    local scenario="$1" aspect="$2" fov="$3"
    echo "$scenario--${aspect/:/x}--fov-$fov"
}

declare -i antipiracy_failures=0 scan_passes_total=0 scan_cells=0

# Spyro 3: every cell arms the store watch over the code ranges the
# anti-piracy scans hash (games/spyro-3/scan_watch.h); a store there logs an
# `antipiracy: FAIL watch:` line, and a cell without the watch's exit row
# never armed it.
PG_CAPTURE_ENV=(OPENPETE_HEADLESS_NATIVE=1)
SCAN_WATCH=0
if [[ "$GAME" == "spyro-3" ]]; then
    PG_CAPTURE_ENV+=(OPENPETE_S3_SCAN_WATCH=1)
    SCAN_WATCH=1
fi
declare -i feed_scenes_total=0

# Check a cell's log for the native feed's exit row; prints the row and
# adds its fed count to the total. Returns 1 when the row is missing or the
# game fed no scene.
check_native_feed() {
    local tag="$1" log="$2" row fed
    row=$(grep -o 'native-feed: captures=.*' "$log" | tail -1 || true)
    fed=$(sed -n 's/.* fed=\([0-9]*\).*/\1/p' <<<"$row")
    if [[ -z "$row" || "${fed:-0}" == 0 ]]; then
        echo "    cell $tag: ${row:-no native-feed exit row (renderer never up)}; VACUOUS" >&2
        return 1
    fi
    feed_scenes_total+=fed
    echo "    cell $tag: $row"
}
declare -i watch_hits_total=0

# Check a cell's log for anti-piracy verdicts: any FAIL line fails the cell,
# a watched cell must print the watch's exit row, and a scan row's cell must
# log at least the row's count of passing scans.
check_antipiracy() {
    local scenario="$1" tag="$2" log="$3"
    local fails passes
    fails=$(grep -c 'antipiracy: FAIL' "$log" || true)
    passes=$(grep -c 'antipiracy: .* complete: PASS' "$log" || true)
    scan_passes_total+=passes
    if (( fails > 0 )); then
        antipiracy_failures+=1
        echo "    cell $tag: $fails anti-piracy FAIL line(s):" >&2
        grep 'antipiracy: FAIL' "$log" | head -5 | sed 's/^/      /' >&2
    fi
    if (( SCAN_WATCH )); then
        local row hits
        row=$(grep -o '\[s3-scan\] watch: .*' "$log" | tail -1 || true)
        if [[ -z "$row" ]]; then
            antipiracy_failures+=1
            echo "    cell $tag: no store-watch exit row (the watch never armed)" >&2
        else
            hits=$(sed -n 's/.* hits=\([0-9]*\).*/\1/p' <<<"$row")
            watch_hits_total+=${hits:-0}
        fi
    fi
    if [[ -n "${SCAN_ROWS[$scenario]:-}" ]]; then
        local min="${SCAN_ROWS[$scenario]#* }"
        if (( passes < min )); then
            antipiracy_failures+=1
            echo "    cell $tag: $passes passing scan(s), the row needs $min (scan stopped or never ran)" >&2
        else
            echo "    cell $tag: $passes passing scan(s) (row needs $min)"
        fi
    fi
}

# Run a single cell. Writes hash csv to $OUT_DIR/h-<tag>.csv. Returns
# 0 on successful run (exit code of openpete), nonzero otherwise.
run_cell() {
    local scenario="$1" aspect="$2" fov="$3"
    local tag; tag=$(cell_tag "$scenario" "$aspect" "$fov")
    local hash="$OUT_DIR/h-$tag.csv"
    local log="$OUT_DIR/log-$tag.txt"
    local frames="$FRAMES"
    [[ -n "${SCAN_ROWS[$scenario]:-}" ]] && frames="${SCAN_ROWS[$scenario]%% *}"
    local args=(--frame-limit "$frames"
                --gameplay-hash-log "$hash"
                --max-runtime-seconds 300)
    [[ "$aspect" != "4:3" ]] && args+=(--aspect "$aspect")
    [[ "$fov"    != "default" ]] && args+=(--fov "$fov")
    if [[ "$scenario" != "title" ]]; then
        local bk2="$PG_BK2_DIR/$scenario.bk2"
        [[ -f "$bk2" ]] || { echo "missing bk2: $bk2" >&2; return 2; }
        args+=(--bk2 "$bk2")
        [[ $BK2_STOP_ON_END -eq 1 ]] && args+=(--bk2-stop-on-end)
    fi
    # pg_capture deletes the hash log before the run and asserts it after, so
    # a cell that failed to start cannot be compared against its predecessor's
    # file. The size test additionally rejects an empty one.
    pg_capture "$log" "$hash" -- "${args[@]}"
    local rc=$?
    [[ -s "$hash" ]] || { echo "    cell $tag: openpete rc=$rc, no hash log produced" >&2; return 1; }
    check_antipiracy "$scenario" "$tag" "$log"
    check_native_feed "$tag" "$log" || return 1
    return 0
}

declare -i total_cells=0 regressions=0 expected_red=0 clean=0 broken_runs=0
declare -i rows_compared=0

# Compare two hash logs; classify the result. Echo a one-line verdict
# (CLEAN / expected-red / REGRESSION / BROKEN) with the rows compared.
# Increments the global counters as a side effect. The verdict is read
# from the classifier's exit status and its final `VERDICT <v> rows=<n>`
# line; an exit status and verdict line that do not agree, or a missing
# line, is BROKEN, so a classifier crash can never count as expected-red.
compare_cell() {
    local label="$1" baseline_csv="$2" cell_csv="$3"
    if [[ ! -s "$cell_csv" ]]; then
        broken_runs+=1
        printf "  %-50s BROKEN (no hash log)\n" "$label"
        return
    fi
    local out rc verdict rows
    out=$(python3 tools/classify_hash_divergence.py --game "$GAME" \
          "$baseline_csv" "$cell_csv" 2>&1)
    rc=$?
    verdict=$(printf '%s\n' "$out" | sed -n 's/^VERDICT \([A-Z]*\) rows=[0-9]*$/\1/p' | tail -n1)
    rows=$(printf '%s\n' "$out" | sed -n 's/^VERDICT [A-Z]* rows=\([0-9]*\)$/\1/p' | tail -n1)
    case "$rc:$verdict" in
        0:CLEAN)
            clean+=1
            rows_compared+=rows
            printf "  %-50s CLEAN (%s rows)\n" "$label" "$rows" ;;
        1:EXPECTED)
            expected_red+=1
            rows_compared+=rows
            printf "  %-50s expected-red (%s rows)\n" "$label" "$rows"
            printf '%s\n' "$out" | sed 's/^/      /' ;;
        1:REGRESSION)
            regressions+=1
            rows_compared+=rows
            printf "  %-50s REGRESSION (%s rows)\n" "$label" "$rows"
            printf '%s\n' "$out" | sed 's/^/      /' ;;
        *)
            broken_runs+=1
            printf "  %-50s BROKEN (classifier rc=%s verdict=%s)\n" \
                "$label" "$rc" "${verdict:-none}"
            printf '%s\n' "$out" | sed 's/^/      /' ;;
    esac
}

mode_tag="matrix"
[[ $FULL -eq 1 ]] && mode_tag="full"
[[ $SOAK -eq 1 ]] && mode_tag="soak"
echo "[$mode_tag] aspects=[${ASPECTS[*]}]  fovs=[${FOVS[*]}]  scenarios=[${SCENARIOS[*]}]  frames=$FRAMES${BK2_STOP_ON_END:+ stop-on-end=$BK2_STOP_ON_END}"
echo

for scenario in "${SCENARIOS[@]}"; do
    echo "── scenario: $scenario ──"
    base_tag=$(cell_tag "$scenario" "4:3" "default")
    base_csv="$OUT_DIR/h-$base_tag.csv"
    if ! run_cell "$scenario" "4:3" "default"; then
        printf "  %-50s BROKEN (baseline run failed)\n" "$scenario / 4:3 / default"
        broken_runs+=1
        total_cells+=1
        continue
    fi
    if [[ -n "${SCAN_ROWS[$scenario]:-}" ]]; then
        scan_cells+=1
        printf "  %-50s baseline only (anti-piracy scan row)\n" "$scenario / 4:3 / default"
        echo
        continue
    fi
    for aspect in "${ASPECTS[@]}"; do
        for fov in "${FOVS[@]}"; do
            [[ "$aspect" == "4:3" && "$fov" == "default" ]] && continue
            total_cells+=1
            label="$scenario / $aspect / fov=$fov"
            tag=$(cell_tag "$scenario" "$aspect" "$fov")
            cell_csv="$OUT_DIR/h-$tag.csv"
            if ! run_cell "$scenario" "$aspect" "$fov"; then
                broken_runs+=1
                printf "  %-50s BROKEN (run failed)\n" "$label"
                continue
            fi
            compare_cell "$label" "$base_csv" "$cell_csv"
        done
    done
    echo
done

# Print the game's registry context to keep the report self-explanatory.
REGISTRY="games/$GAME/manifest/render_channels.toml"
if [[ -f "$REGISTRY" ]]; then
    python3 tools/check_channel_registry.py --registry "$REGISTRY" \
        --state-regions "games/$GAME/manifest/gameplay_state_regions.toml" \
        2>/dev/null | sed "s/^/[$mode_tag] /"
else
    echo "[$mode_tag] $GAME has no channel registry ($REGISTRY): every divergent region is a REGRESSION"
fi

echo
WATCH_SUMMARY=""
(( SCAN_WATCH )) && WATCH_SUMMARY="  watch-hits=$watch_hits_total"
echo "[$mode_tag] game=$GAME cells: $total_cells  clean=$clean  expected-red=$expected_red  regressions=$regressions  broken=$broken_runs  rows-compared=$rows_compared  antipiracy-failures=$antipiracy_failures  scan-row-cells=$scan_cells  scans-passed=$scan_passes_total  native-fed-scenes=$feed_scenes_total$WATCH_SUMMARY"

if [[ $KEEP_LOGS -eq 0 ]]; then
    rm -f "$OUT_DIR"/log-*.txt
fi

# Broken cells are setup or comparison failures, not gameplay
# regressions; they are reported apart but fail the run the same way.
# Exit code: 1 on any regression, any broken cell, or zero rows
# compared; 0 if every cell was clean or expected-red.
if (( regressions > 0 )); then
    exit 1
fi
if (( antipiracy_failures > 0 )); then
    echo "[$mode_tag] FAIL: $antipiracy_failures cell(s) with a failed or missing anti-piracy verdict" >&2
    exit 1
fi
if (( broken_runs > 0 )); then
    echo "[$mode_tag] FAIL: $broken_runs cell(s) BROKEN - see $OUT_DIR/log-*.txt" >&2
    exit 1
fi
if (( rows_compared == 0 )); then
    echo "[$mode_tag] FAIL: VACUOUS - no cell compared a row" >&2
    exit 1
fi
exit 0
