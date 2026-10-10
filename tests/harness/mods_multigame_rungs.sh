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
