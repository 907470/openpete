# mods_multigame_rungs.sh - the rungs of tests/harness/check_mods.sh whose
# fixtures load on every game. SOURCED by it after mods_sdk_rungs.sh (helpers
# say, ok, bad, fixture, run_game, mod_toml; WORK, MODS_SRC, PG_GAME, KEY0,
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
#                   `compile FAILED`, each mod logs armed; scene-sepia's
#                   present-hook draw_text lays out overlay vertices (the
#                   run renders one screenshot, so the hook runs).
#   compile-mods    --compile-mods against the test SDK writes a crt-beans
#                   object under .build/<game>/ for every staged game.
#   assets          a mod with assets/ and assets/spyro-2/ registers
#                   assets/spyro-2 on Spyro 2 and assets/ elsewhere.
#   materials       [[material]] rows by game key and the running game's
#                   material channel mask.
#   levels          (a game with no level ops) a [[level]] row is refused
#                   with one line naming the game; the mod still loads.
#   channel refusal under OPENPETE_MOD_CHANNEL_MASK withholding moby, a
#                   [[material]] row, both C registration entries on moby
#                   and material_set_selector retuning the terrain material
#                   to moby are refused with one line each; terrain
#                   registers and a retune to terrain returns 0.
#   unsupported     read_level_data and moby_classes, called twice each,
#                   return OPENPETE_MOD_ERR_UNSUPPORTED on a game whose
#                   descriptor leaves their member NULL, with one log line
#                   per entry naming the game; none on Spyro 1.
#   vstream refusal (Spyro 2 and 3) a per-game object using vstream_handle
#                   is refused at compile time; a "*" object gets 0 from two
#                   calls with one log line.
#   classes cap     a mod built with a 512-entry buffer gets
#                   min(size, capacity) ascending ids from moby_classes, ids
#                   of 512 and above on Spyro 3, and its link on the moby
#                   ops' update anchor counts calls above 0.
#   engagement      every in-tree mod with per-game arms (the camera-clock
#                   mods, double-gems, natural-gems, raw-probe) loads and its own
#                   override link counts calls above 0 on the attract demo
#                   (OPENPETE_MOD_LINK_CALLS); double-gems and natural-gems
#                   each double a pickup, and a later pickup in the same
#                   run starts from the doubled total, so the write landed
#                   in guest RAM.
#   game arms       game-arms registers its game's arm: the loader's
#                   "override registered" lines for the mod equal the
#                   game's expected count, and every bound address counts
#                   calls above 0 on the attract demo.
#   asset variants  sfx-single-sound, hd-single-texture and
#                   declarative-example register their game's assets/<game>/
#                   subtree; on the attract demo the symbolic sound replaces
#                   a keyed voice and the atlas serves draws at title-screen
#                   presents.
#   sfx_replace     sfx-replace-probe's buzz over its game's default slot
#                   serves, and every key-on and end transition the guest
#                   reads lands on the stock VBL.
#   pose-report     the mod's source built as a "*" object
#                   (-DOPENPETE_MOD_ANY_GAME, no per-game include path)
#                   loads from .build/any/ and reports the player's pose
#                   through the game services at screenshot presents: a
#                   level id, gameplay live, the pose's level equal to
#                   level_id, and at least two distinct positions.
#   arms copies     (Spyro 2 and 3; Spyro 1 runs its own in check_mods.sh)
#                   observer-neutrality (hello-anchor, raw-probe and
#                   chain-duo vs the game's oracle on the smoke window),
#                   chain-duo nesting in both enabled orders, crash-dummy
#                   attribution, arena-canary armed with no DESYNC across a
#                   file save and load that restores its 64 B block.
#   anchor link     a pass-through link (its body only calls base()) on the
#                   game's oracle anchor (games/<game>/oracle.conf
#                   ANCHOR_PC) leaves the oracle at PASS on the smoke run:
#                   base() re-enters the anchor's prologue, and that
#                   re-dispatch must not count the anchor hit, the loop
#                   tick or the pad-ring consumer a second time. The link
#                   counts calls above 0 and the census counts re-dispatches
#                   that skipped the entry observers above 0.
#   render mods     (Spyro 2 and 3) declarative-reshade, moby-instance-tint,
#                   channel-gallery (every row lit) and late-pipes each
#                   repaint pixels at a corpus pin against a no-mod baseline
#                   that a control leg repeats exactly; each logs its rows
#                   or passes armed and none is refused. The gallery's moby
#                   refine row keyed on state 1 repaints a strict nonzero
#                   subset of its any-state leg and on state 250 nothing.
#                   imgui-panel's tick
#                   arm counts calls above 0 and live mobys between 1 and
#                   4095 with the oracle at PASS on the smoke run.
#   scan watch      (Spyro 3) the mod guard restores and names a mod's store
#                   into a scanned code range, a store below the range
#                   lands, a host poke is reported, the overlay load is not;
#                   under --scan-guard 0 the store persists with no compare.
#                   (Spyro 1 and 2) --scan-guard 0 is accepted and inert.

# link_calls <log text> <owner>: the summed calls of the owner's links.
link_calls() {
    printf '%s\n' "$1" |
        sed -n "s/.*link-calls owner=$2 addr=.* calls=\([0-9]*\)$/\1/p" |
        awk '{n += $1} END {print n + 0}'
}

# ── anchor link: a pass-through link on the oracle anchor is neutral ─────
ANCHOR="$(sed -n 's/^ANCHOR_PC=\(0x[0-9A-Fa-f]*\).*/\1/p' "games/$PG_GAME/oracle.conf" | head -1)"
say "anchor link: a pass-through link on $PG_GAME's oracle anchor $ANCHOR vs the oracle"
mkdir -p "$WORK/anchorlink/anchor-link/src"
mod_toml "$WORK/anchorlink/anchor-link" anchor-link \
    "game = [\"$PG_GAME\"]"$'\n'"state = \"none\""$'\n'"runahead = \"pure\""$'\n'"sim_mutative = false"
cat > "$WORK/anchorlink/anchor-link/src/main.c" <<EOF_ANCHOR
#include <openpete_mod_api.h>
static const openpete_mod_api_t* api;
static void pass_through(CPUState* cpu) { api->base(cpu); }
int openpete_mod_entry(const openpete_mod_api_t* a, openpete_mod_t* self) {
    api = a;
    return a->override_addr(self, ${ANCHOR}u, pass_through);
}
EOF_ANCHOR
echo anchor-link > "$WORK/anchorlink/enabled.txt"
out="$(OPENPETE_MOD_LINK_CALLS=1 OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES \
        tests/harness/check_psx_oracle.sh --game "$PG_GAME" -- --mods-dir "$WORK/anchorlink" 2>&1)"
n_link="$(link_calls "$out" anchor-link)"
n_redis="$(printf '%s\n' "$out" | sed -n 's/.*re-dispatches past the entry observers: \([0-9]*\).*/\1/p' | tail -1)"
if printf '%s\n' "$out" | grep -q "verdict: PASS" && [ "$n_link" -gt 0 ] && [ "${n_redis:-0}" -gt 0 ]; then
    ok "anchor link on $ANCHOR: oracle PASS, $n_link link calls, $n_redis re-dispatches past the observers"
