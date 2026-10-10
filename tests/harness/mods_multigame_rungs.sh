# mods_multigame_rungs.sh - the rungs of tests/harness/check_mods.sh whose
# fixtures load on every game. SOURCED by it after mods_sdk_rungs.sh (helpers
# say, ok, bad, fixture, run_game, mod_toml; WORK, MODS_SRC, PG_GAME,
# TEST_SDK, API_VER); runs for every game.
#
# Rungs:
#   lib-and-client  the dependency is entered first despite the reversed
#                   enabled order; a missing dependency is an actionable
#                   error and the run survives; a dependency cycle disables
#                   both mods.
#   arena-probe     the guest arena is live and the host/guest round trip
#                   holds.
#   postfx          crt-beans, crt-filter and scene-sepia from copies with no
#                   .build/ (so no shader cache hit, which logs nothing):
#                   every one of their 5 shaders logs `compiled`, none logs
#                   `compile FAILED`, each mod logs armed; on a game whose
#                   descriptor serves no text rasterizer, scene-sepia's
#                   present-hook draw_text logs unsupported exactly once
#                   (the run renders one screenshot, so the hook runs).
#   compile-mods    --compile-mods against the test SDK writes a crt-beans
#                   object under .build/<game>/ for every staged game.
#   assets          a mod with assets/ and assets/spyro-2/ registers
#                   assets/spyro-2 on Spyro 2 and assets/ elsewhere.
#   materials       [[material]] rows by game key and the running game's
#                   material channel mask.
#   levels          (a game with no level ops) a [[level]] row is refused
#                   with one line naming the game; the mod still loads.
#   engagement      every in-tree mod with per-game arms (the camera-clock
#                   mods, double-gems, natural-gems) loads and its own
#                   override link counts calls above 0 on the attract demo
#                   (OPENPETE_MOD_LINK_CALLS); double-gems and natural-gems
#                   each double a pickup, and a later pickup in the same
#                   run starts from the doubled total, so the write landed
#                   in guest RAM.
#   arms copies     (Spyro 2 and 3; Spyro 1 runs its own in check_mods.sh)
#                   chain-duo nesting in both enabled orders, crash-dummy
#                   attribution, arena-canary armed with no DESYNC across a
#                   file save and load that restores its 64 B block.

# Copy an in-tree mod into <dest dir> without its .build/.
copy_mod() { # copy_mod <id> <dest dir>
    mkdir -p "$2"
    cp -r "$MODS_SRC/$1" "$2/$1"
    rm -rf "${2:?}/$1/.build"
}

# ── M1. lib-and-client: dependency order, missing dependency, cycle ──────
say "lib-and-client: client listed FIRST - topo-sort must load lib first"
fixture "$WORK/libclient" lib-and-client demo-lib
out="$(run_game "$WORK/libclient" 600)"
lib_line=$(printf '%s\n' "$out" | grep -n "demo-lib: entry ran" | head -1 | cut -d: -f1)
cli_line=$(printf '%s\n' "$out" | grep -n "lib-and-client: entry ran" | head -1 | cut -d: -f1)
if [ -n "$lib_line" ] && [ -n "$cli_line" ] && [ "$lib_line" -lt "$cli_line" ]; then
    ok "dependency entry ran first despite reversed enabled order"
else
    bad "dependency order wrong or entries missing (lib=$lib_line cli=$cli_line)"
fi

say "lib-and-client: missing dep - actionable error, run survives"
fixture "$WORK/nodep" lib-and-client
out="$(run_game "$WORK/nodep" 600)"
if printf '%s\n' "$out" | grep -q 'requires "demo-lib", which is not enabled'; then
    ok "missing-dep error is actionable"
else
    bad "missing-dep error absent/wrong"
fi
if printf '%s\n' "$out" | grep -q "openpete-exit: reason=frame-limit"; then
    ok "run survived the disabled mod"
else
    bad "run did not reach the frame limit"
fi

