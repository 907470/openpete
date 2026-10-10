# mods_sdk_rungs.sh - the per-game mod SDK and object identity rungs of
# tests/harness/check_mods.sh. SOURCED by it, after its helpers (say, ok,
# bad, fixture-free run_game, WORK, BIN, API_VER, PG_GAME); runs for every
# game, where the rest of check_mods.sh is Spyro 1's corpus.
#
# Rungs:
#   sdk tree       the staged <exe>/sdk/ and the build's mod-sdk-test/sdk/
#                  hold the game-neutral root and games/<game>/ with
#                  openpete_game.h; a source mod compiles against them,
#                  reads its game's macros and records its game.
#   header edit    an edit to a per-game header rebuilds a mod that
#                  includes it at the next boot and during hot reload.
#   --compile-mods OPENPETE_MOD_SDK_DIR points --compile-mods at the test
#                  SDK directory; each staged game gets its own pass.
#   wrappers       (spyro-1) the overlay-qualified and raw wrapper aliases
#                  of TitlescreenDraw compile and register at 0x8007CEE4.
#   identity       hand-built objects in every lookup path: an object built
#                  for another game, a "*" object, a legacy object, and the
#                  `#error` of a "*" compile that reads a per-game header.
#   two games      a mod naming two games compiles one object per game,
#                  each under its own .build/<game>/ and log label.
#   windows        the same fixtures cross-built as DLLs export
#                  openpete_mod_entry, openpete_mod_abi and, where built
#                  with a game, openpete_mod_game (skips with no mingw).

# The other game a rung pairs with the running one.
sdk_other_game() { [ "$PG_GAME" = "spyro-1" ] && echo spyro-3 || echo spyro-1; }

# run_bin <bin> <mods_dir> <frames> [flags...]: run_game with another binary.
run_bin() {
    local b="$1"; shift
    ( PG_BIN="$b"; run_game "$@" )
}

# Binary of another game, resolved the way the harness resolves its own.
# OPENPETE_BIN and OPENPETE_DISC name the running game's binary and disc
# (ctest sets them), so they are dropped for the other game.
other_bin() {
    ( unset OPENPETE_BIN OPENPETE_DISC; pg_resolve "$1" >/dev/null 2>&1
      echo "$PG_BIN" )
}

# run_other <game> <mods_dir> <frames> [flags...]: a headless run of another
# game with that game's disc and boot flags.
run_other() {
    local g="$1"; shift
    ( unset OPENPETE_BIN OPENPETE_DISC; pg_resolve "$g"; run_game "$@" )
}

# mod_toml <dir> <id> [game-line]: a manifest pinned at the current api.
mod_toml() {
    mkdir -p "$1"
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = %s\n%s' \
        "$2" "$API_VER" "${3:+$3$'\n'}" > "$1/mod.toml"
}

SDK_ROOT="$(dirname "$BIN")/sdk"
SDK_GAME_DIR="$SDK_ROOT/games/$PG_GAME"
TEST_SDK="${PG_BUILD_DIR:-build/linux-release}/mod-sdk-test/sdk"
CC_MOD="${OPENPETE_MOD_CC:-cc}"

# ── S1. per-game SDK tree: staged layout, compile, game macros ───────────
say "sdk tree: <exe>/sdk/ and mod-sdk-test/sdk/ carry games/$PG_GAME/"
n_tree=0
for f in "$SDK_ROOT/openpete_mod_api.h" "$SDK_ROOT/openpete_mod_abi.c" \
         "$SDK_GAME_DIR/openpete_game.h" "$SDK_GAME_DIR/openpete_sdk_symbols.h" \
         "$SDK_GAME_DIR/openpete_sdk_wrappers.h" \
         "$TEST_SDK/openpete_mod_api.h" "$TEST_SDK/games/$PG_GAME/openpete_game.h"; do
    if [ -f "$f" ]; then n_tree=$((n_tree + 1)); else bad "missing $f"; fi
done
[ -f "$SDK_ROOT/openpete_game.h" ] && bad "openpete_game.h staged at the sdk root"
[ "$n_tree" -eq 7 ] && ok "7/7 staged files present, no root openpete_game.h"