else
    bad "anchor link on $ANCHOR: oracle not PASS or not engaged ($n_link link calls, ${n_redis:-?} re-dispatches)"
    printf '%s\n' "$out" | grep -E "verdict|hard_fail|first divergence|anchor-link" | head -4
fi

# Copy an in-tree mod into <dest dir> without its .build/.
copy_mod() { # copy_mod <id> <dest dir>
    mkdir -p "$2"
    cp -r "$MODS_SRC/$1" "$2/$1"
    rm -rf "${2:?}/$1/.build"
}

# ── M1. lib-and-client: dependency order, missing dependency, cycle ──────
say "lib-and-client: client listed FIRST - topo-sort must load lib first"
fixture "$WORK/libclient" lib-and-client demo-lib
out="$(run_game "$WORK/libclient" $((600 + KEY0)))"
lib_line=$(printf '%s\n' "$out" | grep -n "demo-lib: entry ran" | head -1 | cut -d: -f1)
cli_line=$(printf '%s\n' "$out" | grep -n "lib-and-client: entry ran" | head -1 | cut -d: -f1)
if [ -n "$lib_line" ] && [ -n "$cli_line" ] && [ "$lib_line" -lt "$cli_line" ]; then
    ok "dependency entry ran first despite reversed enabled order"
else
    bad "dependency order wrong or entries missing (lib=$lib_line cli=$cli_line)"
fi

say "lib-and-client: missing dep - actionable error, run survives"
fixture "$WORK/nodep" lib-and-client
out="$(run_game "$WORK/nodep" $((600 + KEY0)))"
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
out="$(run_game "$WORK/cycle" $((600 + KEY0)))"
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
out="$(run_game "$WORK/arena" $((600 + KEY0)))"
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
out="$(RUN_OUTS="$WORK/postfx/shot-vk.png" run_game "$WORK/postfx" $((320 + KEY0)) \
        --renderer vk --screenshot-on-frame $((300 + KEY0)) \
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
n_verts="$(printf '%s\n' "$out" |
    sed -n 's/.*draw service live: \([0-9]*\) overlay verts.*/\1/p' | head -1)"
if [ "$n_text" -eq 0 ] && [ "${n_verts:-0}" -gt 0 ]; then
    ok "draw_text served: ${n_verts} overlay verts, no unsupported line"
else
    bad "draw_text on $PG_GAME: ${n_verts:-0} overlay verts, $n_text unsupported lines"
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
out="$(run_game "$WORK/assets" $((60 + KEY0)))"
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
# row 2 has no game key on channel "any". Every game serves every channel,
# so one row leaves by its game key and two stay: rows 0 and 2 on Spyro 1,
# rows 1 and 2 on Spyro 2 and 3. The rows are exclusive (terminal at api
# 13), so the overlapping moby and any rows never compose.
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
exclusive = true

[[material]]
game    = ["spyro-2", "spyro-3"]
channel = "moby"
frag    = "$frag"
exclusive = true

[[material]]
channel = "any"
frag    = "$frag"
exclusive = true
TEOF
mkdir -p "$WORK/mat"
ln -sfn "$d" "$WORK/mat/mat-games"
echo mat-games > "$WORK/mat/enabled.txt"
out="$(run_game "$WORK/mat" $((60 + KEY0)))"
n_game="$(printf '%s\n' "$out" | grep -c "\[\[material\]\] row [0-9] targets other games - skipped (this is $PG_GAME)")"
n_chan="$(printf '%s\n' "$out" | grep -c "\[\[material\]\] row [0-9]: channel -\?[0-9] unsupported on $PG_GAME - skipped")"
n_kept="$(printf '%s\n' "$out" | sed -n 's/.*mat-games: \([0-9]*\) declarative material row(s).*/\1/p' | head -1)"
want_game=1 want_chan=0 want_kept=2
if [ "$n_game" -eq "$want_game" ] && [ "$n_chan" -eq "$want_chan" ] &&
   [ "${n_kept:-0}" -eq "$want_kept" ]; then
    ok "rows kept ${n_kept:-0}, skipped by game $n_game, by channel $n_chan"
else
    bad "rows kept ${n_kept:-0}/$want_kept, by game $n_game/$want_game, by channel $n_chan/$want_chan"
    printf '%s\n' "$out" | grep -E 'material' | head -6
fi

# ── M6b. a withheld channel is refused on both registration paths ────────
# OPENPETE_MOD_CHANNEL_MASK=0x3FC withholds the moby channel (bit 1) from the
# running game's mask. The mod carries a declarative moby row and, from C,
# registers one material on moby through material_register, one through
# material_register_refine, and one on terrain. The declarative row is
# skipped with its line; both C moby registrations return -1 with one
# refusal line each; the terrain registration returns a handle. Retuning
# that handle's selector to moby through material_set_selector returns -1
# with one refusal line; retuning it to terrain returns 0.
say "materials: a withheld channel is refused by [[material]], material_register and material_set_selector on $PG_GAME"
d="$WORK/chanref/chan-refuse"
mod_toml "$d" chan-refuse 'game = ["spyro-1", "spyro-2", "spyro-3"]'
mkdir -p "$d/shaders" "$d/.build/any"
cp "$MODS_SRC/declarative-reshade/shaders/tint.frag" "$d/shaders/"
cat >> "$d/mod.toml" <<'TEOF'

[[material]]
channel = "moby"
frag    = "shaders/tint.frag"
TEOF
cat > "$WORK/chan-refuse.c" <<'CEOF'
#include <openpete_mod_api.h>
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    (void)key; (void)out; (void)cap; return -1;
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    int sh = api->shader_register(self, "shaders/tint.frag");
    openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
    sel.channel = OP_CHAN_MOBY;
    int a = api->material_register(self, &sel, sh);
    int b = api->material_register_refine(self, &sel, sh, refine);
    sel.channel = OP_CHAN_TERRAIN;
    int c = api->material_register(self, &sel, sh);
    sel.channel = OP_CHAN_MOBY;
    int d = api->material_set_selector(self, c, &sel);
    sel.channel = OP_CHAN_TERRAIN;
    int e = api->material_set_selector(self, c, &sel);
    api->log(self, OP_MOD_LOG_INFO, "chan-refuse shader %d moby %d moby-refine %d terrain %d "
             "retune-moby %d retune-terrain %d", sh, a, b, c, d, e);
    return 0;
}
CEOF
echo chan-refuse > "$WORK/chanref/enabled.txt"
if "$CC_MOD" -shared -fPIC -O2 -Wall -Werror -DOPENPETE_MOD_ANY_GAME \
        -I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c" "$WORK/chan-refuse.c" \
        -o "$d/.build/any/chan-refuse.so" > "$WORK/chan-refuse-cc.log" 2>&1; then
    out="$( PG_CAPTURE_ENV=(OPENPETE_MOD_CHANNEL_MASK=0x3FC)
            run_game "$WORK/chanref" $((60 + KEY0)) )"
    res="$(printf '%s\n' "$out" | sed -n 's/.*chan-refuse shader \(-\?[0-9]*\) moby \(-\?[0-9]*\) moby-refine \(-\?[0-9]*\) terrain \(-\?[0-9]*\) retune-moby \(-\?[0-9]*\) retune-terrain \(-\?[0-9]*\).*/\1 \2 \3 \4 \5 \6/p')"
    n_c="$(printf '%s\n' "$out" | grep -c "material_register: channel 1 unsupported on $PG_GAME - refused")"
    n_cr="$(printf '%s\n' "$out" | grep -c "material_register_refine: channel 1 unsupported on $PG_GAME - refused")"
    n_row="$(printf '%s\n' "$out" | grep -c "\[\[material\]\] row 0: channel 1 unsupported on $PG_GAME - skipped")"
    n_sel="$(printf '%s\n' "$out" | grep -c "material_set_selector: channel 1 unsupported on $PG_GAME - refused")"
    read -r r_sh r_a r_b r_c r_d r_e <<<"${res:-x x x x x x}"
    if [ "$r_sh" -ge 0 ] 2>/dev/null && [ "$r_a" = -1 ] && [ "$r_b" = -1 ] &&
       [ "$r_c" -ge 0 ] 2>/dev/null && [ "$r_d" = -1 ] && [ "$r_e" = 0 ] &&
       [ "$n_c" -eq 1 ] && [ "$n_cr" -eq 1 ] && [ "$n_row" -eq 1 ] &&
       [ "$n_sel" -eq 1 ]; then
        ok "moby refused: register $r_a, refine $r_b, retune $r_d, row skipped; terrain handle $r_c, retune $r_e; refusal lines $n_c + $n_cr + $n_sel + $n_row"
    else
        bad "returns [${res:-none}] (want shader>=0 -1 -1 terrain>=0 -1 0), refusal lines $n_c/1 $n_cr/1 retune $n_sel/1 row $n_row/1"
        printf '%s\n' "$out" | grep -E 'chan-refuse|material' | head -8
    fi