say "lib-and-client: dependency cycle - both disabled, run survives"
for m in cycle-x cycle-y; do
    other=cycle-y; [ "$m" = cycle-y ] && other=cycle-x
    mkdir -p "$WORK/cycsrc/$m/src"
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = '"$API_VER"'\ndeps = ["%s"]\n' \
        "$m" "$other" > "$WORK/cycsrc/$m/mod.toml"
    printf '#include <openpete_mod_api.h>\nint openpete_mod_entry(const openpete_mod_api_t* a, openpete_mod_t* s){(void)a;(void)s;return 0;}\n' \
        > "$WORK/cycsrc/$m/src/m.c"
done
mkdir -p "$WORK/cycle"
ln -sfn "$WORK/cycsrc/cycle-x" "$WORK/cycle/cycle-x"
ln -sfn "$WORK/cycsrc/cycle-y" "$WORK/cycle/cycle-y"
printf 'cycle-x\ncycle-y\n' > "$WORK/cycle/enabled.txt"
out="$(run_game "$WORK/cycle" 600)"
if printf '%s\n' "$out" | grep -q "dependency cycle through"; then
    ok "cycle detected and reported"
else
    bad "cycle error absent"
fi
if printf '%s\n' "$out" | grep -q "mods:.*mod.* active"; then
    bad "cycle mods loaded anyway"
else
    ok "neither cycle mod loaded"
fi

# ── M2. arena-probe: guest arena round trip ──────────────────────────────
say "arena-probe: guest arena live and round trip on $PG_GAME"
fixture "$WORK/arena" arena-probe
out="$(run_game "$WORK/arena" 600)"
if printf '%s\n' "$out" | grep -q "guest arena live @ 0x80700000" &&
   printf '%s\n' "$out" | grep -q "arena-probe: guest/host roundtrip OK"; then
    ok "arena live at 0x80700000, round trip OK"
else
    bad "arena-probe: arena or round-trip line missing"
    printf '%s\n' "$out" | grep -E 'arena' | head -5
fi

# ── M3. postfx mods: every shader compiles from an empty cache ───────────
say "postfx: crt-beans, crt-filter, scene-sepia shaders compile on $PG_GAME"
mkdir -p "$WORK/postfx"
: > "$WORK/postfx/enabled.txt"
for m in crt-beans crt-filter scene-sepia; do
    copy_mod "$m" "$WORK/postfx"
    echo "$m" >> "$WORK/postfx/enabled.txt"
done
# A headless run reaches the present hooks only on a rendered present, so
# the run takes one native screenshot.
out="$(RUN_OUTS="$WORK/postfx/shot-vk.png" run_game "$WORK/postfx" 320 \
        --renderer vk --screenshot-on-frame 300 \
        --screenshot-output "$WORK/postfx/shot.png")"
n_compiled="$(printf '%s\n' "$out" |
    grep -cE '(crt-beans|crt-filter|scene-sepia): shader shaders/[a-z_]+\.frag compiled')"
n_failed="$(printf '%s\n' "$out" | grep -c 'compile FAILED')"
n_armed=0
for pat in "crt-beans armed" "crt filter armed" "scene sepia armed"; do
    printf '%s\n' "$out" | grep -q "$pat" && n_armed=$((n_armed + 1))
done
if [ "$n_compiled" -eq 5 ] && [ "$n_failed" -eq 0 ] && [ "$n_armed" -eq 3 ]; then
    ok "5/5 shaders compiled, 0 failed, 3/3 mods armed"
else
    bad "shaders compiled $n_compiled/5, failed $n_failed, armed $n_armed/3"
    printf '%s\n' "$out" | grep -E 'shader|armed' | head -10
fi
n_text="$(printf '%s\n' "$out" |
    grep -c "scene-sepia: draw_text unsupported on $PG_GAME")"
if [ "$PG_GAME" = "spyro-1" ]; then
    if [ "$n_text" -eq 0 ]; then
        ok "draw_text served (no unsupported line)"
    else
        bad "draw_text logged unsupported on $PG_GAME"
    fi
elif [ "$n_text" -eq 1 ]; then
    ok "scene-sepia draw_text logged unsupported once"
else
    bad "scene-sepia draw_text unsupported lines: $n_text (want 1)"
fi

# ── M4. --compile-mods: one crt-beans object per staged game ─────────────
say "--compile-mods: crt-beans builds under .build/<game>/ for every staged game"
copy_mod crt-beans "$WORK/cmods"
out="$( PG_CAPTURE_ENV=(OPENPETE_MOD_SDK_DIR="$(realpath "$TEST_SDK")")
        run_game "$WORK/cmods" 1 --compile-mods )"
