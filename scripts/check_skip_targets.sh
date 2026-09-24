#!/usr/bin/env bash
# check_skip_targets.sh — the skip-target row harness.
#
# Runs every row of games/<slug>/skip-targets.manifest headless. Each row is
# a target of --skip-to-cutscene (or --skip-to-area); the skip module prints
# one release line, "[skip-to-level] level playable — presents resume at
# frame=N" followed by the engagement values it reads back from RAM at the
# release edge (the cutscene index, the credits sequence, the return level,
# the area word), or its fail-safe line, "[skip-to-level] fail-safe release
# (no playable edge)", when the deadline passes first.
#
# A row passes only when its run exits 0, prints the release line, the
# release line carries every expect token of the row, and the run prints no
# fail-safe line. No release line, the fail-safe line, a missing token or a
# nonzero exit fails the row.
#
# Negative control: the first row runs a second time with
# OPENPETE_SKIP_FAILSAFE_VBL=1, which moves the skip's fail-safe deadline to
# one VBL after its first check. That run must classify as FAIL; a PASS there
# means the classifier cannot see a skip that never engaged, and the harness
# fails.
#
# Manifest fields, pipe-separated: name | args | frames | expect
#   args    the skip flags of the row (word-split)
#   frames  --frame-limit of the run, in VBLs
#   expect  whitespace-separated tokens the release line must contain
#
#   scripts/check_skip_targets.sh [--game SLUG] [--build-dir=DIR] [--filter=NAME]
#
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary, disc or manifest missing). At most
# SKIP_TARGETS_JOBS (default 2) game processes run at once.
set -uo pipefail
cd "$(dirname "$0")/.."
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
GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD" pg_resolve "$GAME" || exit 64
MANIFEST="games/$GAME/skip-targets.manifest"
OUT="${SKIP_TARGETS_OUT:-$BUILD/skip-targets/$GAME}"
JOBS="${SKIP_TARGETS_JOBS:-2}"

if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" || ! -f "$MANIFEST" ]]; then
    echo "check_skip_targets: SKIP ($PG_BIN, $PG_DISC or $MANIFEST missing)"; exit 64
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/*.rc

NAMES=(); ARGS=(); FRAMES=(); EXPECT=()
while IFS='|' read -r name args frames expect; do
    name="${name//[[:space:]]/}"
    [[ -z "$name" || "$name" == \#* ]] && continue
    [[ -n "$FILTER" && "$name" != "$FILTER" ]] && continue
    NAMES+=("$name"); ARGS+=("$(echo $args)")
    FRAMES+=("${frames//[[:space:]]/}"); EXPECT+=("$(echo $expect)")
done < "$MANIFEST"
if (( ${#NAMES[@]} == 0 )); then
    echo "check_skip_targets: FAIL (no rows in $MANIFEST${FILTER:+ matching $FILTER})"; exit 1
fi

# run TAG ARGS FRAMES [ENV...]: one headless session, log and exit code
# under $OUT.
run() {
    local tag="$1" args="$2" frames="$3"; shift 3
    local -a argv; read -ra argv <<<"$args"
    env "$@" timeout 900 "$PG_BIN" --headless --no-config --disc "$PG_DISC" \
        "${PG_RUN_ARGS[@]}" --no-card --mute 1 "${argv[@]}" \
        --frame-limit "$frames" >"$OUT/$tag.log" 2>&1
    echo $? >"$OUT/$tag.rc"
}

# classify TAG EXPECT: prints "PASS <line>" or "FAIL <reason>".
classify() {
    local tag="$1" expect="$2" log="$OUT/$1.log" rc line tok
    [[ -f "$log" && -f "$OUT/$tag.rc" ]] || { echo "FAIL no output"; return; }
    rc="$(cat "$OUT/$tag.rc")"
    if grep -q '\[skip-to-level\] fail-safe release' "$log"; then
        echo "FAIL fail-safe line: $(grep -m1 'fail-safe release' "$log")"; return
    fi
    line="$(grep -m1 '\[skip-to-level\] level playable' "$log")"
    [[ -n "$line" ]] || { echo "FAIL no release line (exit $rc)"; return; }
    for tok in $expect; do
        [[ " $line " == *" $tok "* ]] || { echo "FAIL token '$tok' absent: $line"; return; }
    done
    [[ "$rc" == 0 ]] || { echo "FAIL exit $rc: $line"; return; }
    echo "PASS $line"
}

# Rows and the negative control, JOBS at a time.
running=0
for i in "${!NAMES[@]}"; do
    run "${NAMES[$i]}" "${ARGS[$i]}" "${FRAMES[$i]}" &
    running=$((running + 1))
    if (( running >= JOBS )); then wait -n; running=$((running - 1)); fi
done
run "negative-${NAMES[0]}" "${ARGS[0]}" "${FRAMES[0]}" OPENPETE_SKIP_FAILSAFE_VBL=1 &
wait

status=0
for i in "${!NAMES[@]}"; do
    verdict="$(classify "${NAMES[$i]}" "${EXPECT[$i]}")"
    echo "${verdict%% *} ${NAMES[$i]}: ${verdict#* }"
    [[ "$verdict" == PASS* ]] || status=1
done
neg="$(classify "negative-${NAMES[0]}" "${EXPECT[0]}")"
if [[ "$neg" == PASS* ]]; then
    echo "FAIL negative control (negative-${NAMES[0]}) passed; the classifier cannot see a skip that never engaged: ${neg#* }"
    status=1
else
    echo "ok   negative control (negative-${NAMES[0]}) fails as required: ${neg#* }"
fi
echo "check_skip_targets: $GAME ${#NAMES[@]} rows: $( ((status)) && echo FAIL || echo PASS)"
exit $status
