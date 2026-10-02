#!/usr/bin/env bash
# Per-channel material gate: every row of mods/channel-gallery is a CLAIM
# about what the engine can select, and this turns each claim into an
# assertion. A row that should paint must paint; a row marked KNOWN GAP
# must paint nothing (a gap that quietly starts working is also news).
#
# Why this exists as its own gate: the gallery's claims were verified by
# hand, one screenshot at a time, and that is exactly how two real bugs
# got through - a texturedness filter that silently skipped the whole LP
# terrain tier, and per-instance material slots that were dead before
# they were stamped. Both were invisible to check_mods and check_native_
# visual, because neither runs a mod material at all.
#
# The last rung is the one with teeth: a refine material whose callback
# returns a DIFFERENT block per instance must repaint exactly what the
# equivalent DECLARATIVE selector repaints. Same audience, two routes,
# one number. Note the "different block per instance" - the shared-block
# version of that comparison passes straight through the slot bug, which
# is documented at the rung itself.
#
# Usage: tests/harness/check_mod_channels.sh [--game SLUG]
# Exit: 0 pass, 1 failures, 64 skip (no binary / disc / vulkan / PIL).
#
# The gallery mod, the bk2 corpus and every pinned frame below are Spyro 1's,
# so --game names the game whose binary and disc are used and refuses every
# other slug rather than replaying Spyro 1 frame pins against another game.
set -uo pipefail

cd "$(dirname "$0")/../.."
# Every run this script starts goes through pg_capture, which silences
# controller vibration for it.
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"
WORK="${TMPDIR:-/tmp}/openpete-modchan.$$"
fail=0

say() { printf '\n[modchan] %s\n' "$*"; }
ok()  { printf '[modchan]   PASS: %s\n' "$*"; }
bad() { printf '[modchan]   FAIL: %s\n' "$*"; fail=1; }

[ "$PG_GAME" = "spyro-1" ] || {
    echo "[modchan] SKIP: the gallery mod, the bk2 corpus and the frame pins" \
         "are spyro-1's, not $PG_GAME's"; exit 64; }
[ -x "$BIN" ] || { echo "[modchan] SKIP: no binary at $BIN"; exit 64; }
[ -f "$DISC" ] || { echo "[modchan] SKIP: no disc at $DISC"; exit 64; }
python3 -c "import PIL" 2>/dev/null || { echo "[modchan] SKIP: PIL unavailable"; exit 64; }

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# Stage one gallery copy per config so runs never share a config.toml.
# `extra` is appended verbatim, so a rung can drive gallery.class etc.
# STRENGTH overrides the highlight strength for one call; 0.0 makes every
# lit row an IDENTITY material (see the identity rungs below). CACHE=1 forces
# the retained terrain cache on (it is off by default on a software GPU) and
# turns on its per-walk census; EXTRA_ARGS appends CLI flags.
run_rows() {  # run_rows <tag> <bk2> <frame> <rows> <extra-toml>
    local tag=$1 bk2=$2 frame=$3 rows=$4 extra=$5
    local md="$WORK/mods-$tag" out="$WORK/$tag-$frame-vk.png"
    rm -rf "$md"                   # never stage over a previous rung's mod
    mkdir -p "$md"
    cp -r mods/channel-gallery "$md/"
    rm -rf "$md/channel-gallery/.build"
    printf 'gallery.rows = "%s"\ngallery.strength = %s\n%s\n' \
        "$rows" "${STRENGTH:-1.0}" "$extra" \
        > "$md/channel-gallery/config.toml"
    # pg_capture deletes the log and the capture before the run and asserts
    # the capture after, so no rung can diff the previous rung's image.
    local -a PG_CAPTURE_ENV=(
        OPENPETE_TERRAIN_CACHE_FORCE="${CACHE:-0}"
        OPENPETE_TERRAIN_GPU_CENSUS="${CACHE:-0}")
    local PG_CAPTURE_TIMEOUT=600
    pg_capture "$WORK/$tag.log" "$out" -- \
        --renderer vk --mute 1 --aspect 16:9 \
        --bk2 "$PG_BK2_DIR/$bk2.bk2" --screenshot-native-only \
        --mods-dir "$md" --screenshot-on-frame "$frame" \
        --screenshot-output "$WORK/$tag-{n}.png" \
        --frame-limit $((frame + 2)) --max-runtime-seconds 580 \
        ${EXTRA_ARGS:-}
    [ -s "$out" ]                  # caller checks; a missing capture is a hard error
}