n_objs=0
for g in spyro-1 spyro-2 spyro-3; do
    [ -f "$WORK/cmods/crt-beans/.build/$g/crt-beans.so" ] && n_objs=$((n_objs + 1))
done
if [ "$n_objs" -eq 3 ] &&
   ! printf '%s\n' "$out" | grep -qE -- '--compile-mods \[[^]]*\]: .*[1-9][0-9]* failed'; then
    ok "3/3 crt-beans objects, no failure"
else
    bad "crt-beans objects $n_objs/3"
    printf '%s\n' "$out" | grep -E -- '--compile-mods' | head -6
fi

# ── M5. per-game asset root ───────────────────────────────────────────────
say "assets: assets/spyro-2/ registers on spyro-2, assets/ elsewhere"
d="$WORK/assetsrc/asset-variant"
mod_toml "$d" asset-variant 'game = ["spyro-1", "spyro-2", "spyro-3"]'
mkdir -p "$d/assets/spyro-2"
mkdir -p "$WORK/assets"
ln -sfn "$d" "$WORK/assets/asset-variant"
echo asset-variant > "$WORK/assets/enabled.txt"
out="$(run_game "$WORK/assets" 60)"
# The loader names the root through the fixture's link.
want="$WORK/assets/asset-variant/assets"
[ "$PG_GAME" = "spyro-2" ] && want="$want/spyro-2"
n_root="$(printf '%s\n' "$out" | grep -c "asset root registered: .*(asset-variant)")"
if [ "$n_root" -eq 1 ] &&
   printf '%s\n' "$out" | grep -qF "asset root registered: $want (asset-variant)"; then
    ok "one root registered: ${want#"$WORK"/assets/asset-variant/}"
else
    bad "asset roots $n_root (want 1 at ${want#"$WORK"/assets/asset-variant/})"
    printf '%s\n' "$out" | grep "asset root" | head -3
fi

# ── M6. [[material]] game key and channel mask ───────────────────────────
# Row 0 names spyro-1 on terrain, row 1 names spyro-2 and spyro-3 on moby,
# row 2 has no game key on channel "any". Spyro 1 serves every channel and
# keeps rows 0 and 2; Spyro 2 and 3 serve no material channel, so row 0
# leaves by its game key and rows 1 and 2 by the channel mask.
say "materials: [[material]] game key and channel mask on $PG_GAME"
d="$WORK/matsrc/mat-games"
mod_toml "$d" mat-games 'game = ["spyro-1", "spyro-2", "spyro-3"]'
mkdir -p "$d/shaders"
cp "$MODS_SRC/declarative-reshade/shaders/"*.frag "$d/shaders/"
frag="shaders/$(basename "$(ls "$d/shaders/"*.frag | head -1)")"
cat >> "$d/mod.toml" <<TEOF

[[material]]
game    = "spyro-1"
channel = "terrain"
frag    = "$frag"

[[material]]
game    = ["spyro-2", "spyro-3"]
channel = "moby"
frag    = "$frag"

[[material]]
channel = "any"
frag    = "$frag"
TEOF
mkdir -p "$WORK/mat"
ln -sfn "$d" "$WORK/mat/mat-games"
echo mat-games > "$WORK/mat/enabled.txt"
out="$(run_game "$WORK/mat" 60)"
n_game="$(printf '%s\n' "$out" | grep -c "\[\[material\]\] row [0-9] targets other games - skipped (this is $PG_GAME)")"
n_chan="$(printf '%s\n' "$out" | grep -c "\[\[material\]\] row [0-9]: channel -\?[0-9] unsupported on $PG_GAME - skipped")"
n_kept="$(printf '%s\n' "$out" | sed -n 's/.*mat-games: \([0-9]*\) declarative material row(s).*/\1/p' | head -1)"
if [ "$PG_GAME" = "spyro-1" ]; then
    want_game=1 want_chan=0 want_kept=2
else
    want_game=1 want_chan=2 want_kept=0
