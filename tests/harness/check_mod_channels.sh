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
# The pinned frames of the gallery rungs after the per-game block are
# Spyro 1's. --game spyro-2 or spyro-3 runs that game's per-channel
# engagement rungs instead: one declarative row per render channel against
# a disabled-row baseline, at pins from the game's own corpus. On every
# game the gallery's rows are then measured one by one against that game's
# row labels (gallery_rows_rung), with a no-mod control and the class
# picker's list at a named level.
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

[ -x "$BIN" ] || { echo "[modchan] SKIP: no binary at $BIN"; exit 64; }
[ -f "$DISC" ] || { echo "[modchan] SKIP: no disc at $DISC"; exit 64; }
python3 -c "import PIL" 2>/dev/null || { echo "[modchan] SKIP: PIL unavailable"; exit 64; }

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# ── an earlier scene binds its own material outcomes ──────────────────
# OPENPETE_NATIVE_SCENE_DELAY=1 makes the compose draw the history entry one
# scene back, headless included. mods/mat-fx-pulse claims every moby with a
# block whose colour flips with the tick's parity, so the history scene and
# the live one intern different bytes at the same slot index. Composed
# through the material view captured with the history entry, the delayed
# capture at F equals the undelayed capture of the history scene's own VBL
# F'; composed through the live table it binds the newest tick's colour and
# the mobys differ.
#
# F' is calibrated, not assumed: the stock pair (no mod) picks whichever of
# F-1 and F-2 the delayed compose reproduces most closely, and that pair's
# delta is the bar the mod pair may not exceed. Engagement: the mod repaints
# the F' capture, and the census counts view-installed resolves (refuses 0).
# delay_leg <tag> <scene> <frame> <delay> <mods-dir>
delay_leg() {
    local tag="$1" scene="$2" frame="$3" delay="$4" md="$5"
    local -a movie=(--bk2 "$PG_BK2_DIR/$scene.bk2") extra=()
    if [ -f "$PG_BK2_DIR/$scene.bk2.args" ]; then
        read -ra extra < <(grep -v '^#' "$PG_BK2_DIR/$scene.bk2.args" | tr '\n' ' ')
        movie+=("${extra[@]}")
    fi
    local -a PG_CAPTURE_ENV=(OPENPETE_NATIVE_SCENE_DELAY="$delay" OPENPETE_MATERIAL_STATS=1)
    local PG_CAPTURE_TIMEOUT=900
    pg_capture "$WORK/$tag.log" "$WORK/$tag-$frame-vk.png" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --no-card \
        "${movie[@]}" --screenshot-native-only --mods-dir "$md" \
        --screenshot-on-frame "$frame" \
        --screenshot-output "$WORK/$tag-{n}.png" --frame-limit $((frame + 2))
}
# delay_rung <scene> <frame>
delay_rung() {
    local scene="$1" f="$2" fp best="" bestn="" n
    local stock="$WORK/mods-delay-stock" pulse="$WORK/mods-delay-pulse"
    say "a delayed compose binds the history scene's own material block ($scene $f)"
    rm -rf "${stock:?}" "${pulse:?}"
    mkdir -p "$stock" "$pulse"
    : > "$stock/enabled.txt"
    cp -r mods/mat-fx-pulse "$pulse/"
    rm -rf "${pulse:?}/mat-fx-pulse/.build"
    echo mat-fx-pulse > "$pulse/enabled.txt"
    delay_leg dl_stock_d1 "$scene" "$f" 1 "$stock" ||
        { bad "no delayed stock capture (see $WORK/dl_stock_d1.log)"; return; }
    for fp in $((f - 1)) $((f - 2)); do
        delay_leg "dl_stock_d0_$fp" "$scene" "$fp" 0 "$stock" ||
            { bad "no stock capture at $fp (see $WORK/dl_stock_d0_$fp.log)"; return; }
        n=$(diffpx "$WORK/dl_stock_d1-$f-vk.png" "$WORK/dl_stock_d0_$fp-$fp-vk.png")
        if [ -z "$bestn" ] || [ "${n:-999999}" -lt "$bestn" ]; then best=$fp; bestn=${n:-999999}; fi
    done
    ok "the delayed compose at $f reproduces VBL $best within $bestn px (stock)"
    delay_leg dl_pulse_d1 "$scene" "$f" 1 "$pulse" &&
    delay_leg dl_pulse_d0 "$scene" "$best" 0 "$pulse" ||
        { bad "no pulse capture (see $WORK/dl_pulse_d1.log, $WORK/dl_pulse_d0.log)"; return; }
    local reach m views
    reach=$(diffpx "$WORK/dl_stock_d0_$best-$best-vk.png" "$WORK/dl_pulse_d0-$best-vk.png")
    m=$(diffpx "$WORK/dl_pulse_d1-$f-vk.png" "$WORK/dl_pulse_d0-$best-vk.png")
    views=$(sed -n 's/.*\[material-census\] view-installed resolves=\([0-9]*\).*/\1/p' \
            "$WORK/dl_pulse_d1.log" | tail -1)
    if [ "${reach:-0}" -eq 0 ]; then
        bad "mat-fx-pulse repainted 0 px at $best: the rung proves nothing"
    elif [ "${views:-0}" -eq 0 ]; then
        bad "the census counted ${views:-no} view-installed resolves on the delayed compose"
    elif [ "${m:-999999}" -le "$bestn" ]; then
        ok "the delayed compose draws the history scene's block: ${m} px <= stock ${bestn} px (reach ${reach} px, ${views} view-installed resolves)"
    else
        bad "the delayed compose differs from VBL $best by ${m} px, stock ${bestn} px: the history scene bound another scene's material block"
    fi
}