# Last per-walk census line's <field>= value, or "" when the cache never ran.
census_field() {  # census_field <tag> <field>
    sed -n 's/.*\[tgpu-cen [^]]*\].*[ ]'"$2"'=\([0-9-]*\).*/\1/p' \
        "$WORK/$1.log" | tail -1
}

# Changed-pixel count between two captures.
diffpx() {
    python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
print(sum(1 for p in ImageChops.difference(a, b).getdata() if p != (0, 0, 0)))
PY
}

# ── baselines (gallery loaded, every row OFF) ─────────────────────────
# The baseline runs WITH the mod present but no row lit, so the gate
# measures the selector alone and not the cost of loading a mod.
for leg in "base_flame flame 1800" "base_portal cyclorama-portal 2000" \
           "base_bridge artisans-bridge 2000"; do
    set -- $leg
    if ! run_rows "$1" "$2" "$3" "" ""; then
        echo "[modchan] SKIP: no headless vk capture for $2 (see $WORK/$1.log)"
        grep -qiE "no vulkan|no display|failed to (create|open)" "$WORK/$1.log" && exit 64
        echo "[modchan] FAIL: baseline capture missing for $2"; exit 1
    fi
done

# ── per-channel rungs ─────────────────────────────────────────────────
# tag | bk2 | frame | row | min-px (0 = must paint NOTHING) | label
#
# Pin a frame where the channel's geometry is stable for several VBL either
# side, not where its reach is largest: a pose one VBL before the geometry
# leaves the screen reads as a healthy number until anything shifts the
# capture phase, and then reads as zero. The moby shadow sits on screen from
# ~1960 to 1998 (96-128 px across that window, 0 px from 1999); the refine
# band shrinks monotonically through the flame scene (2771 px at 1794 falling
# to 726 px by 1802), so its pin sits high on that slope.
RUNGS="
terrain|flame|1800|1|20000|terrain, both LOD tiers
moby|flame|1800|2|400|mobys
player|flame|1800|4|1500|player (Spyro is not a moby)
flameribbon|flame|1800|6|10000|flame ribbon (additive, textured)
particlequad|flame|1800|10|30|particle billboards (textured, blended)
shadowmoby|artisans-bridge|1986|8|60|moby shadows (DECAL profile: engine keeps the occlusion verdict)
shadowplayer|artisans-bridge|2000|9|0|player shadow (KNOWN GAP - and it must paint nothing, not paint LESS: this row DELETED the shadow, 3099 px, because deferring a tri removes it from the plain span and this bucket then skipped the spans)
sky|cyclorama-portal|2000|5|3000|cyclorama sky
portal|cyclorama-portal|2000|15|3000|portal previews (all three rungs)
terrainrefine|flame|1794|20|1000|terrain refine callback (untextured band)
"

