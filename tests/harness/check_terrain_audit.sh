#!/usr/bin/env bash
# check_terrain_audit.sh - retained terrain cache state against a fresh bake
# of the current guest data, headless on llvmpipe.
#
# Instrument: OPENPETE_TERRAIN_AUDIT=<period> (games/common/renderer/terrain.c,
# taud_sector). Every <period>th tick, on its canonical build, each sector the
# cache owns is re-baked into a private sink and compared field by field with
# the host copy of the bytes the cache holds for it (chunks, their twins, cell
# records, previous-corner and split records, the colour-table entry). The
# run prints one `[ter-audit] summary` line at exit: audits, distinct sectors,
# audits of re-baked sectors, elements, faces, record and table words,
# mismatches, and the first mismatch (sector, arena, element, face, field).
#
# Legs, per game, the attract demo from the plain boot with the cache forced
# on (OPENPETE_TERRAIN_CACHE_FORCE=1, OPENPETE_HEADLESS_NATIVE=1 so the scene
# builds every tick) and env-anim colour sectors on their colour tables
# (OPENPETE_TERRAIN_CANIMNEAR=1):
#   clean   pass: audits > 0, rebaked > 0, mismatches == 0.
#   poison  OPENPETE_TERRAIN_AUDIT_POISON=<the clean leg's sample sector>
#           offsets one baked corner of that sector before each upload; pass:
#           the first mismatch names that sector and field c0x. Skipped by
#           --no-control.
# A leg with no summary line or zero audits is VACUOUS.
#
# Usage:
#   tests/harness/check_terrain_audit.sh [--game SLUG] [--lod original|extended]
#       [--frames N] [--period P] [--no-control] [--build-dir=DIR] [--out DIR]
# Defaults: spyro-2, original, 1500 VBLs, period 1.
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no binary or disc) or bad usage.
set -u

export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
OUT=""
GAME=spyro-2
LOD=original
FRAMES=1500
PERIOD=1
CONTROL=1
prev=""
for a in "$@"; do
    case "$prev" in
        --out)    OUT="$a";;
        --game)   GAME="$a";;
        --lod)    LOD="$a";;
        --frames) FRAMES="$a";;
        --period) PERIOD="$a";;
    esac
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --game=*)      GAME="${a#*=}";;
        --lod=*)       LOD="${a#*=}";;
        --frames=*)    FRAMES="${a#*=}";;
        --period=*)    PERIOD="${a#*=}";;
        --no-control)  CONTROL=0;;
        --out|--game|--lod|--frames|--period) ;;
        -h|--help)     sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 64;;
        *) [[ "$prev" =~ ^--(out|game|lod|frames|period)$ ]] ||
               { echo "terrain-audit: unknown argument $a" >&2; exit 64; };;
    esac
    prev="$a"
done
[[ -n "$OUT" ]] || OUT="$BUILD_DIR/terrain-audit/$GAME-$LOD"
mkdir -p "$OUT"

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64
[[ -x "$PG_BIN"  ]] || { echo "terrain-audit: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "terrain-audit: SKIP (no disc at $PG_DISC)"; exit 64; }

# run_leg TAG ENV...: one audited run; sets LEG_RC and LEG_SUM (the summary line).
run_leg() {
    local tag="$1"; shift
    local log="$OUT/$tag.log"
    PG_CAPTURE_ENV=(OPENPETE_TERRAIN_CACHE_FORCE=1 OPENPETE_HEADLESS_NATIVE=1
                    OPENPETE_TERRAIN_CANIMNEAR=1 "OPENPETE_TERRAIN_AUDIT=$PERIOD" "$@")
    PG_CAPTURE_TIMEOUT=3600
    pg_capture "$log" "" -- --renderer vk --mute 1 --vibration 0 --aspect 4:3 --no-card \
        --lod "$LOD" --frame-limit "$FRAMES" </dev/null
    LEG_RC=$?
    LEG_SUM="$(grep -a '^\[ter-audit\] summary' "$log" | tail -1)"
    echo "  $tag: rc=$LEG_RC ${LEG_SUM:-no summary line (log $log)}"
}

# field NAME: the summary's NAME= value (0 when absent).
field() { local v; v="$(sed -n "s/.* $1=\([^ ]*\).*/\1/p" <<<"$LEG_SUM")"; echo "${v:-0}"; }

fail=0
echo "[terrain-audit] game=$GAME lod=$LOD frames=$FRAMES period=$PERIOD"
run_leg clean
if [[ -z "$LEG_SUM" || "$(field audits)" == 0 ]]; then
    echo "terrain-audit: VACUOUS (clean leg audited nothing)"; fail=1
elif [[ "$(field rebaked)" == 0 ]]; then
    echo "terrain-audit: VACUOUS (clean leg audited no re-baked sector)"; fail=1
elif [[ "$(field mismatches)" != 0 ]]; then
    echo "terrain-audit: FAIL (clean leg: $(field mismatches) mismatches)"; fail=1
fi
sample="$(field sample_sec)"
if (( CONTROL && ! fail )); then
    run_leg poison "OPENPETE_TERRAIN_AUDIT_POISON=$sample"
    first="$(sed -n 's/.* first=\(.*\)$/\1/p' <<<"$LEG_SUM")"
    if [[ "$first" != *" sec=$sample "* || "$first" != *" field=c0x "* ]]; then
        echo "terrain-audit: FAIL (poison control on sector $sample not named: first=${first:-none})"
        fail=1
    fi
fi
echo "[terrain-audit] game=$GAME lod=$LOD verdict=$( (( fail )) && echo FAIL || echo PASS)"
exit "$fail"