# The probe reads its game's macros through the api header alone and the
# typed structs when the game has them; its log line is the engagement.
mkdir -p "$WORK/sdkprobe-src/sdk-probe/src"
mod_toml "$WORK/sdkprobe-src/sdk-probe" sdk-probe "game = \"$PG_GAME\""
cat > "$WORK/sdkprobe-src/sdk-probe/src/probe.c" <<'CEOF'
#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#if __has_include("openpete_game_structs.h")
#include "openpete_game_structs.h"
#endif
#if OPENPETE_GAME_SPYRO1
#define PROBE_ARM "spyro1"
#elif OPENPETE_GAME_SPYRO2
#define PROBE_ARM "spyro2"
#elif OPENPETE_GAME_SPYRO3
#define PROBE_ARM "spyro3"
#else
#error "no arm for this game"
#endif
/* PROBE-REV 1 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    api->log(self, OP_MOD_LOG_INFO, "sdk-probe: game=%s arm=%s",
             OPENPETE_GAME_ID, PROBE_ARM);
    return 0;
}
CEOF
mkdir -p "$WORK/sdkprobe"
ln -sfn "$WORK/sdkprobe-src/sdk-probe" "$WORK/sdkprobe/sdk-probe"
echo sdk-probe > "$WORK/sdkprobe/enabled.txt"
out="$(run_game "$WORK/sdkprobe" 300)"
arm="${PG_GAME//-/}"
if printf '%s\n' "$out" | grep -q "sdk-probe: game=$PG_GAME arm=$arm" &&
   [ -f "$WORK/sdkprobe-src/sdk-probe/.build/$PG_GAME/sdk-probe.so" ] &&
   printf '%s\n' "$out" | grep -q "sdk-probe \[$PG_GAME\]: compiling"; then
    ok "compiled under .build/$PG_GAME/, game=$PG_GAME arm=$arm"
else
    bad "sdk-probe did not compile and report $PG_GAME"
    printf '%s\n' "$out" | grep -E 'sdk-probe|error' | head -5
fi

# ── S2. per-game header edit: rebuild at the next boot ───────────────────
# A sandbox binary directory: a hard link of the binary beside a copy of
# its sdk/, so the edit never touches the build tree's staged SDK.
say "header edit: games/$PG_GAME/ header change rebuilds at the next boot"
BOOT="$WORK/bootbin"
mkdir -p "$BOOT"
ln "$(realpath "$BIN")" "$BOOT/" 2>/dev/null || cp "$(realpath "$BIN")" "$BOOT/"
[ -d "$(dirname "$BIN")/assets" ] && ln -sfn "$(realpath "$(dirname "$BIN")/assets")" "$BOOT/assets"
cp -r "$SDK_ROOT" "$BOOT/sdk"
BOOT_BIN="$BOOT/$(basename "$BIN")"
EDIT_HDR="$BOOT/sdk/games/$PG_GAME/openpete_sdk_symbols.h"
compiles() { printf '%s\n' "$1" | grep -c "sdk-probe \[$PG_GAME\]: compiling" || true; }
# The sandbox SDK has the staged content, so the first boot hits the
# object S1 compiled (c1 0); c1 is reported, not asserted.
c1="$(compiles "$(run_bin "$BOOT_BIN" "$WORK/sdkprobe" 120)")"
c2="$(compiles "$(run_bin "$BOOT_BIN" "$WORK/sdkprobe" 120)")"
printf '\n/* header edit %s */\n' "$$" >> "$EDIT_HDR"
c3="$(compiles "$(run_bin "$BOOT_BIN" "$WORK/sdkprobe" 120)")"
if [ "$c2" -eq 0 ] && [ "$c3" -eq 1 ]; then
    ok "compiles per boot: $c1 (sandbox key), $c2 (unchanged), $c3 (after the edit)"
else
    bad "compiles per boot: $c1, $c2, $c3 (want x, 0, 1)"
fi

# ── S3. per-game header edit: rebuild during hot reload ──────────────────
say "header edit: games/$PG_GAME/ header change hot-reloads a running mod"
# pg_capture writes the log as the run goes, so the edit waits for the load.
( PG_BIN="$BOOT_BIN"; PG_CAPTURE_ENV=(OPENPETE_MOD_HOT_RELOAD=1)
  PG_CAPTURE_TIMEOUT=300
  pg_capture "$WORK/sdk-hot.log" "" -- --frame-limit 4000 \
      --mods-dir "$WORK/sdkprobe" ) &
