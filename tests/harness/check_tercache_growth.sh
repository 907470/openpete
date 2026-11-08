#!/usr/bin/env bash
# check_tercache_growth.sh - the terrain cache's arena growth, consolidation,
# failed-edge and previous-tick store legs, headless on llvmpipe.
#
# Every run forces the cache on (OPENPETE_TERRAIN_CACHE_FORCE=1: software
# Vulkan does not select it alone) and reads the `tercache:` exit row, which
# the renderer prints at shutdown. A leg whose row reports bakes=0 compared
# nothing about the cache and is VACUOUS, never PASS.
#
# Legs (--leg selects one; the default set is level-edges, squeeze-bk2,
# consolidate, store, failed-edge, steady-pose, custom-level):
#   level-edges   the attract demo (boot to title, --frame-limit
#                 DEMO_LIMIT) at the content-estimate sizing: claims_sum=0,
#                 failed_edges=0, and svc_fulfilled equal to the level
#                 latches (`[tc-est] latch` lines: every level edge plus the
#                 session's first latch, each of which posts a replacement).
#   squeeze-demo  the attract demo (boot to title, --frame-limit
#                 DEMO_LIMIT) with OPENPETE_TC_INITIAL_CAP squeezed below the
#                 levels' demand, so every arena must grow: claims_sum=0,
#                 grow>0, grow_claims>0, consolidations=0, dropped_spans=0,
#                 draws_retired=0.
#   squeeze-bk2   the same criteria on one corpus bk2 that crosses a level
#                 edge (Spyro 1: dark-passage-level-transition).
#   consolidate   OPENPETE_TC_BUF_MAX=2 with the squeeze, so the buffer cap
#                 is reached and consolidation runs: consolidations>0,
#                 hold_drops no higher than the stock run's, claims_sum=0;
#                 migrate_ticks is reported.
#   store         OPENPETE_TERRAIN_ANIMNEAR=1 and OPENPETE_TERRAIN_CANIMNEAR=1
#                 with OPENPETE_TC_PREV_CAP small, so the previous-tick store
#                 must grow: prev_grow>0, prev_fail=0.
#   failed-edge   OPENPETE_TC_REFUSE=edge:* on a run that crosses a level edge
#                 with a held scene: failed_edges>0, consolidations>0,
#                 hold_takes>0, sealed_allocs=0, hold_drops=0, claims_sum=0;
#                 sealed_draws is reported (it needs a hold valid across
#                 the refused edge).
#   refuse-grow   OPENPETE_TC_REFUSE=grow:*,cons:* with the squeeze: every
#                 growth and consolidation refused. Reports the claims the
#                 refusal policy leaves (the exhaustion case); no verdict.
#   steady-pose   Spyro 2 skelos_badlands at a freecam pose under
#                 --texture-mapping affine and --lod extended, where the
#                 poly-anim cell-record arena (CA_OPAQUE) re-bakes every
#                 build and outgrows its content estimate, run
#                 STEADY_LIMIT VBLs (the level loads near VBL 1290 and
#                 settles within 300 builds): claims_sum=0,
#                 consolidations=0, exhaust_warnings=0, and at most
#                 STEADY_REQ growth requests per arena after the level
#                 load (7: a growth series that fits the 8 slots; an arena
#                 that never settles requests without bound).
#   custom-level  the M4 fixture, built at run time from the disc: a scratch
#                 mod whose WAD is lofty_castle's (level 52) served with
#                 `replace = true` over the donor toasty (sunny_flight, the
#                 planned donor, hangs at load), at the freecam topdown pose.
#                 The arenas are sized from the level's own sector
#                 headers: claims_sum=0, window_bakes=0, exhaust_warnings=0.
#
# --exe PATH drives another binary with the same legs (a build without
# growth reports claims>0 on the squeeze legs, which is the separation).
#
# Usage:
#   tests/harness/check_tercache_growth.sh [--game SLUG] [--leg NAME]
#        [--exe PATH] [--out DIR] [--squeeze N] [--demo-limit N]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no binary, no disc, or no leg for
# the game).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

export OPENPETE_VIBRATION=0

LEG=""; EXE=""; OUT=""; SQUEEZE=32768; DEMO_LIMIT=40000; STEADY_LIMIT=2400; STEADY_REQ=7
prev=""
for a in "$@"; do
    case "$prev" in
        --leg) LEG="$a";; --exe) EXE="$a";; --out) OUT="$a";;
        --squeeze) SQUEEZE="$a";; --demo-limit) DEMO_LIMIT="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