while IFS='|' read -r tag bk2 frame row minpx label; do
    [ -z "${tag// }" ] && continue
    # The diff is the material's reach, so both sides must be the SAME
    # moment. A rung pinned away from its scene's baseline frame therefore
    # gets its own baseline at that frame, captured once and reused.
    case "$bk2" in
        cyclorama-portal) base="$WORK/base_portal-2000-vk.png"; scene_frame=2000 ;;
        artisans-bridge)  base="$WORK/base_bridge-2000-vk.png"; scene_frame=2000 ;;
        *)                base="$WORK/base_flame-1800-vk.png";  scene_frame=1800 ;;
    esac
    if [ "$frame" != "$scene_frame" ]; then
        btag="base_${bk2}_$frame"
        base="$WORK/$btag-$frame-vk.png"
        if [ ! -s "$base" ] && ! run_rows "$btag" "$bk2" "$frame" "" ""; then
            bad "no baseline for $bk2 at frame $frame (see $WORK/$btag.log)"; continue
        fi
    fi
    say "$label"
    if ! run_rows "$tag" "$bk2" "$frame" "$row" ""; then
        bad "no capture for row $row (see $WORK/$tag.log)"; continue
    fi
    n=$(diffpx "$base" "$WORK/$tag-$frame-vk.png")
    if [ "$minpx" -eq 0 ]; then
        # A known gap that starts painting is news, not a silent win: the
        # row's help text and the census both claim it cannot.
        if [ "${n:-0}" -eq 0 ]; then
            ok "row $row paints nothing, as its KNOWN GAP note claims"
        else
            bad "row $row painted $n px but is documented as a KNOWN GAP - either the engine gained a capability (update the gallery + census) or something else is leaking into this selector"
        fi
    elif [ "${n:-0}" -ge "$minpx" ]; then
        ok "row $row repainted $n px (>= $minpx)"
    else
        bad "row $row repainted only ${n:-0} px, expected >= $minpx"
    fi
done <<< "$RUNGS"

# ── identity materials ────────────────────────────────────────────────
# The rungs above prove a material REACHES its audience. These prove it
# reaches it and nothing else: a material that returns the stock shading
# unchanged (strength 0) must be pixel-identical to no material at all.
#
# It needs no colour classifier and no judgement about what looks right -
# the mod asked for nothing, so any changed pixel is the ENGINE's stage
# leaking, not the mod's. That is what makes it worth having: it is the
# only rung here that can fail because of something nobody wrote.
#
# It has already earned its place four times. The material contract never
# divided vUV/vColor by the perspective ratio vQ the way every stock
# fragment stage does (moby 3958 px -> 1); all three shipped material
# shaders wrote the PSX transparent-texel sentinel through instead of
# discarding it, painting cut-out holes solid on opaque draws; the portal
# override dropped the aperture stencil and painted sky over the wall
# (1785 px); and every override deleted og's per-pixel key contest, which
# is what moved terrain 3105 px until the engine took back main().
#
# TOL 4, not 0: the decode is exact but the pipelines differ, and a
# handful of edge fragments land on the far side of a rounding boundary.
IDTOL=4
say "identity materials repaint nothing they were not asked to"
for leg in "id_terrain flame 1800 1 terrain (both LOD tiers)" \
           "id_moby flame 1800 2 mobys" \
           "id_player flame 1800 4 player" \
           "id_sky cyclorama-portal 2000 5 sky" \
           "id_flame flame 1800 6 flame ribbon" \
           "id_particle flame 1800 10 particle billboards" \
           "id_shadow artisans-bridge 2000 8 moby shadows (decal)" \
           "id_portal cyclorama-portal 2000 15 portal previews" \
           "id_terref flame 1800 20 terrain refine callback"; do
    set -- $leg
    tag=$1 bk2=$2 frame=$3 row=$4; shift 4; label="$*"
    base="$WORK/base_flame-1800-vk.png"
    [ "$bk2" = "cyclorama-portal" ] && base="$WORK/base_portal-2000-vk.png"
    [ "$bk2" = "artisans-bridge" ]  && base="$WORK/base_bridge-2000-vk.png"
    if ! STRENGTH=0.0 run_rows "$tag" "$bk2" "$frame" "$row" ""; then
        bad "no capture for identity row $row (see $WORK/$tag.log)"; continue
    fi
    n=$(diffpx "$base" "$WORK/$tag-$frame-vk.png")
    if [ "${n:-999}" -le "$IDTOL" ]; then
        ok "$label: identity material is stock (${n:-?} px <= $IDTOL)"
    else
        bad "$label: identity material moved ${n:-?} px - the engine's own fragment work is being replaced, not the colour (perspective term? an occlusion contest?)"
    fi
