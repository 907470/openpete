#!/usr/bin/env bash

# check_determinism_matrix.sh — broaden the determinism gate from
# "title-screen demo at 4:3 vs 16:9" to a matrix of
# {scenario × aspect × fov} cells, all asserted bit-identical to the
# scenario's canonical (4:3, default-fov) cell.
#
# Today's single-axis aspect_determinism gate is good at catching
# couplings that show up in the title-screen pre-roll, but a coupling
# that only triggers in-level (e.g. via a moby AI path), or only at
# 21:9, is invisible. This script closes that gap by sweeping the
# product of presentation knobs and the BK2 corpus.
#
# Pass/fail policy: divergence in an `open` channel is EXPECTED
# (gate is documented red until those channels are closed); divergence
# in a `gated`/`verified` channel is a REGRESSION (the harness exits
# non-zero). We piggyback on tools/classify_hash_divergence.py to make
# that distinction — any line tagged REGRESSION in any cell fails.
#
# Defaults are tuned for a "PR-tier" sweep that runs in a handful of
# minutes locally. For a full nightly-style sweep, pass --full or
# override the lists explicitly.
#
# Usage:
#   scripts/check_determinism_matrix.sh
#     [--build-dir=build/linux-release]
#     [--frames=N]                   (default 2000; per-cell frame cap)
#     [--aspects=4:3,16:9,21:9]      (comma list; baseline 4:3)
#     [--fovs=default,90]            ("default" = no --fov flag)
#     [--scenarios=title,stone-hill-entry,inventory]
#                                    ("title" = no --bk2; otherwise a stem
#                                     under games/spyro-1/bk2/)
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
# Exit codes:
#   0 — every cell either matches its baseline or diverges only in
#       open-status channels (documented expected red).
#   1 — at least one cell diverges in a gated/verified channel.
#   2 — setup error.

set -uo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."

BUILD_DIR="build/linux-release"
FRAMES=2000
ASPECTS_CSV="4:3,16:9,21:9"
FOVS_CSV="default,90"
SCENARIOS_CSV="title,stone-hill-entry,inventory"
OUT_DIR="/tmp/determinism-matrix"
KEEP_LOGS=0
FULL=0
SOAK=0
EXCLUDE_CSV="tas,frame-time-test"
BK2_STOP_ON_END=0

for arg in "$@"; do
    case "$arg" in
        --build-dir=*) BUILD_DIR="${arg#--build-dir=}" ;;
        --frames=*)    FRAMES="${arg#--frames=}" ;;
        --aspects=*)   ASPECTS_CSV="${arg#--aspects=}" ;;
        --fovs=*)      FOVS_CSV="${arg#--fovs=}" ;;
        --scenarios=*) SCENARIOS_CSV="${arg#--scenarios=}" ;;
        --out=*)       OUT_DIR="${arg#--out=}" ;;
        --full)        FULL=1 ;;
        --soak)        SOAK=1 ;;
        --exclude=*)   EXCLUDE_CSV="${arg#--exclude=}" ;;
        --keep-logs)   KEEP_LOGS=1 ;;
        --help|-h)     sed -n '1,55p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)             echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# Build a list of bk2 stems on disk, minus the EXCLUDE_CSV list, plus