else
    bad "chan-refuse \"*\" build failed"
    head -5 "$WORK/chan-refuse-cc.log"
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
    out="$(run_game "$WORK/lvl" $((60 + KEY0)))"
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

# ── M7b. OPENPETE_MOD_ERR_UNSUPPORTED from descriptor-backed entries ─────
# A "*" object calls read_level_data and moby_classes twice each at entry.
# A game whose descriptor leaves the member NULL returns
# OPENPETE_MOD_ERR_UNSUPPORTED from both calls of that entry and logs one
# line naming the game: neither on Spyro 1, read_level_data alone on
# Spyro 2 and Spyro 3, which serve the moby class census.
say "unsupported: read_level_data and moby_classes on $PG_GAME"
d="$WORK/unsup/unsup-probe"
mod_toml "$d" unsup-probe
mkdir -p "$d/.build/any"
cat > "$WORK/unsup-probe.c" <<'CEOF'
#include <openpete_mod_api.h>
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    uint8_t  buf[16];
    uint16_t cls[8];
    for (int i = 0; i < 2; i++) {
        int a = api->read_level_data(self, 10, buf, sizeof buf);
        int b = api->moby_classes(self, cls, 8);
        api->log(self, OP_MOD_LOG_INFO, "unsup-probe call %d read_level_data %d "
                 "moby_classes %d (unsupported %d)", i, a, b,
                 OPENPETE_MOD_ERR_UNSUPPORTED);
    }
    return 0;
}
CEOF
echo unsup-probe > "$WORK/unsup/enabled.txt"
if "$CC_MOD" -shared -fPIC -O2 -Wall -Werror -DOPENPETE_MOD_ANY_GAME \
        -I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c" "$WORK/unsup-probe.c" \
        -o "$d/.build/any/unsup-probe.so" > "$WORK/unsup-cc.log" 2>&1; then
    out="$(run_game "$WORK/unsup" $((60 + KEY0)))"
    calls="$(printf '%s\n' "$out" | sed -n 's/.*unsup-probe call [01] read_level_data \(-\?[0-9]*\) moby_classes \(-\?[0-9]*\) (unsupported \(-\?[0-9]*\)).*/\1 \2 \3/p')"
    n_calls="$(printf '%s\n' "$calls" | grep -c .)"
    n_unsup="$(printf '%s\n' "$calls" | awk 'NF { n += ($1 == $3) + ($2 == $3) } END { print n + 0 }')"
    n_rld="$(printf '%s\n' "$out" | grep -c "read_level_data: $PG_GAME serves no level data - unsupported")"
    n_moby="$(printf '%s\n' "$out" | grep -c "moby_classes: $PG_GAME serves no moby class census - unsupported")"
    case "$PG_GAME" in
        spyro-1) want_unsup=0 want_rld=0 want_moby=0 ;;
        *)       want_unsup=2 want_rld=1 want_moby=0 ;;
    esac
    if [ "$n_calls" -eq 2 ] && [ "$n_unsup" -eq "$want_unsup" ] &&
       [ "$n_rld" -eq "$want_rld" ] && [ "$n_moby" -eq "$want_moby" ]; then
        ok "unsupported returns $n_unsup/4, log lines read_level_data $n_rld moby_classes $n_moby; returns [$(printf '%s\n' "$calls" | paste -sd ';')]"
    else
        bad "calls $n_calls/2, unsupported returns $n_unsup/$want_unsup, log lines $n_rld/$want_rld and $n_moby/$want_moby"
        printf '%s\n' "$out" | grep -E 'unsup-probe|unsupported' | head -8
    fi
else
    bad "unsup-probe \"*\" build failed"
    head -5 "$WORK/unsup-cc.log"
fi

# ── M7d. vstream_handle on a game without the vertex-stream field ───────
# Spyro 2 and 3 frame words carry no 21-bit stream field. A per-game object
# using the member fails to compile with the reason in the error: the
# `unavailable` message under clang, the missing explanatory member name
# under a compiler without that attribute. A "*" object compiles, and its
# two calls return 0 with one log line naming the game.
if [ "$PG_GAME" != "spyro-1" ]; then
    say "vstream_handle: refused at compile time per game, 0 at run time for \"*\" on $PG_GAME"
    cat > "$WORK/vs-probe.c" <<'CEOF'
#include <openpete_mod_api.h>
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    uint32_t a = api->vstream_handle(self, 0x80100000u);
    uint32_t b = api->vstream_handle(self, 0x80100040u);
    api->log(self, OP_MOD_LOG_INFO, "vs-probe handles %u %u", a, b);
    return 0;
}
CEOF
    "$CC_MOD" -c -O2 -Wall -I"$SDK_GAME_DIR" -I"$SDK_ROOT" "$WORK/vs-probe.c" \
        -o "$WORK/vs-probe-game.o" > "$WORK/vs-probe-game.log" 2>&1
    vs_rc=$?
    n_msg="$(grep -cE "vstream_handle is Spyro 1 only|vstream_handle_is_spyro1_only__move_streams_through_the_animation_header" "$WORK/vs-probe-game.log")"
    if [ "$vs_rc" -ne 0 ] && [ "$n_msg" -ge 1 ]; then
        ok "per-game build refused (rc $vs_rc, $n_msg error line(s) naming the reason)"
    else
        bad "per-game build: rc $vs_rc, message lines $n_msg"
        head -5 "$WORK/vs-probe-game.log"
    fi
    d="$WORK/vsrun/vs-probe"
    mod_toml "$d" vs-probe
    mkdir -p "$d/.build/any"
    echo vs-probe > "$WORK/vsrun/enabled.txt"
    if "$CC_MOD" -shared -fPIC -O2 -Wall -Werror -DOPENPETE_MOD_ANY_GAME \
            -I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c" "$WORK/vs-probe.c" \
            -o "$d/.build/any/vs-probe.so" > "$WORK/vs-probe-cc.log" 2>&1; then
        out="$(run_game "$WORK/vsrun" $((60 + KEY0)))"
        n_line="$(printf '%s\n' "$out" | grep -c "vstream_handle: $PG_GAME serves no vertex-stream field - unsupported (0)")"
        if printf '%s\n' "$out" | grep -q "vs-probe handles 0 0" && [ "$n_line" -eq 1 ]; then
            ok "\"*\" object: both calls 0, $n_line log line"
        else
            bad "\"*\" object: handles line missing or log lines $n_line (want 1)"
            printf '%s\n' "$out" | grep -E 'vs-probe|vstream' | head -4
        fi
    else
        bad "vs-probe \"*\" build failed"
        head -5 "$WORK/vs-probe-cc.log"
    fi