done


# ── refine == declarative ─────────────────────────────────────────────
# A refine material whose callback accepts every instance selects the same
# audience as the plain declarative all-mobys selector, so it must repaint
# the same pixels.
#
# NOTE this rung alone is NOT sufficient, and it took writing it to see
# why: the gallery's refine row returns ONE shared block, and the
# stamped-slot bug only bit when a callback produced MANY DISTINCT
# outcomes. Measured against the buggy engine this pair reads 1586 vs
# 1586 - a clean pass straight through the defect. The many-outcome rung
# below is the one with teeth; this one covers the shared-block path.
say "refine material claiming everything == declarative all-mobys"
if run_rows "refine_all" flame 1800 19 $'gallery.class = -1\ngallery.refine_state = -1'; then
    nd=$(diffpx "$WORK/base_flame-1800-vk.png" "$WORK/moby-1800-vk.png")
    nr=$(diffpx "$WORK/base_flame-1800-vk.png" "$WORK/refine_all-1800-vk.png")
    if [ "${nr:-0}" -eq "${nd:-1}" ] && [ "${nr:-0}" -gt 0 ]; then
        ok "refine and declarative both repainted $nr px"
    else
        bad "refine repainted ${nr:-0} px, declarative ${nd:-0} - a per-instance material is losing (or gaining) instances"
    fi
else
    bad "no capture for the refine rung (see $WORK/refine_all.log)"
fi

# A callback that declines every instance must leave the frame stock.
say "refine material declining everything == no material at all"
if run_rows "refine_none" flame 1800 19 $'gallery.class = -1\ngallery.refine_state = 250'; then
    nz=$(diffpx "$WORK/base_flame-1800-vk.png" "$WORK/refine_none-1800-vk.png")
    if [ "${nz:-1}" -eq 0 ]; then
        ok "every instance declined, frame is stock"
    else
        bad "declining callback still repainted ${nz} px"
    fi
else
    bad "no capture for the decline rung (see $WORK/refine_none.log)"
fi

# ── retuning a live selector ──────────────────────────────────────────
# A material can be pointed at a different selector while it is live
# (material_set_selector), which is how the class picker follows the user's
# choice. Registering again instead is refused inside a present - where a UI
# section runs - so the retune is the ONLY path a picker has.
#
# Reached here through gallery.class_switch, which retunes from a PRESENT
# hook: the same context the picker uses, and the branch that has to defer to
# the tick boundary rather than splitting a frame between two keys. Both legs
# are pinned, because either half can pass alone for the wrong reason - a
# retune that silently did nothing still matches the BEFORE class, and one
# that matched everything would differ from it too.
#
# Classes 348 and 356 are both drawn at this pose (695 px / 440 px), so
# neither leg can pass by selecting nothing.
say "retuning a live selector re-keys the material"
if run_rows "retune_a" flame 1800 3 'gallery.class = 348' &&
   run_rows "retune_b" flame 1800 3 'gallery.class = 356' &&
   run_rows "retune_s" flame 1800 3 \
        $'gallery.class = 348\ngallery.class_switch = "1000:356"'; then
    na=$(diffpx "$WORK/base_flame-1800-vk.png" "$WORK/retune_a-1800-vk.png")
    nb=$(diffpx "$WORK/base_flame-1800-vk.png" "$WORK/retune_b-1800-vk.png")
    ns=$(diffpx "$WORK/retune_s-1800-vk.png" "$WORK/retune_b-1800-vk.png")
    nx=$(diffpx "$WORK/retune_s-1800-vk.png" "$WORK/retune_a-1800-vk.png")
    if [ "${na:-0}" -gt 0 ] && [ "${nb:-0}" -gt 0 ] &&
       [ "${ns:-1}" -eq 0 ] && [ "${nx:-0}" -gt 0 ]; then
        ok "retuned material matches the class it was pointed at ($nx px from the class it left)"
    else
        bad "retune: before=${na:-0} px after=${nb:-0} px, retuned-vs-after=${ns:-1} px (want 0), retuned-vs-before=${nx:-0} px (want >0)"
    fi