# leading "title" (no --bk2). Used by --full and --soak.
list_bk2_scenarios() {
    local exclude_csv="$1"
    local exclude_pat
    exclude_pat=$(echo "$exclude_csv" | tr ',' '|')
    local stems
    stems=$(ls games/spyro-1/bk2/*.bk2 2>/dev/null \
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
    # per-cell depth — default fov only, default aspect axis, full corpus,
    # `--bk2-stop-on-end` so each cell exits as soon as its movie ends
    # rather than spinning out to FRAMES.
    [[ "$ASPECTS_CSV" == "4:3,16:9,21:9" ]] && ASPECTS_CSV="4:3,16:9"
    [[ "$FOVS_CSV"    == "default,90"    ]] && FOVS_CSV="default"
    [[ "$FRAMES"      == "2000"          ]] && FRAMES=30000
    SCENARIOS_CSV=$(list_bk2_scenarios "$EXCLUDE_CSV")
    BK2_STOP_ON_END=1
fi

BIN="$BUILD_DIR/games/spyro-1/openpete-spyro1"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"

[[ -x "$BIN"  ]] || { echo "missing binary: $BIN (run cmake --build --preset linux-release)" >&2; exit 2; }
[[ -f "$DISC" ]] || { echo "missing disc: $DISC" >&2; exit 2; }

mkdir -p "$OUT_DIR"
IFS=',' read -ra ASPECTS   <<< "$ASPECTS_CSV"
IFS=',' read -ra FOVS      <<< "$FOVS_CSV"
IFS=',' read -ra SCENARIOS <<< "$SCENARIOS_CSV"

# Build a safe filename tag from a (scenario, aspect, fov) triple.
cell_tag() {
    local scenario="$1" aspect="$2" fov="$3"
    echo "$scenario--${aspect/:/x}--fov-$fov"
}

# Run a single cell. Writes hash csv to $OUT_DIR/h-<tag>.csv. Returns
# 0 on successful run (exit code of openpete), nonzero otherwise.
run_cell() {
    local scenario="$1" aspect="$2" fov="$3"
    local tag; tag=$(cell_tag "$scenario" "$aspect" "$fov")
    local hash="$OUT_DIR/h-$tag.csv"
    local log="$OUT_DIR/log-$tag.txt"
    local args=(--headless --disc "$DISC" --run-recomp 0x80012204
                --frame-limit "$FRAMES"
                --gameplay-hash-log "$hash"
                --max-runtime-seconds 300)
    [[ "$aspect" != "4:3" ]] && args+=(--aspect "$aspect")
    [[ "$fov"    != "default" ]] && args+=(--fov "$fov")
    if [[ "$scenario" != "title" ]]; then
        local bk2="games/spyro-1/bk2/$scenario.bk2"
        [[ -f "$bk2" ]] || { echo "missing bk2: $bk2" >&2; return 2; }
        args+=(--bk2 "$bk2")
        [[ $BK2_STOP_ON_END -eq 1 ]] && args+=(--bk2-stop-on-end)
    fi
    "$BIN" "${args[@]}" > "$log" 2>&1
    local rc=$?
    [[ -s "$hash" ]] || { echo "    cell $tag: openpete rc=$rc, no hash log produced" >&2; return 1; }
    return 0
}

declare -i total_cells=0 regressions=0 expected_red=0 clean=0 broken_runs=0

# Compare two hash logs; classify the result. Echo a one-line verdict
# (CLEAN / EXPECTED / REGRESSION / BROKEN). Increments the global
# counters as a side effect.
compare_cell() {
    local label="$1" baseline_csv="$2" cell_csv="$3"
    if [[ ! -s "$cell_csv" ]]; then
        broken_runs+=1
        printf "  %-50s BROKEN (no hash log)\n" "$label"
        return
    fi
    local out
    out=$(python3 tools/classify_hash_divergence.py "$baseline_csv" "$cell_csv" 2>&1)
    if [[ -z "$out" || "$out" == *"MATCH bit-identical"* ]]; then
        clean+=1
        printf "  %-50s CLEAN\n" "$label"
        return
    fi
    if echo "$out" | grep -q "REGRESSION"; then
        regressions+=1
        printf "  %-50s REGRESSION\n" "$label"
        echo "$out" | sed 's/^/      /'
    else
        expected_red+=1
        printf "  %-50s expected-red\n" "$label"
    fi
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

# Print registry context to keep the report self-explanatory.
if [[ -x "$(command -v python3)" ]]; then
    python3 tools/check_channel_registry.py 2>/dev/null | sed "s/^/[$mode_tag] /"
fi

echo
echo "[$mode_tag] cells: $total_cells  clean=$clean  expected-red=$expected_red  regressions=$regressions  broken=$broken_runs"

if [[ $KEEP_LOGS -eq 0 ]]; then
    rm -f "$OUT_DIR"/log-*.txt
fi

# Broken runs are setup failures, not gameplay regressions; report
# them but don't conflate. Exit code: 1 if any regression OR any
# broken run; 0 if every cell was clean or expected-red.
if (( regressions > 0 )); then
    exit 1
fi
if (( broken_runs > 0 )); then
    echo "[$mode_tag] FAIL: $broken_runs cell(s) failed to run — see $OUT_DIR/log-*.txt" >&2
    exit 1
fi
exit 0