fi

# ── M7c. moby_classes through a buffer of the pre-768 cap ────────────────
# On each game a "*" object built with a 512-entry buffer links the master
# update the moby ops anchor on (Spyro 1 GamestateUpdate 0x8003385C, Spyro
# 2 func_8001B140, Spyro 3 Update 0x80055400) and, each time the
# set's size changes, sizes it (NULL, 0), copies it into the 512-entry buffer
# and into an 8-entry one. Every copy returns min(size, capacity) ids in
# ascending order, the link counts calls above 0, and a nonempty set is seen
# on the attract demo. On Spyro 3, whose model table has 768 rows, a class id
# of 512 or above arrives through the 512-entry buffer.
case "$PG_GAME" in
    spyro-1) upd=0x8003385Cu cls_frames=6000 ;;
    spyro-2) upd=0x8001B140u cls_frames=4073 ;;
    spyro-3) upd=0x80055400u cls_frames=6573 ;;
    *)       upd="" ;;
esac
if [ -n "$upd" ]; then
    say "moby_classes: a mod built with a 512-entry buffer on $PG_GAME"
    d="$WORK/clscap/classes-cap"
    mod_toml "$d" classes-cap
    mkdir -p "$d/.build/any"
    cat > "$WORK/classes-cap.c" <<'CEOF'
#include <stddef.h>
#include <openpete_mod_api.h>
#define OLD_CAP 512 /* OPENPETE_MOBY_CLASSES_MAX before Spyro 3's 768 rows */
static const openpete_mod_api_t* A; static openpete_mod_t* S;
static uint16_t buf[OLD_CAP], small[8];
static int last = -1;
static void update_link(CPUState* cpu) {
    A->base(cpu);
    int n = A->moby_classes(S, NULL, 0);
    if (n == last) return;
    last = n;
    int got = A->moby_classes(S, buf, OLD_CAP);
    int got8 = A->moby_classes(S, small, 8);
    int asc = 1, max = -1;
    for (int i = 0; i < got; i++) {
        if (i && buf[i] <= buf[i - 1]) asc = 0;
        if (buf[i] > max) max = buf[i];
    }
    for (int i = 0; i < got8; i++) if (small[i] != buf[i]) asc = 0;
    A->log(S, OP_MOD_LOG_INFO, "classes-cap n %d got %d got8 %d ordered %d max %d",
           n, got, got8, asc, max);
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    A = api; S = self;
    return api->override_addr(self, UPDATE_ADDR, update_link);
}
CEOF
    echo classes-cap > "$WORK/clscap/enabled.txt"
    if "$CC_MOD" -shared -fPIC -O2 -Wall -Werror -DOPENPETE_MOD_ANY_GAME \
            -DUPDATE_ADDR="$upd" -I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c" \
            "$WORK/classes-cap.c" -o "$d/.build/any/classes-cap.so" \
            > "$WORK/classes-cap-cc.log" 2>&1; then
        out="$( PG_CAPTURE_ENV=(OPENPETE_MOD_LINK_CALLS=1)
                RUN_TIMEOUT=600 run_game "$WORK/clscap" "$cls_frames" )"
        rows="$(printf '%s\n' "$out" | sed -n 's/.*classes-cap n \(-\?[0-9]*\) got \(-\?[0-9]*\) got8 \(-\?[0-9]*\) ordered \([01]\) max \(-\?[0-9]*\).*/\1 \2 \3 \4 \5/p')"
        verdict="$(printf '%s\n' "$rows" | awk -v game="$PG_GAME" '
            NF { lines++
                 want = $1 < 512 ? $1 : 512; want8 = $1 < 8 ? $1 : 8
                 if ($2 != want || $3 != want8 || $4 != 1) badrow++
                 if ($1 > 0) live++
                 if ($5 > peak) peak = $5 }
            END { ok = lines > 0 && badrow == 0 && live > 0
                  if (game == "spyro-3" && peak < 512) ok = 0
                  printf "%d %d %d %d %d\n", ok, lines + 0, badrow + 0, live + 0, peak + 0 }')"
        read -r v_ok v_lines v_bad v_live v_peak <<<"$verdict"
        n_link="$(printf '%s\n' "$out" |
            sed -n "s/.*link-calls owner=classes-cap addr=${upd%u} .* calls=\([0-9]*\)$/\1/Ip" |
            awk '{n += $1} END {print n + 0}')"
        if [ "$v_ok" -eq 1 ] && [ "$n_link" -gt 0 ]; then
            ok "$v_lines set sizes, $v_live nonempty, every copy min(size, cap) and ascending; highest id $v_peak; link at $upd: $n_link calls"
        else
            bad "set sizes $v_lines, bad copies $v_bad, nonempty $v_live, highest id $v_peak, link calls $n_link"
            printf '%s\n' "$out" | grep -E 'classes-cap' | head -6
        fi
    else
        bad "classes-cap \"*\" build failed"
        head -5 "$WORK/classes-cap-cc.log"
    fi
fi

# ── M8. per-game arms: engagement on the attract demo ────────────────────
# DEMO_FRAMES covers each game's first two consecutive CollectItem-class
# pickups on the attract demo: Spyro 1 near yield 4100, Spyro 2 near VBL
# 14873, Spyro 3 near VBL 3573.
case "$PG_GAME" in
    spyro-1) DEMO_FRAMES=6000 ;;
    spyro-2) DEMO_FRAMES=16073 ;;
    spyro-3) DEMO_FRAMES=6573 ;;
    *)       DEMO_FRAMES=6000 ;;
esac

# double_chain <log text> <pattern>: 1 when two consecutive doubling lines
# show the second pickup starting from the first one's written total
# (after2 - delta2 == after1 + delta1).
double_chain() {
    printf '%s\n' "$1" |
        sed -n "s/.*$2: +\([0-9]*\) twice, total \([0-9]*\) -> \([0-9]*\).*/\1 \2 \3/p" |
        awk 'NR > 1 && $2 - $1 == prev { ok = 1 } { prev = $3 } END { print ok + 0 }'
}

ARMS_MODS="hello-anchor hud-stopwatch pad-probe mouse-probe sfx-play-probe chain-duo-a chain-duo-b arena-canary double-gems game-arms raw-probe"
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

# raw-probe binds the game's player draw by address and logs its registers
# on the first call and every 500th.
if printf '%s\n' "$out" | grep -q "raw probe #1: a0=0x"; then
    ok "raw-probe: $(printf '%s\n' "$out" | grep -c "raw probe #") register log lines"
else
    bad "raw-probe: no first-call register log line"
fi