[[ -n "$EXE" ]] || EXE="$PG_BIN"
[[ -x "$EXE" ]] || { echo "tercache-growth: SKIP (no binary at $EXE)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "tercache-growth: SKIP (no disc at $PG_DISC)"; exit 64; }
[[ -n "$OUT" ]] || OUT="${TMPDIR:-/tmp}/tercache-growth/$GAME"
mkdir -p "$OUT"

# Per-game rows: the level a spawn leg loads, and the bk2 the squeeze-bk2
# leg replays.
case "$GAME" in
    spyro-1) SPAWN=artisans; BK2=dark-passage-level-transition; STORE_AREA=artisans;;
    spyro-2) SPAWN=summer_forest; BK2=""; STORE_AREA=idol_springs;;
    spyro-3) SPAWN=sunrise_spring; BK2=""; STORE_AREA=seashell_shore;;
esac

# One run: deletes its log first and asserts the exit row after it.
run_leg() {  # tag frame-limit envs... -- args...
    local tag="$1" limit="$2"; shift 2
    local -a envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    local log="$OUT/$tag.log"
    rm -f "$log"
    env OPENPETE_VIBRATION=0 OPENPETE_HEADLESS_NATIVE=1 OPENPETE_TERRAIN_CACHE_FORCE=1 \
        "${envs[@]}" timeout 3600 "$EXE" "${PG_RUN_ARGS[@]}" --headless --no-config \
        --no-card --mute 1 --vibration 0 --renderer vk --disc-cache 0 --disc "$PG_DISC" \
        --frame-limit "$limit" "$@" < /dev/null > "$log" 2>&1
    local rc=$?
    [[ -s "$log" ]] || { echo "  leg $tag produced no log"; return 1; }
    return $rc
}

# A field of the leg's `tercache:` row (the last one), or empty.
field() {  # tag name
    grep -o '^tercache: .*' "$OUT/$1.log" 2>/dev/null | tail -1 |
        tr ' ' '\n' | sed -n "s/^$2=//p"
}

fail=0
ran=0
want() { [[ "$LEG" == "$1" || ( -z "$LEG" && " level-edges squeeze-bk2 consolidate store failed-edge steady-pose custom-level " == *" $1 "* ) ]]; }
# check tag expr...: each expr is name<op>value over the row's fields.
check() {
    local tag="$1"; shift
    local bakes; bakes="$(field "$tag" bakes)"
    echo "[tercache-growth] leg=$tag $(grep -o '^tercache: .*' "$OUT/$tag.log" | tail -1 | cut -c11-)"
    if [[ -z "$bakes" || "$bakes" == 0 ]]; then
        echo "[tercache-growth] leg=$tag VACUOUS (no tercache: row or bakes=0)"; fail=1; return
    fi
    local e v want_v op ok
    for e in "$@"; do
        if [[ "$e" =~ ^([a-z_]+)(=|\>|\<)([0-9]+)$ ]]; then
            v="$(field "$tag" "${BASH_REMATCH[1]}")"; op="${BASH_REMATCH[2]}"; want_v="${BASH_REMATCH[3]}"
            ok=0
            case "$op" in
                =) [[ "${v:-x}" == "$want_v" ]] && ok=1;;
                \>) [[ -n "$v" && "$v" -gt "$want_v" ]] && ok=1;;
                \<) [[ -n "$v" && "$v" -lt "$want_v" ]] && ok=1;;
            esac
            [[ "$ok" == 1 ]] || { echo "[tercache-growth] leg=$tag FAIL ($e, read ${v:-none})"; fail=1; }
        fi
    done
}

SQ=(OPENPETE_TC_INITIAL_CAP="$SQUEEZE")
GROW_PASS=(claims_sum=0 grow\>0 grow_claims\>0 consolidations=0 dropped_spans=0 draws_retired=0)

if want level-edges; then
    ran=1
    run_leg level-edges "$DEMO_LIMIT" --
    latches="$(grep -c '^\[tc-est\] latch level=' "$OUT/level-edges.log")"
    echo "[tercache-growth] leg=level-edges latches=$latches (level edges + 1)"
    check level-edges claims_sum=0 failed_edges=0 "svc_fulfilled=$latches"
    [[ "$latches" -gt 1 ]] ||
        { echo "[tercache-growth] leg=level-edges VACUOUS (no level edge crossed)"; fail=1; }
fi

if want squeeze-demo; then
    ran=1
    run_leg squeeze-demo "$DEMO_LIMIT" "${SQ[@]}" --
    check squeeze-demo "${GROW_PASS[@]}"
fi

if want squeeze-bk2; then
    if [[ -z "$BK2" ]]; then
        echo "[tercache-growth] leg=squeeze-bk2 SKIP (no bk2 row for $GAME)"
    else
        ran=1
        mapfile -t BK2_ARGS < <(grep -v '^#' "games/$GAME/bk2/$BK2.bk2.args" | tr ' ' '\n' | sed '/^$/d')
        run_leg squeeze-bk2 6000 "${SQ[@]}" -- --bk2 "games/$GAME/bk2/$BK2.bk2" --bk2-stop-on-end "${BK2_ARGS[@]}"
        check squeeze-bk2 "${GROW_PASS[@]}"
    fi