# ── the gallery, row by row, on the running game ──────────────────────
# Each row of mods/channel-gallery lit alone, at a pin where its geometry
# is on screen, against a baseline with the gallery loaded and no row lit.
# The row's label and tooltip on that game (k_text in channel_gallery.c)
# are the claim: a row the game's table labels a known gap must paint
# nothing (min 0), every other row must paint at least min px. A gap row
# sits at a pin where its channel draws (the per-channel rungs above prove
# the particle, shadow and portal channels at the same pins), so its 0 px
# is the key missing the draws, not an empty screen.
#
# Controls: at every pin a no-mod capture must equal the baseline (the
# gallery with no row lit draws nothing), and the baseline leg logs the
# class picker's list (gallery.census_log), which must hold the named
# class the class row lights and not the absent class, whose row paints
# nothing.
#
# Table rows: row|scene|pin|min px (0 = known gap)|config line. Census:
# scene|pin|listed class|absent class. Scene "attract" is the plain boot.
gallery_leg() {  # gallery_leg <tag> <scene> <pin> <rows|-> <config line>
    local tag="$1" scene="$2" pin="$3" rows="$4" cfg="$5"
    local md="$WORK/gal-mods-$tag"
    local -a movie=() extra=()
    if [ "$scene" != attract ]; then
        movie=(--bk2 "$PG_BK2_DIR/$scene.bk2")
        if [ -f "$PG_BK2_DIR/$scene.bk2.args" ]; then
            read -ra extra < <(grep -v '^#' "$PG_BK2_DIR/$scene.bk2.args" | tr '\n' ' ')
            movie+=("${extra[@]}")
        fi
    fi
    rm -rf "${md:?}"
    mkdir -p "$md"
    : > "$md/enabled.txt"
    if [ "$rows" != - ]; then
        cp -r mods/channel-gallery "$md/"
        rm -rf "${md:?}/channel-gallery/.build"
        printf 'gallery.rows = "%s"\ngallery.strength = 1.0\n%s\n' "$rows" "$cfg" \
            > "$md/channel-gallery/config.toml"
        echo channel-gallery > "$md/enabled.txt"
    fi
    local PG_CAPTURE_TIMEOUT=900
    pg_capture "$WORK/gal-$tag.log" "$WORK/gal-$tag-$pin-vk.png" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --no-card \
        "${movie[@]}" --screenshot-native-only --mods-dir "$md" \
        --screenshot-on-frame "$pin" \
        --screenshot-output "$WORK/gal-$tag-{n}.png" --frame-limit $((pin + 2))
}
gallery_rows_rung() {  # gallery_rows_rung <table> <census>
    local table="$1" census="$2" row scene pin minpx cfg key n
    say "channel-gallery row by row on $PG_GAME"
    local -A based=()
    while IFS='|' read -r row scene pin minpx cfg; do
        [ -z "${row// }" ] && continue
        key="$scene-$pin"
        if [ -z "${based[$key]:-}" ]; then
            based[$key]=1
            gallery_leg "base-$key" "$scene" "$pin" "" "gallery.census_log = 1" &&
            gallery_leg "nomod-$key" "$scene" "$pin" - "" ||
                { bad "no baseline or no-mod capture at $scene $pin"; based[$key]=bad; continue; }
            n=$(diffpx "$WORK/gal-base-$key-$pin-vk.png" "$WORK/gal-nomod-$key-$pin-vk.png")
            if [ "${n:-1}" -eq 0 ]; then ok "control: the gallery with no row lit equals no mod at $scene $pin"
            else bad "control: the unlit gallery differs from no mod by ${n:-?} px at $scene $pin"; based[$key]=bad; fi
        fi
        [ "${based[$key]}" = bad ] && { bad "row $row: no valid baseline at $scene $pin"; continue; }
        if ! gallery_leg "r$row-$key" "$scene" "$pin" "$row" "$cfg" ||
           ! grep -q "selector rows armed, 1 lit" "$WORK/gal-r$row-$key.log"; then
            bad "row $row: no capture, or the row did not light (see $WORK/gal-r$row-$key.log)"; continue
        fi
        n=$(diffpx "$WORK/gal-base-$key-$pin-vk.png" "$WORK/gal-r$row-$key-$pin-vk.png")
        if [ "$minpx" -eq 0 ]; then
            if [ "${n:-1}" -eq 0 ]; then ok "row $row paints nothing at $scene $pin${cfg:+ with $cfg}, as its label or the class census claims"
            else bad "row $row painted $n px at $scene $pin but is labelled a known gap on $PG_GAME: relabel it in k_text"; fi
        elif [ "${n:-0}" -ge "$minpx" ]; then
            ok "row $row repainted $n px at $scene $pin (>= $minpx)"
        else
            bad "row $row repainted ${n:-0} px at $scene $pin, expected >= $minpx"
        fi
    done <<< "$table"

    local listed absent line count
    IFS='|' read -r scene pin listed absent <<< "$census"
    line=$(grep -o 'class census at tick [0-9]*: [0-9]* classes:.*' \
           "$WORK/gal-base-$scene-$pin.log" 2>/dev/null | tail -1)
    count=$(sed -n 's/.*: \([0-9]*\) classes:.*/\1/p' <<< "$line")
    if [ "${count:-0}" -gt 0 ] && grep -qw "$listed" <<< "${line#*classes:}" &&
       ! grep -qw "$absent" <<< "${line#*classes:}"; then
        ok "class picker at $scene $pin lists $count classes, $listed among them, $absent not"
    else
        bad "class picker at $scene $pin: '${line:-no census line}' (want > 0 classes, $listed listed, $absent not)"
    fi
}