else
    bad "no capture for the retune rung (see $WORK/retune_s.log)"
fi

# ── many distinct outcomes == the same audience ───────────────────────
# The rung with teeth. mods/moby-instance-tint's callback returns a
# DIFFERENT block per instance, so it interns a slot per moby instead of
# one shared slot. It must still claim exactly the audience the plain
# all-mobys selector claims - every moby, no more, no fewer.
#
# This is the shape that caught the real bug: slot ids were interned
# during the extraction walk and stamped into the tag after
# scene_begin_frame had reset the intern table, so the high ids named
# nothing and most instances fell back to stock. Shared-block callbacks
# sailed through it (the table refills from 1); this one read 218 against
# the declarative 1586.
say "many-distinct-outcome refine == declarative all-mobys"
tintmd="$WORK/mods-tint"
rm -rf "$tintmd"
mkdir -p "$tintmd"
cp -r mods/moby-instance-tint "$tintmd/"
rm -rf "$tintmd/moby-instance-tint/.build"
PG_CAPTURE_ENV=(OPENPETE_MATERIAL_STATS=1)
PG_CAPTURE_TIMEOUT=300
pg_capture "$WORK/tint.log" "$WORK/tint-1800-vk.png" -- \
    --renderer vk --mute 1 --aspect 16:9 \
    --bk2 "$PG_BK2_DIR/flame.bk2" --screenshot-native-only \
    --mods-dir "$tintmd" --screenshot-on-frame 1800 \
    --screenshot-output "$WORK/tint-{n}.png" \
    --frame-limit 1802 --max-runtime-seconds 280
unset PG_CAPTURE_ENV PG_CAPTURE_TIMEOUT
if [ -s "$WORK/tint-1800-vk.png" ]; then
    # Its baseline is the no-mods frame: this mod has no gallery rows.
    if ! run_rows nomods flame 1800 "" ""; then
        bad "no no-mods baseline for the many-outcome rung"
    else
        # The gallery baseline has the mod loaded but inert, so both
        # baselines are the same image; compare against the tint mod's own.
        nt=$(diffpx "$WORK/nomods-1800-vk.png" "$WORK/tint-1800-vk.png")
        nd2=$(diffpx "$WORK/base_flame-1800-vk.png" "$WORK/moby-1800-vk.png")
        if [ "${nt:-0}" -eq "${nd2:-1}" ] && [ "${nt:-0}" -gt 0 ]; then
            ok "per-instance blocks reached all $nt px the declarative selector does"
        else
            bad "per-instance refine repainted ${nt:-0} px, declarative ${nd2:-0} - instances are being lost (check that slot ids are resolved where they are STAMPED, not banked during the walk)"
        fi
        # Pixel-count equality is necessary but NOT sufficient, and finding
        # that out is why this line exists: a variant where every instance
        # collapses onto ONE slot repaints exactly the same pixels (each
        # moby is tinted, just all the same wrong colour) and sails past
        # the comparison above. The interned-slot census is the structural
        # witness that the outcomes really are per-instance. A hash over
        # distinct moby addresses cannot legitimately produce one slot.
        peak=$(grep -oE 'per-instance slots interned this present = [0-9]+' \
               "$WORK/tint.log" | tail -1 | grep -oE '[0-9]+$')
        if [ "${peak:-0}" -ge 4 ]; then
            ok "callback produced $peak distinct interned outcomes"
        else
            bad "only ${peak:-0} distinct outcomes interned from a per-vaddr hash - instances are collapsing onto a shared slot, so they render with each other's parameters"
        fi
    fi
else
    bad "no capture for the many-outcome rung (see $WORK/tint.log)"
fi

