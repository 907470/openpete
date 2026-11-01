#!/usr/bin/env bash
# emit-first-boot-costs.sh - measure a game's first-boot compile cost table:
# each translation unit's compile seconds over the compile step's total, from
# one headless first boot of the game's disc by a *-launcher build.
#
# The first boot's progress bar fills its build segment by these shares
# (games/common/recomp_build.cpp, compile_order): a source's compile time
# grows faster than its size, so a size-weighted bar runs ahead while the
# largest unit compiles. The table is written to
#   games/<game>/manifest/first_boot_costs.tsv
# and staged by CMake as <exe dir>/recomp/costs/<game>.tsv, outside the
# manifest tree the codegen stamp hashes. A unit the table does not name
# (a codegen change since the measurement) falls back to its size share.
# Re-run after a codegen change that adds, removes or reshapes units:
#   GAME=spyro-2 bash scripts/emit-first-boot-costs.sh [build-dir]
# Measure on an idle host: the shares are relative, but a loaded host
# stretches the units unevenly.
set -euo pipefail

GAME=${GAME:-spyro-1}
BUILD=${1:-build/linux-release-launcher}
case "$GAME" in
    spyro-1) CUE=games/spyro-1/spyro-1/build/SCUS-94228.cue ;;
    spyro-2) CUE=games/spyro-2/spyro-2/build/SCUS-94425.cue ;;
    spyro-3) CUE=games/spyro-3/spyro-3/build/SCUS-94467.cue ;;
    *) echo "[emit-first-boot-costs] unknown GAME $GAME" >&2; exit 2 ;;
esac
BIN="$BUILD/openpete"
OUT="games/$GAME/manifest/first_boot_costs.tsv"
[[ -x "$BIN" ]] || { echo "[emit-first-boot-costs] $BIN missing" >&2; exit 2; }
[[ -d "$BUILD/recomp/manifest/$GAME" ]] \
    || { echo "[emit-first-boot-costs] $BUILD has no first-boot inputs for $GAME (a *-launcher build)" >&2; exit 2; }
[[ -f "$CUE" ]] || { echo "[emit-first-boot-costs] $CUE missing" >&2; exit 2; }

SCRATCH=$(mktemp -d)
trap 'rm -rf "${SCRATCH:?}"' EXIT
rm -f "$OUT.new"
env XDG_DATA_HOME="$SCRATCH/data" XDG_STATE_HOME="$SCRATCH/state" XDG_CONFIG_HOME="$SCRATCH/config" \
    OPENPETE_MSGBOX_SKIP=1 OPENPETE_FIRST_BOOT_COSTS_OUT="$PWD/$OUT.new" \
    "$BIN" --headless --no-config --no-card --vibration 0 --mute 1 --ingest "$CUE" --frame-limit 1 \
    > "$SCRATCH/run.log" 2>&1 || { tail -20 "$SCRATCH/run.log" >&2; exit 1; }
[[ -s "$OUT.new" ]] || { echo "[emit-first-boot-costs] no table written" >&2; tail -20 "$SCRATCH/run.log" >&2; exit 1; }
mv "$OUT.new" "$OUT"
echo "[emit-first-boot-costs] $OUT: $(grep -vc '^#' "$OUT") units"