# ── material composition: moby chains of pinned-13 materials ──────────
# The mat-chain-* fixtures are channel-wide moby declaratives. a, b and c
# (tint, scale, lift) and the five rows of mat-chain-cap are composable
# (api 13); mat-chain-old12 is pinned at 12, so terminal; mat-chain-unlinkable
# declares a buffer of its own, so it is unlinkable. Each mat-chain-ref*
# mod is one exclusive material whose shader is the members' functions
# written by hand in chain order, so a composed capture must equal its
# reference pixel for pixel: a,b = ref2; a,b,c = ref3; mat-chain-cap = the
# four highest rows (refcap); a,unlinkable,b = ref2 (the unlinkable member
# below b is skipped); a,b,unlinkable = unlinkable alone (on top it draws
# alone); a,old12 = old12 alone (a pinned-12 material on top is terminal).
# Engagement: every reference repaints the stock frame, a,b differs from
# a alone, the census counts chain draws of 2, 3 and 4 members, and the
# a,b leg counts members excluded at extraction as absent from the
# installed set: the first moby scene after entry is extracted before
# the compose installs the combined module, so that count is never 0.
# chain_leg <tag> <scene> <frame> <mod>...: one capture, the mods enabled
# in the listed order (lowest priority first).
chain_leg() {
    local tag="$1" scene="$2" frame="$3"; shift 3
    local md="$WORK/mods-$tag"
    local -a movie=() extra=()
    if [ "$scene" != attract ]; then
        movie=(--bk2 "$PG_BK2_DIR/$scene.bk2")
        if [ -f "$PG_BK2_DIR/$scene.bk2.args" ]; then
            read -ra extra < <(grep -v '^#' "$PG_BK2_DIR/$scene.bk2.args" | tr '\n' ' ')
            movie+=("${extra[@]}")
        fi
    fi
    rm -rf "${md:?}"
    mkdir -p "$md"
    : > "$md/enabled.txt"
    local m
    for m in "$@"; do
        cp -r "mods/$m" "$md/"
        rm -rf "${md:?}/$m/.build"
        echo "$m" >> "$md/enabled.txt"
    done
    local PG_CAPTURE_TIMEOUT=900
    local -a PG_CAPTURE_ENV=(OPENPETE_MATERIAL_STATS=1)
    pg_capture "$WORK/$tag.log" "$WORK/$tag-$frame-vk.png" -- \
        --renderer vk --mute 1 --aspect 4:3 --lod original --no-card \
        "${movie[@]}" --screenshot-native-only --mods-dir "$md" \
        --screenshot-on-frame "$frame" \
        --screenshot-output "$WORK/$tag-{n}.png" --frame-limit $((frame + 2))
}
# chain_census <tag> <field>: a value of the census chain line.
chain_census() {
    sed -n "s/.*\[material-census\] chains: .*$2=\([0-9]*\).*/\1/p" "$WORK/$1.log" | tail -1
}
chain_rung() {  # chain_rung <scene> <frame>
    local scene="$1" frame="$2" leg spec tag
    say "material composition: moby chains at $scene $frame"
    for spec in "stock|" "a|mat-chain-a" "ab|mat-chain-a mat-chain-b" "ref2|mat-chain-ref2" \
                "abc|mat-chain-a mat-chain-b mat-chain-c" "ref3|mat-chain-ref3" \
                "aub|mat-chain-a mat-chain-unlinkable mat-chain-b" \
                "abu|mat-chain-a mat-chain-b mat-chain-unlinkable" "unl|mat-chain-unlinkable" \
                "cap|mat-chain-cap" "refcap|mat-chain-refcap" \
                "aold|mat-chain-a mat-chain-old12" "old|mat-chain-old12"; do
        tag="chain-${spec%%|*}"
        read -ra leg <<< "${spec#*|}"
        chain_leg "$tag" "$scene" "$frame" "${leg[@]}" ||
            { bad "chains: no capture for $tag (see $WORK/$tag.log)"; return; }
    done
    local f="-$frame-vk.png" n pair want
    for pair in "a stock" "ref2 stock" "ref3 stock" "refcap stock" "unl stock" "old stock" "ab a"; do
        set -- $pair
        n=$(diffpx "$WORK/chain-$1$f" "$WORK/chain-$2$f")
        if [ "${n:-0}" -gt 0 ]; then ok "chains: $1 repaints $n px against $2"
        else bad "chains: $1 equals $2 - the comparison below proves nothing"; fi
    done
    for pair in "ab ref2" "abc ref3" "cap refcap" "aub ref2" "abu unl" "aold old"; do
        set -- $pair
        n=$(diffpx "$WORK/chain-$1$f" "$WORK/chain-$2$f")
        if [ "${n:-1}" -eq 0 ]; then ok "chains: $1 is pixel-identical to $2"
        else bad "chains: $1 differs from $2 by ${n:-?} px"; fi
    done
    for want in "ab 2" "abc 3" "cap 4" "aub 2"; do
        set -- $want
        n=$(chain_census "chain-$1" "$2")
        if [ "${n:-0}" -gt 0 ]; then ok "chains: $1 drew $n chain(s) of $2 members"
        else bad "chains: $1 drew no chain of $2 members (see $WORK/chain-$1.log)"; fi
    done
    n=$(chain_census chain-ab absent-excluded)
    if [ "${n:-0}" -gt 0 ]; then ok "chains: $n member(s) excluded at extraction as absent from the installed set"
    else bad "chains: no member excluded as absent before the combined module installed"; fi
    n=$(chain_census chain-aold walks)
    if [ "${n:-1}" -eq 0 ]; then ok "chains: a pinned-12 material on top formed no chain"
    else bad "chains: a pinned-12 material on top formed $n chain(s)"; fi
}