hotpid=$!
for _ in $(seq 1 1200); do
    grep -q "loaded sdk-probe" "$WORK/sdk-hot.log" 2>/dev/null && break
    sleep 0.1
done
sleep 1
printf '\n/* hot edit %s */\n' "$$" >> "$EDIT_HDR"
wait "$hotpid"
if grep -q "hot reload: sdk-probe (source change)" "$WORK/sdk-hot.log" &&
   grep -q "hot reload: sdk-probe live" "$WORK/sdk-hot.log" &&
   [ "$(grep -c "sdk-probe: game=$PG_GAME" "$WORK/sdk-hot.log")" -ge 2 ]; then
    ok "reload fired on the header edit; entry ran $(grep -c "sdk-probe: game=$PG_GAME" "$WORK/sdk-hot.log") times"
else
    bad "no hot reload on the header edit (see $WORK/sdk-hot.log)"
    grep -E "hot reload|sdk-probe" "$WORK/sdk-hot.log" | head -5
fi

# ── S4. --compile-mods against the test SDK directory ───────────────────
# The test SDK stages every game; --compile-mods makes one pass per staged
# game, and sdk-probe (game = this game) compiles in this game's pass only.
say "--compile-mods: OPENPETE_MOD_SDK_DIR selects mod-sdk-test/sdk/, one pass per staged game"
rm -rf "${WORK:?}/sdkprobe-src/sdk-probe/.build"
out="$( PG_CAPTURE_ENV=(OPENPETE_MOD_SDK_DIR="$(realpath "$TEST_SDK")")
        run_game "$WORK/sdkprobe" 1 --compile-mods )"
n_staged=0
for g in spyro-1 spyro-2 spyro-3; do
    printf '%s\n' "$out" | grep -q -- "--compile-mods: staged $g " && n_staged=$((n_staged + 1))
done
n_objs="$(find "$WORK/sdkprobe-src/sdk-probe/.build" -name 'sdk-probe.so' | wc -l)"
if printf '%s\n' "$out" | grep -q "SDK $(realpath "$TEST_SDK") (OPENPETE_MOD_SDK_DIR)" &&
   [ "$n_staged" -eq 3 ] &&
   printf '%s\n' "$out" | grep -q -- "--compile-mods \[$PG_GAME\]: 1 compiled" &&
   [ "$n_objs" -eq 1 ] &&
   [ -f "$WORK/sdkprobe-src/sdk-probe/.build/$PG_GAME/sdk-probe.so" ]; then
    ok "3 games staged; 1 compiled against the test SDK, into .build/$PG_GAME/ only"
else
    bad "--compile-mods: staged $n_staged of 3, objects $n_objs (want 1 under .build/$PG_GAME/)"
    printf '%s\n' "$out" | grep -E 'compile-mods|SDK' | head -8
fi

# ── S5. (spyro-1) overlay and raw wrapper aliases register ───────────────
if [ "$PG_GAME" = "spyro-1" ]; then
    say "wrappers: func_titlescreen_8007CEE4 and func_8007CEE4 register at 0x8007CEE4"
    for alias in func_titlescreen_8007CEE4 func_8007CEE4; do
        d="$WORK/wrapsrc/wrap-$alias"
        mkdir -p "$d/src"
        mod_toml "$d" "wrap-$alias" 'game = "spyro-1"'
        cat > "$d/src/wrap.c" <<CEOF
#include <openpete_mod_api.h>
#include <openpete_sdk_wrappers.h>
OP_OVERRIDE_IMPL_$alias(draw_hook) { op_base_$alias(cpu); }
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    return OP_REGISTER_OVERRIDE_$alias(api, self, draw_hook);
}
CEOF
        mkdir -p "$WORK/wrap"
        ln -sfn "$d" "$WORK/wrap/wrap-$alias"
        echo "wrap-$alias" >> "$WORK/wrap/enabled.txt"
    done
    out="$(run_game "$WORK/wrap" 300)"
    n_reg="$(printf '%s\n' "$out" | grep -c 'mod:wrap-func_.*override registered at 0x8007CEE4' || true)"
    if [ "$n_reg" -eq 2 ]; then
        ok "2/2 wrapper fixtures registered at 0x8007CEE4"
    else
        bad "$n_reg/2 wrapper fixtures registered"
        printf '%s\n' "$out" | grep -E 'wrap-func' | head -6
    fi