# game-arms binds the camera update, the collect function and the sine on
# every game: by name on Spyro 1, by address on Spyro 2 and 3.
case "$PG_GAME" in
    spyro-1|spyro-2|spyro-3) want_reg=3 ;;
    *) want_reg=0 ;;
esac
n_reg="$(printf '%s\n' "$out" | grep -c "mod:game-arms.*override registered")"
arm_calls="$(printf '%s\n' "$out" |
    sed -n 's/.*link-calls owner=game-arms addr=\(0x[0-9A-F]*\) .* calls=\([0-9]*\)$/\1 \2/p' |
    awk '{ n[$1] += $2 } END { for (a in n) if (n[a] > 0) printf "%s=%d ", a, n[a] }')"
n_addr="$(printf '%s' "$arm_calls" | wc -w)"
if [ "$n_reg" -eq "$want_reg" ] && [ "$n_addr" -eq "$want_reg" ]; then
    ok "game-arms: $n_reg/$want_reg overrides registered, $n_addr/$want_reg addresses called (${arm_calls% })"
else
    bad "game-arms: $n_reg/$want_reg overrides registered, $n_addr/$want_reg addresses called"
    printf '%s\n' "$out" | grep -E "game-arms" | head -8
fi

say "pose-report: a \"*\" object reports the pose on $PG_GAME"
d="$WORK/pose/pose-report"
mkdir -p "$WORK/pose"
cp -r "$MODS_SRC/pose-report" "$d"
rm -rf "${d:?}/.build" "${d:?}/src"
mkdir -p "$d/.build/any"
echo pose-report > "$WORK/pose/enabled.txt"
if "$CC_MOD" -shared -fPIC -O2 -fno-strict-aliasing -Wall -Wundef -Werror \
        -DOPENPETE_MOD_ANY_GAME -I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c" \
        "$MODS_SRC/pose-report/src/pose_report.c" -o "$d/.build/any/pose-report.so" \
        > "$WORK/pose-cc.log" 2>&1; then
    ok "pose-report builds as a \"*\" object"
else
    bad "pose-report \"*\" build failed"
    head -5 "$WORK/pose-cc.log"
fi
# A headless run presents only at a screenshot VBL, so the run takes ten,
# spread over the demo window.
shots=""
for k in 1 2 3 4 5 6 7 8 9 10; do
    shots="${shots:+$shots,}$((DEMO_FRAMES * k / 11))"
done
out="$( RUN_TIMEOUT=600 run_game "$WORK/pose" "$DEMO_FRAMES" \
        --renderer vk --screenshot-native-only --screenshot-on-frame "$shots" \
        --screenshot-output "$WORK/pose/shot__{n}.png" )"
pose_lines="$(printf '%s\n' "$out" |
    sed -n 's/.*pose tick \([0-9]*\) live \(-\?[0-9]*\) level \(-\?[0-9]*\)\/\(-\?[0-9]*\) pos (\(-\?[0-9]*\), \(-\?[0-9]*\), \(-\?[0-9]*\)).*/\1 \2 \3 \4 \5 \6 \7/p')"
n_pose="$(printf '%s\n' "$pose_lines" | grep -c .)"
n_pos="$(printf '%s\n' "$pose_lines" | awk 'NF { seen[$5 " " $6 " " $7] = 1 } END { print length(seen) }')"
n_bad="$(printf '%s\n' "$pose_lines" | awk 'NF && ($1 == 0 || $2 != 1 || $3 < 0 || $3 != $4) { n++ } END { print n + 0 }')"
if printf '%s\n' "$out" | grep -q "loaded pose-report " &&
   printf '%s\n' "$out" | grep -q "running on $PG_GAME" &&
   [ "$n_pose" -ge 2 ] && [ "$n_pos" -ge 2 ] && [ "$n_bad" -eq 0 ]; then
    ok "pose-report: $n_pose reports, $n_pos distinct positions; first [$(printf '%s\n' "$pose_lines" | awk 'NF { print; exit }')] last [$(printf '%s\n' "$pose_lines" | awk 'NF { l = $0 } END { print l }')] (tick live level level_id x y z)"
else
    bad "pose-report: $n_pose reports, $n_pos distinct positions, $n_bad inconsistent"
    printf '%s\n' "$out" | grep -E "pose-report|pose tick" | head -8
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
if printf '%s\n' "$out" | grep -q "natural call Sin(1024) -> 4096"; then
    ok "natural-gems: Sin(1024) -> 4096"
else
    bad "natural-gems: Sin call missing or wrong"
fi

# ── M8b. per-game asset variants and sfx_replace ────────────────────────
# sfx-single-sound and hd-single-texture each ship one subtree per game,
# named in that game's own terms (Spyro 1 gemPickup and title_ui, Spyro 2
# and 3 gemCollect and ui_atlas). Each mod's root must be its game's
# subtree, the symbolic sound must replace a keyed voice (the gem pickup
# lies inside DEMO_FRAMES) and the symbolic atlas must serve draws at
# title-screen presents (a headless run presents only at a screenshot
# VBL). declarative-example's texture and sound subtrees take the same
# checks; its texture serves in either form.
asset_shots=""
for k in 300 600 900 1200 1500 1800 2400; do asset_shots="${asset_shots:+$asset_shots,}$((k + KEY0))"; done
asset_engage() { # asset_engage <fixture dir> <mods...> -> report lines
    local dir="$1" out m replaced served
    shift
    out="$( RUN_TIMEOUT=600 run_game "$dir" "$DEMO_FRAMES" --mute 0 \
            --renderer vk --screenshot-native-only \
            --screenshot-on-frame "$asset_shots" \
            --screenshot-output "$dir-shot__{n}.png" )"
    for m in "$@"; do
        if printf '%s\n' "$out" | grep -q "asset root registered: .*/$m/assets/$PG_GAME ($m)"; then
            ok "$m: root assets/$PG_GAME"
        else
            bad "$m: root assets/$PG_GAME not registered"
            printf '%s\n' "$out" | grep "asset root" | head -3
        fi
    done
    replaced="$(printf '%s\n' "$out" | sed -n 's/^\[sfxpack\].* replaced=\([0-9]*\).*/\1/p' | head -1)"
    served="$(printf '%s\n' "$out" | sed -n 's/^\[texpack\] served=\([0-9]*\) .*/\1/p' | head -1)"
    if [ "${replaced:-0}" -gt 0 ] && [ "${served:-0}" -gt 0 ]; then
        ok "$*: sfx replaced=$replaced, texture served=$served draws ($DEMO_FRAMES VBLs)"
    else
        bad "$*: sfx replaced=${replaced:-0}, texture served=${served:-0} - vacuous"
        printf '%s\n' "$out" | grep -E '^\[sfxpack\]|^\[texpack\]' | head -5
    fi
}
say "asset variants: sfx-single-sound + hd-single-texture engage on $PG_GAME"
fixture "$WORK/assetvar" sfx-single-sound hd-single-texture
asset_engage "$WORK/assetvar" sfx-single-sound hd-single-texture
say "asset variants: declarative-example's sound and texture engage on $PG_GAME"
fixture "$WORK/declvar" declarative-example
asset_engage "$WORK/declvar" declarative-example