# ── Spyro 2 and 3: per-channel engagement ─────────────────────────────
# The gallery mod and the frame pins of the rungs after this block are
# Spyro 1's. On Spyro 2 and 3 each render channel is proven live by one
# declarative [[material]] row on that channel (a full-strength magenta
# tint; on the shadow channel the decal-contract shader, since shadow draws
# resolve only decal materials) against a baseline whose row is disabled,
# at a pin from the game's own subtick corpus where the channel's geometry
# is on screen. The "ctl" leg repeats the baseline: a nonzero count there
# means the capture does not repeat and every other count is void.
if [ "$PG_GAME" != "spyro-1" ]; then
    case "$PG_GAME" in
        spyro-2) S23_RUNGS="
moby|shady-oasis-flame|1421
terrain|shady-oasis-flame|1421
player|shady-oasis-flame|1421
flame|shady-oasis-flame|1421
particle|shady-oasis-flame|1421
shadow|shady-oasis-flame|1421
ctl|shady-oasis-flame|1421
sky|autumn-plains-skelos-badlands-portal-entry|1425
portal|autumn-plains-skelos-badlands-portal-entry|1669
tracer|attract|14575" ;;
        spyro-3) S23_RUNGS="
moby|crystal-islands-flame|1653
terrain|crystal-islands-flame|1653
player|crystal-islands-flame|1653
flame|crystal-islands-flame|1653
particle|crystal-islands-flame|1653
sky|crystal-islands-flame|1653
ctl|crystal-islands-flame|1653
shadow|sheilas-alp-sheila-water-invisible|5105
portal|sunrise-spring-sunny-villa-portal-entry|3103
tracer|attract|5074" ;;
        *) echo "[modchan] SKIP: no channel pins for $PG_GAME"; exit 64 ;;
    esac
    # s23_leg <tag> <scene> <frame> <channel> <enabled>: one capture with a
    # one-row chan-probe mod; scene "attract" is the plain boot.
    s23_leg() {
        local tag="$1" scene="$2" frame="$3" ch="$4" en="$5"
        local md="$WORK/mods-$tag" frag=tint.frag
        local -a movie=() extra=()
        if [ "$scene" != attract ]; then
            movie=(--bk2 "$PG_BK2_DIR/$scene.bk2")
            read -ra extra < <(grep -v '^#' "$PG_BK2_DIR/$scene.bk2.args" | tr '\n' ' ')
            movie+=("${extra[@]}")
        fi
        rm -rf "${md:?}"
        mkdir -p "$md/chan-probe/shaders"
        cp mods/declarative-reshade/shaders/tint.frag "$md/chan-probe/shaders/"
        if [ "$ch" = shadow ]; then
            cp mods/channel-gallery/shaders/highlight_decal.frag "$md/chan-probe/shaders/"
            frag=highlight_decal.frag
        fi
        printf '[mod]\nid = "chan-probe"\nversion = "0.0.1"\napi = 13\ngame = ["%s"]\n\n[[material]]\nfrag = "shaders/%s"\nchannel = "%s"\nparams = [1.0, 0.0, 1.0, 1.0]\nenabled = %s\n' \
            "$PG_GAME" "$frag" "$ch" "$en" > "$md/chan-probe/mod.toml"
        echo chan-probe > "$md/enabled.txt"
        local PG_CAPTURE_TIMEOUT=900
        pg_capture "$WORK/$tag.log" "$WORK/$tag-$frame-vk.png" -- \
            --renderer vk --mute 1 --aspect 4:3 --lod original --no-card \
            "${movie[@]}" --screenshot-native-only --mods-dir "$md" \
            --screenshot-on-frame "$frame" \
            --screenshot-output "$WORK/$tag-{n}.png" --frame-limit $((frame + 2))
    }
    s23_diffpx() {
        python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
print(sum(1 for p in ImageChops.difference(a, b).getdata() if p != (0, 0, 0)))
PY
    }
    while IFS='|' read -r ch scene frame; do
        [ -z "${ch// }" ] && continue
        base="$WORK/base-$scene-$frame-vk.png"
        if [ ! -s "$base" ] && ! s23_leg "base-$scene" "$scene" "$frame" moby false; then
            bad "no baseline capture for $scene at $frame (see $WORK/base-$scene.log)"; continue
        fi
        tag="$ch-$scene"
        if [ "$ch" = ctl ]; then
            s23_leg "$tag" "$scene" "$frame" moby false
        else
            s23_leg "$tag" "$scene" "$frame" "$ch" true
        fi || { bad "no capture for $ch at $scene $frame (see $WORK/$tag.log)"; continue; }
        n=$(s23_diffpx "$base" "$WORK/$tag-$frame-vk.png")
        if [ "$ch" = ctl ]; then
            if [ "${n:-1}" -eq 0 ]; then ok "control: the baseline repeats exactly at $scene $frame"
            else bad "control: the baseline differs by ${n:-?} px at $scene $frame; the counts are void"; fi
        elif ! grep -q "chan-probe: 1 declarative material row" "$WORK/$tag.log"; then
            bad "$ch: the row was not kept (see $WORK/$tag.log)"
        elif [ "${n:-0}" -gt 0 ]; then
            ok "$ch: $n px repainted at $scene $frame"
        else
            bad "$ch: 0 px repainted at $scene $frame"
        fi
    done <<< "$S23_RUNGS"
    diffpx() { s23_diffpx "$@"; }
    delay_scene=$(sed -n 's/^moby|\([^|]*\)|\([0-9]*\)$/\1/p' <<< "$S23_RUNGS")
    delay_frame=$(sed -n 's/^moby|\([^|]*\)|\([0-9]*\)$/\2/p' <<< "$S23_RUNGS")
    delay_rung "$delay_scene" "$delay_frame"
    chain_rung "$delay_scene" "$delay_frame"
    case "$PG_GAME" in
        spyro-2) GALLERY_TABLE="