fi

# ── S6. object identity: every lookup path, both directions ──────────────
OTHER="$(sdk_other_game)"
OTHER_SDK="$(dirname "$(other_bin "$OTHER")")/sdk"
say "identity: objects for $PG_GAME, $OTHER, every game and no game"
mkdir -p "$WORK/idsrc"
cat > "$WORK/idsrc/body.c" <<'CEOF'
#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api;
static void pass_through(CPUState* cpu) { g_api->base(cpu); }
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
#ifdef ID_TARGETS
    /* GamestateUpdate's Spyro 1 address and raw alias; the override only
     * calls base(). */
    (void)api->override_addr(self, 0x8003385Cu, pass_through);
    (void)api->override_name(self, "func_8003385C", pass_through);
#else
    (void)pass_through;
#endif
    api->log(self, OP_MOD_LOG_INFO, "ID PROBE ENTRY RAN");
    return 0;
}
#ifdef ID_DLLEXPORT
/* One explicit export, which turns the MinGW linker's export of every
 * other global off unless the line passes --export-all-symbols. */
__declspec(dllexport) int id_marked_export(void) { return 1; }
#endif
CEOF
# id_obj <id> <subdir or ""> <cc flags...>: a binary-only mod whose object
# sits at .build/<subdir><id>.so
id_obj() {
    local id="$1" sub="$2"; shift 2
    local d="$WORK/id/$id"
    mod_toml "$d" "$id"
    mkdir -p "$d/.build/$sub"
    "$CC_MOD" -shared -fPIC -O2 -fno-strict-aliasing -Wall -Wundef "$@" \
        -o "$d/.build/$sub$id.so" "$WORK/idsrc/body.c" \
        > "$WORK/id-$id.cc.log" 2>&1 || bad "identity fixture $id did not compile"
    echo "$id" >> "$WORK/id/enabled.txt"
}
mkdir -p "$WORK/id"; : > "$WORK/id/enabled.txt"
G_OWN=(-I"$SDK_GAME_DIR" -I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c")
G_OTHER=(-I"$OTHER_SDK/games/$OTHER" -I"$OTHER_SDK" "$OTHER_SDK/openpete_mod_abi.c")
G_ANY=(-DOPENPETE_MOD_ANY_GAME -DID_TARGETS -I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c")
G_NONE=(-I"$SDK_ROOT" "$SDK_ROOT/openpete_mod_abi.c")
id_obj id-own          "$PG_GAME/" "${G_OWN[@]}"
id_obj id-other        "$PG_GAME/" "${G_OTHER[@]}"
id_obj id-other-legacy ""          "${G_OTHER[@]}"
id_obj id-any          "any/"      "${G_ANY[@]}"
id_obj id-any-ingame   "$PG_GAME/" "${G_ANY[@]}"
id_obj id-own-inany    "any/"      "${G_OWN[@]}"
id_obj id-own-legacy   ""          "${G_OWN[@]}"
id_obj id-legacy       ""          "${G_NONE[@]}"
out="$(run_game "$WORK/id" 600)"
printf '%s\n' "$out" > "$WORK/id.log"
# expect <id> load|refuse [pattern]
id_expect() {
    local id="$1" want="$2" pat="${3:-}"
    local loaded=0 refused=0
    grep -q "loaded $id v" "$WORK/id.log" && loaded=1
    grep -q "mods: $id: .*- disabled" "$WORK/id.log" && refused=1
    if [ "$want" = load ] && [ $loaded -eq 1 ] && [ $refused -eq 0 ]; then
        ok "$id loads"; n_id=$((n_id + 1))
    elif [ "$want" = refuse ] && [ $loaded -eq 0 ] && [ $refused -eq 1 ] &&
         { [ -z "$pat" ] || grep -q "mods: $id: .*$pat" "$WORK/id.log"; }; then
        ok "$id refused: $(grep -m1 "mods: $id: .*- disabled" "$WORK/id.log" | sed 's/.*mods: //')"
        n_id=$((n_id + 1))
    else
        bad "$id: want $want, loaded=$loaded refused=$refused"
        grep -E "$id" "$WORK/id.log" | head -3
    fi
}
n_id=0
id_expect id-own          load
id_expect id-other        refuse "records game $OTHER, not the running $PG_GAME"
id_expect id-other-legacy refuse "records game $OTHER, not the running $PG_GAME"
id_expect id-any          load
id_expect id-any-ingame   refuse "records game \\*, not the running $PG_GAME"
id_expect id-own-inany    refuse "records game $PG_GAME, but .build/any/ holds"
id_expect id-own-legacy   load
if [ "$PG_GAME" = "spyro-1" ]; then
    id_expect id-legacy load
else
    id_expect id-legacy refuse "built before per-game SDKs"
fi
n_entries="$(grep -c 'ID PROBE ENTRY RAN' "$WORK/id.log" || true)"
n_any_warn="$(grep -c "mod:id-any.*from a mod built for every game.*on $PG_GAME" "$WORK/id.log" || true)"
# id-own, id-any, id-own-legacy, and on spyro-1 id-legacy.
want_entries=3; [ "$PG_GAME" = "spyro-1" ] && want_entries=4
if [ "$n_id" -eq 8 ] && [ "$n_entries" -eq "$want_entries" ] && [ "$n_any_warn" -eq 2 ]; then
    ok "8/8 rows as the lookup table says; $n_entries entries ran; \"*\" warnings: $n_any_warn (override_addr, override_name func_)"
else
    bad "rows $n_id/8, entries ran $n_entries (want $want_entries), \"*\" warnings $n_any_warn (want 2)"
fi

# A "*" compile that reads a per-game header stops at its #error: through
# the per-game include path (openpete_mod_api.h finds openpete_game.h), and
# on spyro-1 through a root copy of openpete_game_structs.h.
n_err=0
if ! "$CC_MOD" -fsyntax-only -DOPENPETE_MOD_ANY_GAME -I"$SDK_GAME_DIR" -I"$SDK_ROOT" \
        "$WORK/idsrc/body.c" > "$WORK/any-pergame.log" 2>&1 &&
   grep -q '#error "openpete_game.h is per-game' "$WORK/any-pergame.log"; then
    n_err=$((n_err + 1))
else
    bad "\"*\" compile with games/$PG_GAME/ on the path did not stop at #error"
fi
want_err=1
if [ "$PG_GAME" = "spyro-1" ]; then
    want_err=2
    printf '#include <openpete_mod_api.h>\n#include "openpete_game_structs.h"\n' \
        > "$WORK/idsrc/structs.c"
    if ! "$CC_MOD" -fsyntax-only -DOPENPETE_MOD_ANY_GAME -I"$SDK_ROOT" \
            "$WORK/idsrc/structs.c" > "$WORK/any-root.log" 2>&1 &&
       grep -q '#error "openpete_game_structs.h is per-game' "$WORK/any-root.log"; then
        n_err=$((n_err + 1))
    else
        bad "\"*\" compile including the root openpete_game_structs.h did not stop at #error"
    fi
fi
[ "$n_err" -eq "$want_err" ] && ok "\"*\" with a per-game header: $n_err/$want_err compiles stopped at #error"

# ── S7. a two-game mod: one object per game, labelled per game ───────────
OTHER_BIN="$(other_bin "$OTHER")"
if [ -x "$OTHER_BIN" ]; then
    say "two games: game = [\"$PG_GAME\", \"$OTHER\"] builds one object per game"
    d="$WORK/twosrc/two-game"
    mkdir -p "$d/src"
    mod_toml "$d" two-game "game = [\"$PG_GAME\", \"$OTHER\"]"
    cat > "$d/src/two.c" <<'CEOF'
#include <openpete_mod_api.h>
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    api->log(self, OP_MOD_LOG_INFO, "two-game: built for %s", OPENPETE_GAME_ID);
    return 0;
}
CEOF
    mkdir -p "$WORK/two"
    ln -sfn "$d" "$WORK/two/two-game"
    echo two-game > "$WORK/two/enabled.txt"
    out_a="$(run_game "$WORK/two" 120)"
    out_b="$(run_other "$OTHER" "$WORK/two" 120)"
    if printf '%s\n' "$out_a" | grep -q "two-game \[$PG_GAME\]: compiling" &&
       printf '%s\n' "$out_b" | grep -q "two-game \[$OTHER\]: compiling" &&
       printf '%s\n' "$out_a" | grep -q "two-game: built for $PG_GAME" &&
       printf '%s\n' "$out_b" | grep -q "two-game: built for $OTHER" &&
       [ -f "$d/.build/$PG_GAME/two-game.so" ] && [ -f "$d/.build/$OTHER/two-game.so" ]; then
        ok "2 objects: .build/$PG_GAME/ and .build/$OTHER/, labels [$PG_GAME] and [$OTHER]"
    else
        bad "two-game mod did not build one object per game"
        printf '%s\n%s\n' "$out_a" "$out_b" | grep -E 'two-game' | head -6
    fi
else
    say "two games: SKIP - no $OTHER binary at $OTHER_BIN"
fi

# ── S8. windows: the fixture DLLs export what the loader resolves ────────
say "windows: fixture DLLs export entry, abi and the recorded game"
# The engine's Windows line (mod_compile) passes -Wl,--export-all-symbols.
# shellcheck source=scripts/lib/mingw.sh
source scripts/lib/mingw.sh
WCC="$(mingw_find_cc "$(mingw_spyro_root "$PWD")" 2>/dev/null || true)"
WOBJDUMP=""
for t in "$(dirname "${WCC:-/x}")/llvm-objdump" "$(dirname "${WCC:-/x}")/x86_64-w64-mingw32-objdump" \
         "$(command -v x86_64-w64-mingw32-objdump || true)"; do
    [ -x "$t" ] && { WOBJDUMP="$t"; break; }
done
if [ -z "$WCC" ] || [ -z "$WOBJDUMP" ]; then
    say "windows: SKIP - no mingw cross compiler or objdump"
else
    n_dll=0
    # wdll <name> <want game export 0|1> <flags...>
    wdll() {
        local name="$1" want="$2"; shift 2
        local dll="$WORK/win-$name.dll"
        rm -f "$dll"
        if ! "$WCC" -shared -Wl,--export-all-symbols -O2 -fno-strict-aliasing \
                -Wall -Wundef -static-libgcc "$@" \
                -o "$dll" "$WORK/idsrc/body.c" > "$WORK/win-$name.log" 2>&1; then
            bad "windows fixture $name did not compile"; return
        fi
        local ex; ex="$("$WOBJDUMP" -p "$dll" | awk '/Export Table:|Name Pointer\] Table/{f=1; next} f && /^[[:space:]]*$/{f=0} f')"
        local has_entry=0 has_abi=0 has_game=0
        printf '%s\n' "$ex" | grep -qw openpete_mod_entry && has_entry=1
        printf '%s\n' "$ex" | grep -qw openpete_mod_abi && has_abi=1
        printf '%s\n' "$ex" | grep -qw openpete_mod_game && has_game=1
        if [ $has_entry -eq 1 ] && [ $has_abi -eq 1 ] && [ $has_game -eq "$want" ]; then
            ok "$name.dll exports entry, abi$([ "$want" -eq 1 ] && echo ', game')"
            n_dll=$((n_dll + 1))
        else
            bad "$name.dll exports entry=$has_entry abi=$has_abi game=$has_game (want game=$want)"
        fi
    }
    wdll own    1 "${G_OWN[@]}"
    wdll other  1 "${G_OTHER[@]}"
    wdll any    1 "${G_ANY[@]}"
    wdll legacy 0 "${G_NONE[@]}"
    wdll own-marked 1 -DID_DLLEXPORT "${G_OWN[@]}"
    if ! "$WCC" -fsyntax-only -DOPENPETE_MOD_ANY_GAME -I"$SDK_GAME_DIR" -I"$SDK_ROOT" \
            "$WORK/idsrc/body.c" > "$WORK/win-any-pergame.log" 2>&1 &&
       grep -q 'openpete_game.h is per-game' "$WORK/win-any-pergame.log"; then
        n_dll=$((n_dll + 1)); ok "\"*\" cross-compile with games/$PG_GAME/ stops at #error"
    else
        bad "\"*\" cross-compile with games/$PG_GAME/ did not stop at #error"
    fi
    [ "$n_dll" -eq 6 ] && ok "windows: 6/6 export and #error checks ($(basename "$WCC"))"
fi