# sfx_replace: the guest sees every voice end on the STOCK tick. The mod
# API's sfx_replace registers a symbolic sfx-pack entry from memory; the
# swap happens at the PCM upload below the voice mirror, so the one-shot
# duration the game polls through SpuGetKeyStatus (and with it the tick it
# frees the sound slot and writes the sound-ref byte) is the stock
# sample's. sfx-replace-probe replaces its game's default slot (Spyro 1
# pickupDing, keyed ~11 times in 6000 yields of the attract demo; Spyro 2
# and 3 flame, first keyed near VBL 6523 and 5173) with a 2 s buzz.
# OPENPETE_SFX_END_LOG names every key-on and every ON/RESET transition
# the guest reads; the two logs must match line for line, and the pack
# census must show the replacement actually served (a green that did not
# engage is vacuous). Re-pointing the voice's addr/pitch instead moved the
# end and shifted the voice allocator's later choices from that tick on.
case "$PG_GAME" in
    spyro-2) SFXREP_FRAMES=8573 ;;
    spyro-3) SFXREP_FRAMES=6573 ;;
    *)       SFXREP_FRAMES=6000 ;;
esac
say "sfx_replace: end-tick neutrality on $PG_GAME - replaced voices end on the stock tick"
fixture "$WORK/sfxrep" sfx-replace-probe
mkdir -p "$WORK/nomods"; : > "$WORK/nomods/enabled.txt"
stock_keys="$(OPENPETE_SFX_END_LOG=1 run_game "$WORK/nomods" "$SFXREP_FRAMES" --mute 0 | grep '^\[sfx-key ' || true)"
probe_out="$(OPENPETE_SFX_END_LOG=1 run_game "$WORK/sfxrep" "$SFXREP_FRAMES" --mute 0)"
probe_keys="$(printf '%s\n' "$probe_out" | grep '^\[sfx-key ' || true)"
n_stock="$(printf '%s\n' "$stock_keys" | grep -c . || true)"
replaced="$(printf '%s\n' "$probe_out" | grep -o '^\[sfxpack\].*replaced=[0-9]*' | grep -o 'replaced=[0-9]*' | cut -d= -f2)"
if [ "${n_stock:-0}" -lt 50 ]; then
    bad "end-tick log too short to judge ($n_stock transitions)"
elif [ "${replaced:-0}" -lt 1 ]; then
    bad "replacement never served (replaced=${replaced:-0}) - vacuous"
    printf '%s\n' "$probe_out" | grep -i "sfx" | head -5
elif [ "$stock_keys" = "$probe_keys" ]; then
    ok "$n_stock key transitions identical with $replaced replaced plays"
else
    bad "sfx_replace moved a guest-visible key transition"
    diff <(printf '%s\n' "$stock_keys") <(printf '%s\n' "$probe_keys") | head -10
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

    say "observer-neutrality: hello-anchor + raw-probe + chain-duo vs the $PG_GAME oracle"
    # The moby class census reads guest RAM at every tick boundary while any
    # mod is loaded, so these four drive it on a game with moby ops. The
    # classes-cap object is left out: it links the oracle's anchor function
    # (Update on Spyro 3), and a pass-through link there fails the oracle
    # at record 1 as well.
    fixture "$WORK/observers-$PG_GAME" hello-anchor raw-probe chain-duo-a chain-duo-b
    out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES tests/harness/check_psx_oracle.sh \
            --game "$PG_GAME" -- --mods-dir "$WORK/observers-$PG_GAME" 2>&1)"
    if printf '%s\n' "$out" | grep -q "verdict: PASS"; then
        ok "oracle PASS with 4 observer mods enabled"
    else
        bad "oracle did not PASS with observers enabled"
        printf '%s\n' "$out" | grep -E "verdict|hard_fail|diverg" | head -3
    fi
    if printf '%s\n' "$out" | grep -q "raw probe #"; then
        ok "raw-probe fired"
    else
        bad "raw-probe never fired"
    fi

    say "arena-canary: armed, saved and restored on $PG_GAME"
    fixture "$WORK/canary-$PG_GAME" arena-canary
    st="$WORK/canary-$PG_GAME.state"
    out="$(RUN_OUTS="$st" run_game "$WORK/canary-$PG_GAME" $((5000 + KEY0)) \
            --state-save-on-frame $((4500 + KEY0)) --state-save-output "$st")"
    if printf '%s\n' "$out" | grep -q "arena-canary: armed" &&
       printf '%s\n' "$out" | grep -q "savestate: saved"; then
        ok "canary armed; savestate written"
    else
        bad "canary not armed or savestate not written"
    fi
    out="$(run_game "$WORK/canary-$PG_GAME" $((6000 + KEY0)) --state-load-on-frame $((5000 + KEY0)) \
            --state-load-input "$st")"
    if printf '%s\n' "$out" | grep -q "arena ledger applied: .* 9502784 B restored" &&
       printf '%s\n' "$out" | grep -q "savestate: loaded" &&
       ! printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
        ok "file load restored the three arena ranges (9502784 B), no DESYNC"
    else
        bad "file load: arena not restored, or DESYNC"
        printf '%s\n' "$out" | grep -E 'arena|savestate' | head -5
    fi
fi

