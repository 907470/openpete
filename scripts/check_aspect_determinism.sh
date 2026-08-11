#!/usr/bin/env bash
# check_aspect_determinism.sh — gameplay-state regression gate.
#
# Runs the headless demo (no input, title-screen autoplay) at two
# aspect ratios and asserts that gameplay-readable state is bit-
# identical between them.
#
# PRIMARY check: per-frame FNV1a-64 hash over the gameplay-state
# regions defined in manifest/gameplay_state_regions.toml. Catches
# any leak that perturbs gameplay state, not just leaks that have
# already propagated to Spyro's position. First-divergent frame is
# the precise moment a new render→gameplay channel woke up.
#
# SECONDARY check (kept for backwards compat): per-frame Spyro
# position log. Coarser proxy; only fires if the leak has reached
# Spyro physics. Useful as a confirmation when triaging a hash
# divergence ("did it actually move Spyro yet?").
#
# This is the regression gate for the
# "gameplay 100% decoupled from presentation" goal. Any code change
# that re-couples a render-side decision into gameplay state will
# show up as a divergent byte here. See:
#   notes/plans/spyro-moby-update-vs-render.md
#   notes/plans/openpete-widescreen-fov.md
#
# Usage:
#   scripts/check_aspect_determinism.sh
#     [--build-dir=build/linux-release]
#     [--frames=N]                 (default: 8000)
#     [--aspects=4:3,16:9]         (comma list)
#     [--baseline=4:3]
#     [--out=/tmp/aspect-determinism]
#
# Exit codes:
#   0 — all aspects match the baseline
#   1 — one or more aspects diverged
#   2 — setup error (missing binary / disc)

set -uo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="build/linux-release"
FRAMES=8000
ASPECTS_CSV="4:3,16:9"
BASELINE="4:3"
OUT_DIR="/tmp/aspect-determinism"

for arg in "$@"; do
    case "$arg" in
        --build-dir=*) BUILD_DIR="${arg#--build-dir=}" ;;
        --frames=*)    FRAMES="${arg#--frames=}" ;;
        --aspects=*)   ASPECTS_CSV="${arg#--aspects=}" ;;
        --baseline=*)  BASELINE="${arg#--baseline=}" ;;
        --out=*)       OUT_DIR="${arg#--out=}" ;;
        --help|-h)     sed -n '1,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)             echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

BIN="$BUILD_DIR/games/spyro-1/openpete-spyro1"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"

[[ -x "$BIN"  ]] || { echo "missing binary: $BIN (run cmake --build --preset linux-release)" >&2; exit 2; }
[[ -f "$DISC" ]] || { echo "missing disc: $DISC" >&2; exit 2; }

mkdir -p "$OUT_DIR"
IFS=',' read -ra ASPECTS <<< "$ASPECTS_CSV"

run_one() {
    local label="$1"; local aspect_arg="$2"
    local pos="$OUT_DIR/pos-$label.csv"
    local hash="$OUT_DIR/hash-$label.csv"
    local log="$OUT_DIR/log-$label.txt"
    # --no-config: a personal openpete.toml must not sway the comparison.
    local args=(--headless --no-config --disc "$DISC" --run-recomp 0x80012204
                --mute 1
                --frame-limit "$FRAMES"
                --position-log "$pos"
                --gameplay-hash-log "$hash"
                --max-runtime-seconds 600)
    [[ -n "$aspect_arg" ]] && args+=(--aspect "$aspect_arg")
    "$BIN" "${args[@]}" > "$log" 2>&1
    local rc=$?
    [[ $rc -eq 0 && -s "$pos" && -s "$hash" ]] || {
        echo "FAIL: $label run exited $rc" >&2; return 1; }
    # Strip vbl from both — vbl timing can wobble cosmetically without
    # indicating gameplay drift. The position-log strip is a 4-column
    # frame,x,y,z extract; the hash log keeps every per-region column
    # (frame, then total fnv1a64, then one column per region) so the
    # classifier can pinpoint which region first diverged.
    awk -F, 'NR>1 { print $1","$3","$4","$5 }' "$pos"  > "$OUT_DIR/x-$label.csv"
    awk -F, 'BEGIN { OFS="," }
             { $2=""; sub(/,,/, ","); print }' "$hash" > "$OUT_DIR/h-$label.csv"
}

# Run the baseline first.
case "$BASELINE" in
    4:3) baseline_arg="" ;;
    *)   baseline_arg="$BASELINE" ;;
esac
echo "[determinism] baseline = $BASELINE  ($FRAMES frames)"
run_one "$BASELINE" "$baseline_arg" || exit 1

declare -i fail=0
for a in "${ASPECTS[@]}"; do
    [[ "$a" == "$BASELINE" ]] && continue
    echo "[determinism] checking $a vs $BASELINE"
    run_one "$a" "$a" || { fail=1; continue; }
    # Primary: hash log (catches all gameplay-state divergence).
    hash_pass=1
    if diff -q "$OUT_DIR/h-$BASELINE.csv" "$OUT_DIR/h-$a.csv" > /dev/null; then
        :  # hash green
    else
        hash_pass=0
        nh=$(diff "$OUT_DIR/h-$BASELINE.csv" "$OUT_DIR/h-$a.csv" \
              | awk '/^[<>]/ {n++} END {print n+0}')
        first_h=$(diff "$OUT_DIR/h-$BASELINE.csv" "$OUT_DIR/h-$a.csv" \
                  | awk -F, '/^[<>] [0-9]/ { sub(/^[<>] /,""); print $1; exit }')
        echo "  FAIL  $a hash diverges from $BASELINE: $nh frames differ (first at frame ~$first_h)"
        echo "        compare: diff $OUT_DIR/h-$BASELINE.csv $OUT_DIR/h-$a.csv"
        # Region-level classification: name the channel that owns
        # whichever region first diverged. Stays best-effort — if
        # python3 / the registry is missing we skip silently and the
        # raw diff command above is still actionable.
        if [[ -x "$(command -v python3)" ]]; then
            python3 tools/classify_hash_divergence.py \
                "$OUT_DIR/h-$BASELINE.csv" "$OUT_DIR/h-$a.csv" \
                | sed 's/^/        /' || true
        fi
    fi
    # Secondary: position log (coarser; useful for "did it reach Spyro yet?").
    if diff -q "$OUT_DIR/x-$BASELINE.csv" "$OUT_DIR/x-$a.csv" > /dev/null; then
        if [[ $hash_pass -eq 1 ]]; then
            echo "  PASS  $a is bit-identical to $BASELINE across $FRAMES frames (hash + position)"
        else
            echo "        position log identical (leak hasn't propagated to Spyro physics yet)"
        fi
    else
        ndiff=$(diff "$OUT_DIR/x-$BASELINE.csv" "$OUT_DIR/x-$a.csv" \
                | awk '/^[<>]/ {n++} END {print n+0}')
        first=$(diff "$OUT_DIR/x-$BASELINE.csv" "$OUT_DIR/x-$a.csv" \
                | awk -F, '/^[<>] [0-9]/ { sub(/^[<>] /,""); print $1; exit }')
        echo "        position log: $ndiff line diffs (first at frame ~$first)"
    fi
    [[ $hash_pass -eq 0 ]] && fail=1
done

# Channel registry context: any divergence above is "expected red"
# until every open channel gets a substitution. Print the dashboard
# so the failure message includes how many are still expected open.
if [[ -x "$(command -v python3)" && -f "manifest/render_channels.toml" ]]; then
    echo -n "[determinism] "
    python3 tools/check_channel_registry.py 2>&1 || true
fi

exit "$fail"