fi
if [ "$n_game" -eq "$want_game" ] && [ "$n_chan" -eq "$want_chan" ] &&
   [ "${n_kept:-0}" -eq "$want_kept" ]; then
    ok "rows kept ${n_kept:-0}, skipped by game $n_game, by channel $n_chan"
else
    bad "rows kept ${n_kept:-0}/$want_kept, by game $n_game/$want_game, by channel $n_chan/$want_chan"
    printf '%s\n' "$out" | grep -E 'material' | head -6
fi

# ── M7. [[level]] rows on a game with no level ops ───────────────────────
if [ "$PG_GAME" != "spyro-1" ]; then
    say "levels: [[level]] row refused on $PG_GAME, mod still loads"
    d="$WORK/lvlsrc/lvl-games"
    mod_toml "$d" lvl-games 'game = ["spyro-1", "spyro-2", "spyro-3"]'
    cat >> "$d/mod.toml" <<'TEOF'

[[level]]
name  = "probe"
files = "probe"
donor = "artisans"
TEOF
    mkdir -p "$WORK/lvl"
    ln -sfn "$d" "$WORK/lvl/lvl-games"
    echo lvl-games > "$WORK/lvl/enabled.txt"
    out="$(run_game "$WORK/lvl" 60)"
    n_ref="$(printf '%s\n' "$out" |
        grep -c "lvl-games: 1 \[\[level\]\] row(s) refused - $PG_GAME serves no level ops")"
    if [ "$n_ref" -eq 1 ] &&
       printf '%s\n' "$out" | grep -q "loaded lvl-games v0.0.1 (declarative)"; then
        ok "1 refusal line naming $PG_GAME; mod loaded"
    else
        bad "level refusal lines $n_ref (want 1) or the mod did not load"
        printf '%s\n' "$out" | grep -E 'lvl-games|level' | head -5
    fi
fi

# ── M8. per-game arms: engagement on the attract demo ────────────────────
# DEMO_FRAMES covers each game's first two consecutive CollectItem-class
# pickups on the attract demo: Spyro 1 near VBL 4100, Spyro 2 near VBL
# 14300, Spyro 3 near VBL 3000.
case "$PG_GAME" in
    spyro-1) DEMO_FRAMES=6000 ;;
    spyro-2) DEMO_FRAMES=15500 ;;
    *)       DEMO_FRAMES=6000 ;;
esac

# link_calls <log text> <owner>: the summed calls of the owner's links.
link_calls() {
    printf '%s\n' "$1" |
        sed -n "s/.*link-calls owner=$2 addr=.* calls=\([0-9]*\)$/\1/p" |
        awk '{n += $1} END {print n + 0}'
}

# double_chain <log text> <pattern>: 1 when two consecutive doubling lines
# show the second pickup starting from the first one's written total
# (after2 - delta2 == after1 + delta1).
double_chain() {
    printf '%s\n' "$1" |
        sed -n "s/.*$2: +\([0-9]*\) twice, total \([0-9]*\) -> \([0-9]*\).*/\1 \2 \3/p" |
        awk 'NR > 1 && $2 - $1 == prev { ok = 1 } { prev = $3 } END { print ok + 0 }'
}

ARMS_MODS="hello-anchor hud-stopwatch pad-probe mouse-probe sfx-play-probe chain-duo-a chain-duo-b arena-canary double-gems"
say "per-game arms: $ARMS_MODS engage on $PG_GAME ($DEMO_FRAMES VBLs)"
# shellcheck disable=SC2086
fixture "$WORK/arms" $ARMS_MODS
out="$( PG_CAPTURE_ENV=(OPENPETE_MOD_LINK_CALLS=1)
        RUN_TIMEOUT=600 run_game "$WORK/arms" "$DEMO_FRAMES" )"
for m in $ARMS_MODS; do
    n="$(link_calls "$out" "$m")"
    if printf '%s\n' "$out" | grep -q "loaded $m " && [ "$n" -gt 0 ]; then
        ok "$m: $n calls"
    else
        bad "$m: loaded line or calls missing ($n calls)"
        printf '%s\n' "$out" | grep -E "$m" | head -3
    fi
done
n_dbl="$(printf '%s\n' "$out" | grep -c "gem pickup doubled: ")"
if [ "$n_dbl" -gt 0 ] && [ "$(double_chain "$out" "gem pickup doubled")" -eq 1 ]; then
    ok "double-gems: $n_dbl pickups doubled; a later pickup started from the doubled total"
