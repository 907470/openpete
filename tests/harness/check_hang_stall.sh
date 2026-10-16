#!/usr/bin/env bash
# check_hang_stall.sh - the hang watchdog's stall monitor trips on a guest
# that runs without a VBL, and never on a run that takes its VBLs.
#
# The stall monitor (core/psx/dispatch.c) is a host thread that counts the
# wall time the game fiber runs with openpete_vbl unchanged and trips the
# hang dump (hang-trace.bin, hang-ram.bin, exit 2) at
# OPENPETE_HANG_STALL_SECONDS (default 30). Every run prints the census line
#   watchdog: stall monitor: <samples> samples, longest game-fiber run with
#   openpete_vbl unchanged <ms> ms at openpete_vbl=<v>; trip at <ms> ms
# at exit; a run that trips prints
#   watchdog: hang detected: game fiber ran <s> s wall with openpete_vbl=<v>
#   unchanged ..., taken|dumped off-thread ...
#
# Rows (Spyro 1):
#   spin    Linux and Windows. An inline mod busy-waits inside its CameraUpdate override
#           on the 200th call (over the load row's movie): the game fiber runs host code that enters no
#           guest function and polls no loop, so the request is not taken
#           and the monitor dumps off-thread.
#           Pass: exit 2, the trip line with "dumped off-thread", the stall
#           at most N + 3 s, both dump files written.
#   callspin  Linux and Windows. As spin, but the loop calls the guest's Cos
#           (0x80016CB0) through the dispatch layer, with
#           OPENPETE_HANG_THRESHOLD=4000000000 so the entry-count trip stays
#           out of reach: the game fiber takes the request at an entry.
#           Pass: as spin, the trip line with "taken at 0x80016CB0".
#   triple  Windows exe. Mods spyro-japanese-extras + cynder + sparx-swap
#           (community mods, not in the tree: copies under the directory
#           OPENPETE_HANG_STALL_MODS names; the row skips without it) over
#           games/spyro-1/bk2/level-transition-gems.bk2 stall the guest at
#           openpete_vbl 3482 in the loop at 0x8001FFF8, which polls on few
#           of its back-edges. Before the monitor the loop-quanta trip fired
#           at 8845 s wall.
#           Pass: exit 2, the trip line, the stall at most N + 3 s.
#   demo    Linux and Windows. No input, --frame-limit 20000: the title and
#           the attract demo's level loads and gameplay.
#   load    Linux and Windows. level-transition-gems.bk2 from
#           --skip-to-level artisans, --frame-limit 5350: the skip's
#           fast-forward and the level transitions.
#           Pass (demo, load): exit 0, samples > 0 (else VACUOUS), the
#           longest stretch below N.
#
# Outputs under <build-dir>/hang-stall/<row>-<platform>/, deleted before
# each run.
#
# Usage: tests/harness/check_hang_stall.sh [--build-dir=DIR] [--win-build-dir=DIR]
#            [--row=spin|callspin|triple|demo|load]... [--platform=linux|windows|both]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 every row skipped.
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/wslenv.sh
source scripts/lib/wslenv.sh

ROWS=()
PLATFORM=both
WIN_BUILD_DIR=build/windows-mingw
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --win-build-dir=*) WIN_BUILD_DIR="${a#--win-build-dir=}";;
        --row=*) ROWS+=("${a#--row=}");;
        --platform=*) PLATFORM="${a#--platform=}";;
        *) echo "hang-stall: unknown argument $a" >&2; exit 64;;
    esac