# ── M9b. (Spyro 2 and 3) the render-surface mods ─────────────────────────
# declarative-reshade, moby-instance-tint, channel-gallery and late-pipes
# each repaint pixels at a pin from the game's own corpus where terrain,
# mobys, the player and the flame are on screen, against a baseline with
# no mod; a control leg repeats the baseline, so a nonzero count there
# voids the others. imgui-panel's per-game tick arm and moby array walk
# count live mobys on the attract demo with the oracle at PASS.
if [ "$PG_GAME" != "spyro-1" ]; then
    case "$PG_GAME" in
        spyro-2) P7_SCENE=shady-oasis-flame; P7_PIN=1421 ;;
        spyro-3) P7_SCENE=crystal-islands-flame; P7_PIN=1653 ;;
    esac
    read -ra P7_ARGS < <(grep -v '^#' "$PG_BK2_DIR/$P7_SCENE.bk2.args" | tr '\n' ' ')
    # p7_leg <tag> <mod>...: copies of the named mods (no .build/, so each
    # compiles for this game) and one screenshot at the pin. A mod's
    # config.toml is written by the caller into $WORK/p7-<tag>/<mod>/.
    p7_stage() {
        local dir="$WORK/p7-$1"; shift
        rm -rf "${dir:?}"
        mkdir -p "$dir"
        : > "$dir/enabled.txt"
        local m
        for m in "$@"; do
            cp -r "$MODS_SRC/$m" "$dir/$m"
            rm -rf "${dir:?}/$m/.build" "${dir:?}/$m/config.toml"
            echo "$m" >> "$dir/enabled.txt"
        done
    }
    p7_shoot() {
        local tag="$1" png="$WORK/p7-$1-$P7_PIN-vk.png"
        RUN_OUTS="$png" RUN_TIMEOUT=900 run_game "$WORK/p7-$tag" $((P7_PIN + 2)) \
            --renderer vk --mute 1 --aspect 4:3 --lod original \
            --bk2 "$PG_BK2_DIR/$P7_SCENE.bk2" "${P7_ARGS[@]}" \
            --screenshot-native-only --screenshot-on-frame "$P7_PIN" \
            --screenshot-output "$WORK/p7-$tag-{n}.png"
    }
    p7_diffpx() {
        python3 - "$1" "$2" <<'PY_P7'
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
print(sum(1 for p in ImageChops.difference(a, b).getdata() if p != (0, 0, 0)))
PY_P7
    }
    say "render-surface mods: repaint at $P7_SCENE VBL $P7_PIN on $PG_GAME"
    p7_stage base
    p7_stage ctl
    p7_stage reshade declarative-reshade
    p7_stage tint moby-instance-tint
    p7_stage gallery channel-gallery
    printf 'gallery.rows = "all"\n' > "$WORK/p7-gallery/channel-gallery/config.toml"
    p7_stage late late-pipes
    p7_base="$WORK/p7-base-$P7_PIN-vk.png"
    p7_shoot base > "$WORK/p7-base.log" 2>&1
    for tag in ctl reshade tint gallery late; do
        out="$(p7_shoot "$tag")"; rc=$?
        if [ "$rc" -ne 0 ] || [ ! -s "$p7_base" ] || [ ! -s "$WORK/p7-$tag-$P7_PIN-vk.png" ]; then
            bad "$tag: no capture (rc=$rc)"
            printf '%s\n' "$out" | grep -iE "error|refused|fail" | head -4
            continue
        fi
        n="$(p7_diffpx "$p7_base" "$WORK/p7-$tag-$P7_PIN-vk.png")"
        case "$tag" in
            ctl)
                if [ "${n:-1}" -eq 0 ]; then ok "control: the baseline repeats exactly"
                else bad "control: the baseline differs by ${n:-?} px; the counts are void"; fi
                continue ;;
            reshade) want="declarative-reshade: 3 declarative material row(s)" ;;
            tint)    want="loaded moby-instance-tint " ;;
            gallery) want="21 selector rows armed, 21 lit" ;;
            late)    want="late-pipes: registered 3 postfx passes, 2 materials" ;;
        esac
        if ! printf '%s\n' "$out" | grep -q "$want"; then
            bad "$tag: log line missing: $want"
            printf '%s\n' "$out" | grep -iE "refused|$tag" | head -4
        elif printf '%s\n' "$out" | grep -qE "row '.*' refused|selector inert|failed to build"; then
            bad "$tag: a row or shader was refused"
            printf '%s\n' "$out" | grep -E "refused|inert|failed" | head -4
        elif [ "${n:-0}" -gt 0 ]; then
            ok "$tag: $n px repainted"
        else
            bad "$tag: 0 px repainted"
        fi
    done

    # The gallery's moby refine row alone (row 19, every class) keyed on the
    # game's moby state byte (gallery.refine_state; Spyro 2 offset 0x48, Spyro
    # 3 the typed Moby view). At the pin the refine candidates hold states 0,
    # 1 and others on both games, so state P7_STATE selects a strict subset:
    # it repaints more than 0 px and fewer than the claim-all leg (-1, which
    # skips the byte read), and state 250, held by no moby, repaints nothing.
    # A wrong offset reads another byte and fails one of the two bounds.
    say "gallery refine row keyed on the moby state byte on $PG_GAME"
    P7_STATE=1
    for leg in all:-1 state:$P7_STATE none:250; do
        tag="ref${leg%%:*}"
        p7_stage "$tag" channel-gallery
        printf 'gallery.rows = "19"\ngallery.class = -1\ngallery.refine_state = %s\n' \
            "${leg#*:}" > "$WORK/p7-$tag/channel-gallery/config.toml"
        out="$(p7_shoot "$tag")"; rc=$?
        if [ "$rc" -ne 0 ] || [ ! -s "$WORK/p7-$tag-$P7_PIN-vk.png" ] ||
           ! printf '%s\n' "$out" | grep -q "21 selector rows armed, 1 lit"; then
            bad "$tag: no capture (rc=$rc) or the refine row did not light"
            printf '%s\n' "$out" | grep -iE "error|refused|lit" | head -4
            continue
        fi
        printf -v "p7_$tag" '%s' "$(p7_diffpx "$p7_base" "$WORK/p7-$tag-$P7_PIN-vk.png")"
    done
    if [ "${p7_refstate:-0}" -gt 0 ] && [ "${p7_refstate:-0}" -lt "${p7_refall:-0}" ] &&
       [ "${p7_refnone:-1}" -eq 0 ]; then
        ok "refine state $P7_STATE: ${p7_refstate} px, a strict subset of any state ${p7_refall} px; state 250: 0 px"
    else
        bad "refine state $P7_STATE: ${p7_refstate:-?} px (want 0 < n < any state ${p7_refall:-?} px), state 250: ${p7_refnone:-?} px (want 0)"
    fi
    unset p7_refall p7_refstate p7_refnone

    say "imgui-panel: the tick arm samples the moby array on $PG_GAME"
    fixture "$WORK/imgui-$PG_GAME" imgui-panel
    out="$(OPENPETE_MOD_LINK_CALLS=1 OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES \
            tests/harness/check_psx_oracle.sh --game "$PG_GAME" -- \
            --mods-dir "$WORK/imgui-$PG_GAME" 2>&1)"
    n="$(link_calls "$out" imgui-panel)"
    live="$(printf '%s\n' "$out" |
        sed -n 's/.*imgui-panel: hovered [0-9]*\/[0-9]* presents, \([0-9]*\) live mobys.*/\1/p' |
        sort -n | tail -1)"
    if printf '%s\n' "$out" | grep -q "verdict: PASS" &&
       printf '%s\n' "$out" | grep -q "imgui-panel: ImGui bound" &&
       [ "$n" -gt 0 ] && [ "${live:-0}" -gt 0 ] && [ "${live:-0}" -lt 4096 ]; then
        ok "imgui-panel: oracle PASS, $n tick calls, at most $live live mobys in a sample"
    else
        bad "imgui-panel: oracle, bind, $n tick calls or ${live:-no} live mobys"
        printf '%s\n' "$out" | grep -E "verdict|imgui-panel" | head -5
    fi
fi

# ── M10. (Spyro 3) the anti-piracy scanned-range watch and mod guard ─────
# Spyro 3's loading, title and level scans hash EXE .text
# 0x8001200C..0x80064F9C and the resident overlay's text. The mod guard
# restores a byte a mod changed there through a guest() pointer, naming the
# mod; the store watch (OPENPETE_S3_SCAN_WATCH=1) reports a store there and
# names its writer. One run carries every rung. scan-probe overrides the
# per-tick Draw and on its 300th call, after base(), stores the complement
# of the word at 0x8001200C and of the byte at 0x8001200B (one below the
# range) through op_write_u32/op_write_u8; the next call reads both back:
# the guard must restore the word and leave the byte. At VBL 1473
# OPENPETE_S3_SCAN_POKE stores an EXE word from the host fiber: the watch
# must report one host poke, and that poke fails the next loading walk, whose
# verdict must name scan-probe while the game's own zeroing store is
# counted apart. Over the level load no recompiler store may be reported:
# the level overlay's decryption pass is the load and is counted apart.
# A second run under --scan-guard 0: boot logs the guard off once, the
# probe's store at 0x8001200C persists, no restore is logged, the guard
# makes no compare, and its cost per stretch falls to the bracket alone.
# On Spyro 1 and 2 --scan-guard 0 is accepted and changes nothing: the run
# exits 0 and logs no [s3-scan] line.
if [ "$PG_GAME" = "spyro-3" ]; then
    scan_count() { # scan_count <log> <key> -> the key's value on an [s3-scan] exit line
        printf '%s\n' "$1" | grep -E '^\[s3-scan\] (watch|guard|checksum)' |
            grep -oE "(^| |\()$2=[0-9]+" | head -1 | cut -d= -f2
    }
    mkdir -p "$WORK/scanprobe-src/scan-probe/src"
    mod_toml "$WORK/scanprobe-src/scan-probe" scan-probe 'game = "spyro-3"'
    cat > "$WORK/scanprobe-src/scan-probe/src/probe.c" <<'CEOF'