# ── residency invariance (terrain) ────────────────────────────────────
# A face's shading is a function of the face, never of where the face
# happens to live. Terrain is the only channel where that can be tested,
# because it is the only one with two residencies: the CPU walk, and the
# retained GPU cache that redraws baked arenas without walking.
#
# Two legs, same material, same frame, same build - one with the cache on
# and one with --terrain-cache 0. The number is 0, not "within tolerance":
# both legs render the same frame.
#
# Two things this rung has to get right, and both were learned the hard way.
#
# It MUST assert the cache actually engaged, or it passes vacuously - a
# terrain material used to claim every sector back to the walk, which would
# make both legs walk and read 0 px having tested nothing. `secs` counts the
# sectors the cache is serving and must be nonzero. (There is no longer a
# claim reason for a material selector to check: the claim is gone, which is
# why `secs` alone now settles it.)
#
# And it must measure the material's residency delta against the STOCK one,
# not against zero. The cache's fp64 mirror of the walk is a handful of
# pixels off at 1 LSB even with no mods at all - 10 px at this frame. A
# material that repaints most of the screen HIDES those pixels, so a raw
# "== 0" reads green for the wide rows and red for narrow ones, which is
# backwards. What the law actually forbids is a material ADDING residency
# dependence, so the stock pair is the baseline.
say "a material adds no residency dependence of its own"
if CACHE=1 run_rows stockret flame 1800 "" "" &&
   CACHE=1 EXTRA_ARGS="--terrain-cache 0" run_rows stockwalk flame 1800 "" ""; then
    secs=$(census_field stockret secs)
    base_delta=$(diffpx "$WORK/stockret-1800-vk.png" "$WORK/stockwalk-1800-vk.png")
    if [ -z "$secs" ]; then
        # No census line at all: the cache never ran, so there is nothing to
        # compare. Skip loudly rather than report a green that means nothing.
        ok "SKIPPED: the retained terrain cache did not engage on this device"
    elif [ "${secs:-0}" -eq 0 ]; then
        bad "the cache served no sectors (secs=0) - both legs walked, so this rung proves nothing"
    else
        ok "stock residency delta is ${base_delta} px (the cache's own fp64 mirror; the bar to not exceed)"
        # row 1  = declarative, both terrain LOD tiers
        # row 20 = refine callback on the untextured band - the rung that
        #          says a per-present outcome survives being BAKED, which is
        #          the one thing a retained face cannot carry by itself.
        for leg in "resdecl 1 declarative terrain row" \
                   "resref 20 refine callback on terrain"; do
            set -- $leg
            tag=$1 row=$2; shift 2; label="$*"
            if CACHE=1 run_rows "$tag-r" flame 1800 "$row" "" &&
               CACHE=1 EXTRA_ARGS="--terrain-cache 0" run_rows "$tag-w" flame 1800 "$row" ""; then
                reach=$(diffpx "$WORK/stockret-1800-vk.png" "$WORK/$tag-r-1800-vk.png")
                n=$(diffpx "$WORK/$tag-r-1800-vk.png" "$WORK/$tag-w-1800-vk.png")
                if [ "${reach:-0}" -eq 0 ]; then
                    bad "$label: reached 0 px, so comparing residencies proves nothing - the row is not selecting anything at this frame"
                elif [ "${n:-999}" -le "${base_delta:-0}" ]; then
                    ok "$label: residency delta ${n} px <= stock ${base_delta} px (reached ${reach} px, cache engaged secs=$secs)"
                else
                    bad "$label: residency delta ${n} px EXCEEDS the stock ${base_delta} px - the material's reach or output depends on where the face lives, which strobes as the player moves"
                fi
            else
                bad "$label: no capture (see $WORK/$tag-r.log, $WORK/$tag-w.log)"
            fi
        done
    fi
else
    bad "no capture for the residency baseline (see $WORK/stockret.log, $WORK/stockwalk.log)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "[modchan] ALL GREEN"
else
    echo "[modchan] FAILURES - see above"
fi
exit $fail