done
(( ${#ROWS[@]} )) || ROWS=(spin callspin triple demo load)

GAME=spyro-1
N_S=30
MOVIE=games/spyro-1/bk2/level-transition-gems.bk2
pg_resolve "$GAME"
OUT_ROOT="${PG_BUILD_DIR:-build/linux-release}/hang-stall"
WIN_EXE="$(win_tree_exe "$WIN_BUILD_DIR" "$GAME")"

# run_leg PLATFORM DIR TIMEOUT MODS ARGS...: one headless run; writes DIR/run.log
# and DIR/rc. MODS is a mods directory or "-" for none.
run_leg() {
    local plat="$1" dir="$2" to="$3" mods="$4"; shift 4
    rm -f "${dir:?}/run.log" "${dir:?}/rc" "${dir:?}/hang-trace.bin" "${dir:?}/hang-ram.bin"
    mkdir -p "$dir"
    local rc
    if [[ "$plat" == linux ]]; then
        local margs=()
        [[ "$mods" != - ]] && margs=(--mods-dir "$mods")
        env OPENPETE_VIBRATION=0 "${LEG_ENV[@]}" OPENPETE_HANG_TRACE="$dir/hang-trace.bin" \
            OPENPETE_HANG_RAM="$dir/hang-ram.bin" timeout -k 10 "$to" \
            "$PG_BIN" --game "$GAME" --headless --no-config --disc "$PG_DISC" \
            --no-log-file --mute 1 --vibration 0 --no-card "${margs[@]}" "$@" \
            </dev/null > "$dir/run.log" 2>&1
        rc=$?
    else
        local wargs=() a
        [[ "$mods" != - ]] && wargs=(--mods-dir "$(wslpath -w "$mods")")
        for a in "$@"; do
            if [[ -e "$a" ]]; then wargs+=("$(wslpath -w "$(realpath "$a")")"); else wargs+=("$a"); fi
        done
        wslenv_forward OPENPETE_VIBRATION=0 "${LEG_ENV[@]}" \
            OPENPETE_HANG_TRACE="$(wslpath -w "$dir")\\hang-trace.bin" \
            OPENPETE_HANG_RAM="$(wslpath -w "$dir")\\hang-ram.bin"
        env "${WSLENV_ARGS[@]}" timeout -k 10 "$to" "$WIN_EXE" --game "$GAME" --headless \
            --no-config --disc "$(wslpath -w "$PG_DISC")" --no-log-file --mute 1 \
            --vibration 0 --no-card "${wargs[@]}" </dev/null > "$dir/run.log" 2>&1
        rc=$?
    fi
    echo "$rc" > "$dir/rc"
}

# census DIR: "<samples> <longest ms>" from the stall monitor's census line, or "- -".
census() {
    sed -n 's/.*stall monitor: \([0-9]*\) samples, longest game-fiber run with openpete_vbl unchanged \([0-9]*\) ms.*/\1 \2/p' \
        "$1/run.log" | tail -n 1 | grep . || echo "- -"
}

# trip_ms DIR: the stall in ms from the trip line, or "-".
trip_ms() {
    sed -n 's/.*game fiber ran \([0-9]*\)\.\([0-9]*\) s wall.*/\1\2/p' "$1/run.log" |
        head -n 1 | sed 's/^0*\([0-9]\)/\1/' | grep . || echo "-"
}

# verdict_trip ROW PLATFORM DIR NEED: the pass test of a row that must trip;
# NEED is the text the trip line must carry.
verdict_trip() {
    local row="$1" plat="$2" dir="$3" need="$4" rc ms head
    rc="$(cat "$dir/rc")"; ms="$(trip_ms "$dir")"
    head="[hang-stall] row=$row platform=$plat exit=$rc stall_ms=$ms limit_ms=$((N_S * 1000))"
    if [[ "$rc" != 2 || "$ms" == - ]]; then
        echo "$head verdict=FAIL: no stall trip"; grep -E 'watchdog' "$dir/run.log" | head -n 3 | sed 's/^/[hang-stall]   /'
        return 1
    fi
    if (( ms > (N_S + 3) * 1000 )); then echo "$head verdict=FAIL: tripped late"; return 1; fi
    if ! grep -q "$need" "$dir/run.log"; then echo "$head verdict=FAIL: trip line lacks \"$need\""; return 1; fi
    if [[ ! -s "$dir/hang-trace.bin" || "$(stat -c %s "$dir/hang-ram.bin" 2>/dev/null || echo 0)" != 2097152 ]]; then
        echo "$head verdict=FAIL: dump files missing"; return 1
    fi
    echo "$head verdict=PASS"
}

# verdict_clean ROW PLATFORM DIR: the pass test of a row that must not trip.
verdict_clean() {
    local row="$1" plat="$2" dir="$3" rc samples ms head
    rc="$(cat "$dir/rc")"; read -r samples ms < <(census "$dir")
    head="[hang-stall] row=$row platform=$plat exit=$rc samples=$samples longest_ms=$ms limit_ms=$((N_S * 1000))"
    if [[ "$rc" != 0 ]]; then
        echo "$head verdict=FAIL"; grep -E 'watchdog' "$dir/run.log" | head -n 3 | sed 's/^/[hang-stall]   /'
        return 1
    fi
    if [[ "$samples" == - || "$samples" == 0 ]]; then echo "$head verdict=VACUOUS: the monitor took no sample"; return 1; fi
    if (( ms >= N_S * 1000 )); then echo "$head verdict=FAIL: stretch at the limit"; return 1; fi
    echo "$head verdict=PASS"
}

# The inline mod of the spin row: CameraUpdate busy-waits on its 200th call.
write_spin_mod() {
    local root="$1" body="$2"
    rm -rf "${root:?}/stall-probe"
    mkdir -p "$root/stall-probe/src"
    printf '[mod]\nid = "stall-probe"\nversion = "0.0.1"\napi = 1\ngame = ["spyro-1"]\n' \
        > "$root/stall-probe/mod.toml"
    {
        cat <<'EOF'
#include <openpete_mod_api.h>
static const openpete_mod_api_t* A; static openpete_mod_t* S; static unsigned n;
static volatile unsigned long spin;
/** @brief CameraUpdate override: run the original, and on the 200th call never return. */
static void fn(CPUState* cpu) {
    A->base(cpu);
    if (++n == 200u) {
        A->log(S, OP_MOD_LOG_WARN, "spinning without a VBL");
EOF
        printf '        for (;;) { %s }\n    }\n}\n' "$body"
        cat <<'EOF'
/** @brief Bind the override. */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    A = api; S = self;
    return api->override_name(self, "CameraUpdate", fn);
}
EOF
    } > "$root/stall-probe/src/m.c"
}

linux_ok() { [[ -x "$PG_BIN" && -f "$PG_DISC" ]]; }
win_ok() { [[ -f "$WIN_EXE" && -f "$PG_DISC" ]] && command -v wslpath >/dev/null; }

FAILS=0 RAN=0
LEG_ENV=()
leg() {  # leg ROW PLATFORM: run and judge one leg
    local row="$1" plat="$2" dir="$OUT_ROOT/$1-$2"
    if [[ "$plat" == linux ]] && ! linux_ok; then echo "[hang-stall] row=$row platform=linux SKIP (binary or disc missing)"; return; fi
    if [[ "$plat" == windows ]] && ! win_ok; then echo "[hang-stall] row=$row platform=windows SKIP (no $WIN_EXE)"; return; fi
    RAN=$((RAN + 1))
    case "$row" in
        spin|callspin)
            # spin: host code that enters no guest function and polls no
            # loop, so the monitor dumps off-thread. callspin: guest calls
            # (Cos, 0x80016CB0) through the dispatch layer, so the game fiber
            # takes the request at an entry; the entry-count trip is raised
            # out of reach so the stall monitor is the one that fires.
            local body="spin++;" need="dumped off-thread"
            LEG_ENV=()
            if [[ "$row" == callspin ]]; then
                body="A->call(cpu, 0x80016CB0u);" need="taken at 0x80016CB0"
                LEG_ENV=(OPENPETE_HANG_THRESHOLD=4000000000)
            fi
            write_spin_mod "$OUT_ROOT/spin-mods-$plat" "$body"
            if [[ "$plat" == windows ]]; then
                # The Windows exe finds no compiler under WSL: build the DLL
                # the way the mod loader does and ship it as the prebuilt.
                local sdk m="$OUT_ROOT/spin-mods-$plat/stall-probe"
                sdk="$(dirname "$WIN_EXE")/sdk"
                mkdir -p "$m/.build/spyro-1"
                x86_64-w64-mingw32-gcc -shared -Wl,--export-all-symbols -O2 -fno-strict-aliasing \
                    -I"$sdk/games/$GAME" -I"$sdk" -o "$m/.build/spyro-1/stall-probe.dll" \
                    "$m/src/m.c" "$sdk/openpete_mod_abi.c" ||
                    { echo "[hang-stall] row=$row platform=windows verdict=FAIL: probe DLL did not build"; FAILS=$((FAILS + 1)); LEG_ENV=(); return; }
            fi
            local exe="$PG_BIN"; [[ "$plat" == windows ]] && exe="$WIN_EXE"
            if [[ ! -f "$(dirname "$exe")/sdk/openpete_mod_api.h" ]]; then
                RAN=$((RAN - 1)); echo "[hang-stall] row=$row platform=$plat SKIP (no mod SDK beside $exe: build openpete_sdk_stage)"; LEG_ENV=(); return
            fi
            run_leg "$plat" "$dir" $((N_S + 120)) "$OUT_ROOT/spin-mods-$plat" \
                --bk2 "$MOVIE" --skip-to-level artisans --frame-limit 5350
            LEG_ENV=()
            verdict_trip "$row" "$plat" "$dir" "$need" || FAILS=$((FAILS + 1));;
        triple)
            local src="${OPENPETE_HANG_STALL_MODS:-}" m
            if [[ -z "$src" ]]; then RAN=$((RAN - 1)); echo "[hang-stall] row=triple SKIP (OPENPETE_HANG_STALL_MODS unset)"; return; fi
            if [[ ! -f "$(dirname "$WIN_EXE")/sdk/openpete_mod_api.h" ]]; then
                RAN=$((RAN - 1)); echo "[hang-stall] row=triple SKIP (no mod SDK beside $WIN_EXE: build openpete_sdk_stage)"; return
            fi
            rm -rf "${OUT_ROOT:?}/triple-mods"; mkdir -p "$OUT_ROOT/triple-mods"
            for m in spyro-japanese-extras cynder sparx-swap; do
                [[ -d "$src/$m" ]] || { RAN=$((RAN - 1)); echo "[hang-stall] row=triple SKIP ($src/$m missing)"; return; }
                cp -r "$src/$m" "$OUT_ROOT/triple-mods/"
            done
            run_leg "$plat" "$dir" $((N_S + 300)) "$OUT_ROOT/triple-mods" \
                --bk2 "$MOVIE" --skip-to-level artisans --frame-limit 5350
            verdict_trip triple "$plat" "$dir" "hang detected: game fiber ran" || FAILS=$((FAILS + 1));;
        demo)
            run_leg "$plat" "$dir" 1800 - --frame-limit 20000
            verdict_clean demo "$plat" "$dir" || FAILS=$((FAILS + 1));;
        load)
            run_leg "$plat" "$dir" 900 - --bk2 "$MOVIE" --skip-to-level artisans --frame-limit 5350
            verdict_clean load "$plat" "$dir" || FAILS=$((FAILS + 1));;
        *) echo "hang-stall: unknown row $row" >&2; exit 64;;
    esac
}

for row in "${ROWS[@]}"; do
    case "$row" in
        triple) [[ "$PLATFORM" != linux ]] && leg triple windows;;
        *)
            [[ "$PLATFORM" != windows ]] && leg "$row" linux
            [[ "$PLATFORM" != linux ]] && leg "$row" windows;;
    esac
done
(( RAN )) || exit 64
(( FAILS )) && exit 1
exit 0