#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api;
static openpete_mod_t* g_self;
static unsigned s_calls;
static uint32_t s_word;
static uint8_t  s_byte;
static void tick(CPUState* cpu) {
    g_api->base(cpu);
    if (++s_calls == 300u) {
        s_word = op_read_u32(g_api, 0x8001200Cu);
        s_byte = op_read_u8(g_api, 0x8001200Bu);
        op_write_u32(g_api, 0x8001200Cu, ~s_word);
        op_write_u8(g_api, 0x8001200Bu, (uint8_t)~s_byte);
        g_api->log(g_self, OP_MOD_LOG_INFO, "scan-probe: stored");
    } else if (s_calls == 301u) {
        uint32_t w = op_read_u32(g_api, 0x8001200Cu);
        uint8_t  b = op_read_u8(g_api, 0x8001200Bu);
        g_api->log(g_self, OP_MOD_LOG_INFO, "scan-probe: in-range %s, below %s",
                   w == s_word ? "restored" : "CHANGED",
                   b == (uint8_t)~s_byte ? "landed" : "LOST");
        op_write_u8(g_api, 0x8001200Bu, s_byte);
    }
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    return api->override_name(self, "Draw", tick);
}
CEOF
    mkdir -p "$WORK/scanprobe"
    ln -sfn "$WORK/scanprobe-src/scan-probe" "$WORK/scanprobe/scan-probe"
    echo scan-probe > "$WORK/scanprobe/enabled.txt"

    say "scan watch: mod guard restore, a store below the range, a host poke and the overlay load on $PG_GAME"
    out="$( PG_CAPTURE_ENV=(OPENPETE_S3_SCAN_WATCH=1 OPENPETE_S3_SCAN_POKE=1473:0x80030000:0x13579BDF)
            RUN_TIMEOUT=600 run_game "$WORK/scanprobe" 3573 )"
    n_restore="$(scan_count "$out" restores)"
    n_stretch="$(scan_count "$out" stretches)"
    n_host="$(scan_count "$out" host)"
    n_rec="$(scan_count "$out" recompiler)"
    n_decrypt="$(scan_count "$out" decrypt)"
    n_fail="$(scan_count "$out" fail)"
    n_vstore="$(scan_count "$out" verdict-store)"
    if printf '%s\n' "$out" | grep -q "guard: mod 'scan-probe' stored into scanned range exe: 4 byte(s) from 0x8001200C restored" &&
       printf '%s\n' "$out" | grep -q "scan-probe: in-range restored" &&
       [ "${n_restore:-0}" -eq 1 ]; then
        ok "scan-probe: 4 bytes at 0x8001200C restored and named; $n_restore restore over $n_stretch guarded stretches"
    else
        bad "scan-probe: restore not logged or the word not restored (restores=${n_restore:-0}, stretches=${n_stretch:-0})"
        printf '%s\n' "$out" | grep -E "s3-scan|scan-probe" | head -6
    fi
    if printf '%s\n' "$out" | grep -q "scan-probe: in-range .*, below landed"; then
        ok "scan-probe: the store at 0x8001200B landed (1 byte, outside the range)"
    else
        bad "scan-probe: the store one byte below the range did not land"
    fi
    if printf '%s\n' "$out" | grep -q "FAIL watch: store of 4 byte(s) at 0x80030000 by host poke into scanned range exe" &&
       [ "${n_host:-0}" -eq 1 ]; then
        ok "host poke at 0x80030000 reported ($n_host host hit)"
    else
        bad "host poke not reported (host=${n_host:-0})"
        printf '%s\n' "$out" | grep -E "s3-scan" | head -6
    fi
    if [ "${n_fail:-0}" -eq 1 ] && [ "${n_vstore:-0}" -eq 1 ] &&
       printf '%s\n' "$out" | grep -q "loading walk failed with mods active: scan-probe"; then
        ok "the poked walk failed once naming scan-probe; the game's zeroing store counted apart ($n_vstore)"
    else
        bad "poked walk: fail=${n_fail:-0} verdict-store=${n_vstore:-0}, mods named: $(printf '%s\n' "$out" | grep -o 'mods active: .*' | head -1)"
    fi
    if [ "${n_decrypt:-0}" -gt 0 ] && [ "${n_rec:-1}" -eq 0 ]; then
        ok "overlay load not reported: $n_decrypt decryption-pass stores counted apart, 0 recompiler hits"
    else
        bad "overlay load: decrypt=${n_decrypt:-0} recompiler=${n_rec:-?} (want >0 and 0)"
    fi
    n_cmp_on="$(scan_count "$out" compares)"
    cost_on="$(printf '%s\n' "$out" | grep -oE 'cost=[0-9.]+ us/stretch' | head -1)"

    say "scan guard off: --scan-guard 0 lets the probe's store persist on $PG_GAME"
    out="$( PG_CAPTURE_ENV=(OPENPETE_S3_SCAN_WATCH=1)
            RUN_TIMEOUT=600 run_game "$WORK/scanprobe" 2073 --scan-guard 0 )"
    n_off_lines="$(printf '%s\n' "$out" | grep -c 'scan guard off (--scan-guard 0)')"
    n_restore="$(scan_count "$out" restores)"
    n_stretch="$(scan_count "$out" stretches)"
    n_cmp="$(scan_count "$out" compares)"
    cost_off="$(printf '%s\n' "$out" | grep -oE 'cost=[0-9.]+ us/stretch' | head -1)"
    if [ "$n_off_lines" -eq 1 ] &&
       printf '%s\n' "$out" | grep -q "scan-probe: in-range CHANGED, below landed" &&
       ! printf '%s\n' "$out" | grep -q "guard: mod '" &&
       [ "${n_restore:--1}" -eq 0 ] && [ "${n_cmp:--1}" -eq 0 ] && [ "${n_stretch:-0}" -gt 0 ]; then
        ok "guard off: store at 0x8001200C persisted, 0 restores, 0 compares over $n_stretch stretches ($cost_off; on: ${n_cmp_on:-?} compares, $cost_on)"
    else
        bad "guard off: off-lines=$n_off_lines restores=${n_restore:-?} compares=${n_cmp:-?} stretches=${n_stretch:-?}"
        printf '%s\n' "$out" | grep -E "s3-scan|scan-probe" | head -6
    fi
else
    say "scan guard: --scan-guard 0 is accepted and inert on $PG_GAME"
    if out="$(run_game "$WORK/noscan-empty" $((200 + KEY0)) --scan-guard 0)" &&
       ! printf '%s\n' "$out" | grep -q '\[s3-scan\]'; then
        ok "--scan-guard 0 accepted on $PG_GAME, no [s3-scan] line"
    else
        bad "--scan-guard 0 on $PG_GAME: refused or logged an [s3-scan] line"
        printf '%s\n' "$out" | grep -E "scan|error" | head -4
    fi
fi