fi

if want consolidate; then
    ran=1
    run_leg consolidate 1900 OPENPETE_TC_BUF_MAX=2 "${SQ[@]}" -- --skip-to-level "$SPAWN"
    check consolidate consolidations\>0 hold_drops\<2 claims_sum=0 dropped_spans=0
fi

if want store; then
    ran=1
    run_leg store 1900 OPENPETE_TERRAIN_ANIMNEAR=1 OPENPETE_TERRAIN_CANIMNEAR=1 \
        OPENPETE_TC_PREV_CAP=256 -- --skip-to-level "$STORE_AREA" --lod extended
    check store prev_grow\>0 prev_fail=0
fi

if want failed-edge; then
    ran=1
    run_leg failed-edge 1900 OPENPETE_TC_REFUSE='edge:*' -- --skip-to-level "$SPAWN"
    check failed-edge failed_edges\>0 consolidations\>0 hold_takes\>0 sealed_allocs=0 \
        hold_drops=0 claims_sum=0
fi

if want steady-pose; then
    if [[ "$GAME" != spyro-2 ]]; then
        echo "[tercache-growth] leg=steady-pose SKIP (the pose is a Spyro 2 row)"
    else
        ran=1
        run_leg steady-pose "$STEADY_LIMIT" -- --skip-to-level skelos_badlands \
            --freecam-pose "pos=(116034, 2704, 95746) yaw=1112 pitch=541" \
            --texture-mapping affine --lod extended
        check steady-pose claims_sum=0 consolidations=0 exhaust_warnings=0
        # Growth and consolidation requests per arena after the level load.
        req="$(sed -n '/loader took level=/,$p' "$OUT/steady-pose.log" |
            sed -n 's/^\[ter-gpu\] arena \([0-9]*\) \(grow\|consolidation\) request.*/\1/p' |
            sort | uniq -c | sort -rn | head -1)"
        read -r n arena <<< "${req:-0 -}"
        echo "[tercache-growth] leg=steady-pose most requests after the level load: $n (arena $arena)"
        if ! grep -q 'loader took level=' "$OUT/steady-pose.log"; then
            echo "[tercache-growth] leg=steady-pose VACUOUS (the level never loaded)"; fail=1
        elif [[ "$n" -gt "$STEADY_REQ" ]]; then
            echo "[tercache-growth] leg=steady-pose FAIL (arena $arena: $n requests > $STEADY_REQ)"; fail=1
        fi
    fi
fi

if want refuse-grow; then
    ran=1
    run_leg refuse-grow 1900 "${SQ[@]}" OPENPETE_TC_REFUSE='grow:*,cons:*' -- --skip-to-level "$SPAWN"
    check refuse-grow
fi

# The M4 fixture: lofty_castle's WAD (level 52) as a replace-mode custom
# level over the donor toasty, at the freecam topdown pose.
custom_level() {  # tag envs... -- checks...
    local tag="$1"; shift
    local -a envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    if [[ "$GAME" != spyro-1 ]]; then
        echo "[tercache-growth] leg=$tag SKIP (custom levels run on Spyro 1 only)"; return
    fi
    ran=1
    MODS="$OUT/m4mods"
    rm -rf "${MODS:?}"
    mkdir -p "$MODS/m4big/assets/levels"
    python3 tools/extract_wad_entry.py --disc "$PG_DISC" --level 52 \
        --out "$MODS/m4big/assets/levels/big.wad" > /dev/null ||
        { echo "[tercache-growth] leg=$tag FAIL (WAD extraction)"; fail=1; }
    printf '[mod]\nid      = "m4big"\nname    = "M4 big"\nversion = "1.0.0"\ngame    = "spyro-1"\napi     = 1\n\n[[level]]\nname  = "M4 BIG"\nfiles = "big"\ndonor = "toasty"\nreplace = true\n' \
        > "$MODS/m4big/mod.toml"
    run_leg "$tag" 2400 "${envs[@]}" -- --mods-dir "$MODS" --skip-to-level toasty \
        --lod extended --freecam-pose topdown
    check "$tag" "$@"
}

want custom-level && custom_level custom-level -- \
    claims_sum=0 window_bakes=0 exhaust_warnings=0

[[ "$ran" == 1 ]] || { echo "tercache-growth: SKIP (no leg ran)"; exit 64; }
if [[ "$fail" == 0 ]]; then echo "tercache-growth: PASS"; exit 0; fi
echo "tercache-growth: FAIL"
exit 1