0|shady-oasis-flame|1421|100000|
1|shady-oasis-flame|1421|100000|
2|shady-oasis-flame|1421|10000|
3|shady-oasis-flame|1421|3000|gallery.class = 583
3|shady-oasis-flame|1421|0|gallery.class = 0
4|shady-oasis-flame|1421|2000|
5|autumn-plains-skelos-badlands-portal-entry|1423|1000|
6|shady-oasis-flame|1421|1000|
7|shady-oasis-flame|1421|300|
8|shady-oasis-flame|1421|400|
9|shady-oasis-flame|1421|0|
10|shady-oasis-flame|1421|0|
11|glimmer-pogo-dialogue|1435|700|
12|shady-oasis-flame|1897|150|
13|shady-oasis-flame|1421|0|
14|attract|14575|150|
15|autumn-plains-skelos-badlands-portal-entry|1669|100000|
16|autumn-plains-skelos-badlands-portal-entry|1669|80000|
17|autumn-plains-skelos-badlands-portal-entry|1669|25000|
18|autumn-plains-skelos-badlands-portal-entry|1669|0|
19|shady-oasis-flame|1421|3000|gallery.class = 583
20|autumn-plains-skelos-badlands-portal-entry|1423|15000|"
            GALLERY_CENSUS="shady-oasis-flame|1421|583|0" ;;
        spyro-3) GALLERY_TABLE="