else
    bad "double-gems: $n_dbl pickups doubled, no pickup started from a doubled total"
fi

say "per-game arms: natural-gems engages on $PG_GAME"
fixture "$WORK/natural" natural-gems
out="$( PG_CAPTURE_ENV=(OPENPETE_MOD_LINK_CALLS=1)
        RUN_TIMEOUT=600 run_game "$WORK/natural" "$DEMO_FRAMES" )"
n="$(link_calls "$out" natural-gems)"
n_dbl="$(printf '%s\n' "$out" | grep -c "natural gem double: ")"
if [ "$n" -gt 0 ] && [ "$n_dbl" -gt 0 ] &&
   [ "$(double_chain "$out" "natural gem double")" -eq 1 ]; then
    ok "natural-gems: $n calls, $n_dbl pickups doubled, doubled total carried"
else
    bad "natural-gems: $n calls, $n_dbl pickups doubled or the doubled total not carried"
fi
if [ "$PG_GAME" = "spyro-1" ]; then
    if printf '%s\n' "$out" | grep -q "natural call Sin(1024) -> 4096"; then
        ok "natural-gems: Sin(1024) -> 4096"
    else
        bad "natural-gems: typed Sin call missing or wrong"
    fi
fi

# ── M9. Spyro 2 and 3 copies of the Spyro 1 arms rungs ───────────────────
if [ "$PG_GAME" != "spyro-1" ]; then
    say "chain-duo: enabled order sets nesting on $PG_GAME"
    for order in "chain-duo-a chain-duo-b" "chain-duo-b chain-duo-a"; do
        # shellcheck disable=SC2086
        fixture "$WORK/duo-${order%% *}" $order
        out="$(run_game "$WORK/duo-${order%% *}" "$DEMO_FRAMES")"
        seq="$(printf '%s\n' "$out" | grep -oE 'chain (enter|exit)  ?[<>AB]+' |
               tr -d ' ' | paste -sd, -)"
        want="chainenterB>,chainenterA>,chainexit<A,chainexit<B"
        [ "${order%% *}" = chain-duo-b ] &&
            want="chainenterA>,chainenterB>,chainexit<B,chainexit<A"
        if [ "$seq" = "$want" ]; then
            ok "order [$order]: $seq"
        else
            bad "order [$order]: $seq (want $want)"
        fi
    done

    say "crash-dummy: the fault is attributed to the mod on $PG_GAME"
    fixture "$WORK/crash-$PG_GAME" crash-dummy
    out="$(run_game "$WORK/crash-$PG_GAME" "$DEMO_FRAMES")"; rc=$?
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] &&
       printf '%s\n' "$out" | grep -q "MOD ATTRIBUTION: .* mod 'crash-dummy'"; then
        ok "process died (rc=$rc), report names crash-dummy"
    else
        bad "crash-dummy: rc=$rc or attribution line missing"
        printf '%s\n' "$out" | grep -A3 "openpete crash" | head -6
    fi

    say "arena-canary: armed, saved and restored on $PG_GAME"
    fixture "$WORK/canary-$PG_GAME" arena-canary
    st="$WORK/canary-$PG_GAME.state"
    out="$(RUN_OUTS="$st" run_game "$WORK/canary-$PG_GAME" 5000 \
            --state-save-on-frame 4500 --state-save-output "$st")"
    if printf '%s\n' "$out" | grep -q "arena-canary: armed" &&
       printf '%s\n' "$out" | grep -q "savestate: saved"; then
        ok "canary armed; savestate written"
    else
        bad "canary not armed or savestate not written"
    fi
    out="$(run_game "$WORK/canary-$PG_GAME" 6000 --state-load-on-frame 5000 \
            --state-load-input "$st")"
    if printf '%s\n' "$out" | grep -q "arena ledger applied: .* 64 B restored" &&
       printf '%s\n' "$out" | grep -q "savestate: loaded" &&
       ! printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
        ok "file load restored the 64 B arena block, no DESYNC"
    else
        bad "file load: arena not restored, or DESYNC"
        printf '%s\n' "$out" | grep -E 'arena|savestate' | head -5
    fi
fi