0|crystal-islands-flame|1653|100000|
1|crystal-islands-flame|1653|100000|
2|crystal-islands-flame|1653|4000|
3|crystal-islands-flame|1653|1300|gallery.class = 743
3|crystal-islands-flame|1653|0|gallery.class = 0
4|crystal-islands-flame|1653|2500|
5|crystal-islands-flame|1653|25000|
6|crystal-islands-flame|1653|1500|
7|crystal-islands-flame|1653|300|
8|sheilas-alp-sheila-water-invisible|5105|500|
9|sheilas-alp-sheila-water-invisible|5105|0|
10|crystal-islands-flame|1653|0|
11|sunrise-spring-camera-swing|1807|600|
12|sunrise-spring-camera-swing|1807|30|
13|crystal-islands-flame|1653|0|
14|attract|5074|1500|
15|sunrise-spring-sunny-villa-portal-entry|3103|75000|
16|sunrise-spring-sunny-villa-portal-entry|3103|70000|
17|sunrise-spring-sunny-villa-portal-entry|3103|5000|
18|sunrise-spring-sunny-villa-portal-entry|3103|0|
19|crystal-islands-flame|1653|1300|gallery.class = 743
20|crystal-islands-flame|1653|20000|"
            GALLERY_CENSUS="crystal-islands-flame|1653|743|0" ;;
    esac
    gallery_rows_rung "$GALLERY_TABLE" "$GALLERY_CENSUS"
    echo
    if [ "$fail" -eq 0 ]; then echo "[modchan] ALL GREEN"; else echo "[modchan] FAILURES - see above"; fi
    exit $fail
fi

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

# ── shading contracts ─────────────────────────────────────────────────
# A material's fragment is written against one shading contract: the scene
# contract (openpete_psx_material.glsl) or the decal profile
# (openpete_psx_decal.glsl). The two differ in descriptor sets, push block
# and fragment inputs, so a fragment built into the other contract's
# pipeline is an invalid pipeline. Row 0 is a scene-contract material with
# no key, so its selector also matches the moby-shadow tag, which draws on
# the decal pipe; the resolve must skip it there and leave the shadow stock.
# Row 8 is the decal-contract shadow material: lit together with row 0 it
# must still win the shadow tag, so rows 0,8 repaint the shadow (96-128 px
# at this pin) that rows 0 alone leaves stock.
say "a material resolves only on a draw path of its own shading contract"
cbase="$WORK/base_artisans-bridge_1986-1986-vk.png"
if [ ! -s "$cbase" ] && ! run_rows "base_artisans-bridge_1986" artisans-bridge 1986 "" ""; then
    bad "no baseline for artisans-bridge at frame 1986"
elif ! run_rows "contract0" artisans-bridge 1986 "0" ""; then
    bad "rows 0 produced no capture (see $WORK/contract0.log): a scene-contract material was built into a decal pipeline"
else
    n0=$(diffpx "$cbase" "$WORK/contract0-1986-vk.png")
    if [ "${n0:-0}" -gt 0 ]; then
        ok "rows 0 captured and repainted $n0 px"
    else
        bad "rows 0 repainted 0 px, so the contract rungs prove nothing"
    fi
    if ! run_rows "contract08" artisans-bridge 1986 "0,8" ""; then
        bad "rows 0,8 produced no capture (see $WORK/contract08.log)"
    else
        n8=$(diffpx "$WORK/contract0-1986-vk.png" "$WORK/contract08-1986-vk.png")
        if [ "${n8:-0}" -ge 60 ]; then
            ok "rows 0,8 differ from rows 0 by $n8 px (>= 60): the decal-contract row wins the shadow tag"
        else
            bad "rows 0,8 differ from rows 0 by only ${n8:-0} px, expected >= 60: the scene-contract row still claims the shadow tag"
        fi
    fi
fi

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

delay_rung flame 1800
chain_rung flame 1800

# Row 12 keys the flare kind, which Spyro 1 never draws: its sparkle
# renderer emits cross lines (the sparkle-line kind) and no four-point star,
# so the row is a known gap pinned where the renderer builds six crosses.
gallery_rows_rung "
0|flame|1800|100000|
1|flame|1800|100000|
2|flame|1800|800|
3|flame|1800|300|gallery.class = 348
3|flame|1800|0|gallery.class = 1
4|flame|1800|2500|
5|cyclorama-portal|2000|3000|
6|flame|1800|10000|
7|flame|1790|300|
8|artisans-bridge|1986|60|
9|artisans-bridge|1986|0|
10|flame|1800|50|
11|stone-hill-gem-spyro-occlusion|1900|3000|
12|key|2441|0|
13|stone-hill-lindar-rescue|2120|1500|
14|gnasty_gnorc_attack|1800|500|
15|cyclorama-portal|2000|10000|
16|cyclorama-portal|2000|10000|
17|beast-makers-distant-portal|2000|250|
18|beast-makers-distant-portal|2000|40|
19|flame|1800|300|gallery.class = 348
20|flame|1800|350|" "flame|1800|348|1"

echo
if [ "$fail" -eq 0 ]; then
    echo "[modchan] ALL GREEN"
else
    echo "[modchan] FAILURES - see above"
fi
exit $fail
