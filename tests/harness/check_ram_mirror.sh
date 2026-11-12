#!/usr/bin/env bash
# check_ram_mirror.sh - a moby the game relinks at a negative coordinate,
# end to end, through mods/ram-mirror-probe.
#
# The game's moby grid relink computes its bucket index with unsigned
# shifts, so a moby at a negative X stores its bucket's head word 2 MB or
# more past the grid, which on hardware is a RAM mirror of the bucket its
# stored index names (physical RAM repeats through 8 MB). The probe makes
# the game's own relink of one live moby run with the coordinate negative
# `force` times, then once with it restored (mods/ram-mirror-probe/mod.toml).
#
# Rows, per game (Spyro 1 town_square, Spyro 2 idol_springs, Spyro 3
# sunny_villa, from --skip-to-level):
#   x         X negative. Pass: exit 0 at the frame limit, the probe's PASS
#             line (every forced relink left the moby heading the bucket its
#             halfword names, read through the base and the mirror address,
#             and the release relink left it heading its own bucket), the
#             census mirror_hit count above 0 (engagement), and no arena
#             block mapped (no "guest arena live" line); mirror_hit is the
#             census's mirror hits less its host lookups, the guest bus
#             accesses the mirror resolved.
#   y         Y negative: the head word lies about 64 MB past the grid, where
#             hardware raises a data bus error; the bus reads 0 and drops the
#             store. Pass: exit 0, the probe's PASS line, and the bus's
#             unmapped-store warning naming the store.
# Modes:
#   --control BIN   run the x row on BIN (a build without the mirror); pass
#             when it ends in the hang watchdog: exit 2 and the trip line
#             naming the relink's unlink loop header.
#   --conflict      run the x row with the probe's `conflict` config, which
#             maps 9 MiB and then 2 MiB of the mod arena's DMA-reachable pool
#             so the second block covers the first RAM mirror copy; reports
#             the exit code and the watchdog line, and has no pass criterion.
#
# Outputs under ${RAM_MIRROR_WORK:-build/linux-release/ram-mirror}/<game>-<row>/
# (run.log, census.txt, mirror.txt: the mirror site census), deleted before
# each run.
#
# Usage: tests/harness/check_ram_mirror.sh [--game spyro-1|spyro-2|spyro-3]...
#            [--row x|y]... [--control BIN | --conflict]
# Exit 0 PASS, 1 FAIL, 64 bad usage or no binary or disc.
set -u
export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAMES=() ROWS=() CONTROL="" CONFLICT=0
while (( $# )); do
    case "$1" in
        --game) GAMES+=("$2"); shift ;;
        --row) ROWS+=("$2"); shift ;;
        --control) CONTROL="$2"; shift ;;
        --conflict) CONFLICT=1 ;;
        *) echo "ram-mirror: unknown argument $1" >&2; exit 64 ;;
    esac
    shift
done
(( ${#GAMES[@]} )) || GAMES=(spyro-1 spyro-2 spyro-3)
(( ${#ROWS[@]} )) || ROWS=(x y)
[[ -n "$CONTROL" || "$CONFLICT" == 1 ]] && ROWS=(x)
WORK="${RAM_MIRROR_WORK:-build/linux-release/ram-mirror}"

# level_of GAME: the level each game's rows skip to.
level_of() {
    case "$1" in
        spyro-1) echo town_square ;;
        spyro-2) echo idol_springs ;;
        spyro-3) echo sunny_villa ;;
    esac
}
# limit_of GAME: the frame limit (yields on Spyro 1, VBLs on Spyro 2 and 3),
# a few hundred ticks past the forced relinks.
limit_of() {
    case "$1" in
        spyro-1) echo 4000 ;;
        *) echo 2400 ;;
    esac
}
# header_of GAME: the relink's unlink loop header, which a hang trips at.
header_of() {
    case "$1" in
        spyro-1) echo 0x80052AB4 ;;
        spyro-2) echo 0x80041634 ;;
        spyro-3) echo 0x80055E20 ;;
    esac
}

FAILS=0 RAN=0
for game in "${GAMES[@]}"; do
    pg_resolve "$game"
    [[ -n "$CONTROL" ]] && PG_BIN="$CONTROL"
    if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
        echo "[ram-mirror] game=$game SKIP (binary or disc missing)"
        continue
    fi
    for row in "${ROWS[@]}"; do
        tag="$row"
        [[ -n "$CONTROL" ]] && tag="x-control"
        [[ "$CONFLICT" == 1 ]] && tag="x-conflict"
        dir="$WORK/$game-$tag"
        mods="$dir/mods"
        rm -rf "${dir:?}/mods"
        rm -f "${dir:?}/run.log" "${dir:?}/census.txt" "${dir:?}/mirror.txt" "${dir:?}/hang-trace.bin" "${dir:?}/hang-ram.bin"
        mkdir -p "$mods/ram-mirror-probe"
        cp -r mods/ram-mirror-probe/mod.toml mods/ram-mirror-probe/src "$mods/ram-mirror-probe/"
        printf 'case = "%s"\nconflict = %s\n' "$row" "$([[ "$CONFLICT" == 1 ]] && echo true || echo false)" \
            > "$mods/ram-mirror-probe/config.toml"
        echo ram-mirror-probe > "$mods/enabled.txt"
        RAN=$((RAN + 1))
        PG_CAPTURE_ENV=("OPENPETE_ARENA_CENSUS=$dir/census.txt" "OPENPETE_MIRROR_CENSUS=$dir/mirror.txt"
                        "OPENPETE_HANG_TRACE=$dir/hang-trace.bin"
                        "OPENPETE_HANG_RAM=$dir/hang-ram.bin" "OPENPETE_HANG_STALL_SECONDS=20")
        PG_CAPTURE_TIMEOUT=300
        pg_capture "$dir/run.log" "" -- --mute 1 --no-card --no-log-file --mods-dir "$mods" \
            --skip-to-level "$(level_of "$game")" --frame-limit "$(limit_of "$game")"
        rc=$?
        log="$dir/run.log"
        # Guest bus lookups the mirror resolved: the census's mirror hits
        # less its host lookups (the probe's own reads), with no region mapped.
        mirror="$(sed -n 's/.*summary .*arena_host_hit=\([0-9]*\) mirror_hit=\([0-9]*\).*/\2 \1/p' "$log" |
                  tail -n 1 | awk '{print $1 - $2}')"
        trip="$(grep -m1 'hang detected' "$log")"
        head="[ram-mirror] game=$game row=$tag exit=$rc mirror_hit=${mirror:--}"
        if [[ "$CONFLICT" == 1 ]]; then
            echo "$head REPORT"
            grep -E 'ram-mirror-probe: (arena|target|forced relink 1|PASS|FAIL)|hang detected' "$log" | sed 's/^/[ram-mirror]   /'
            continue
        fi
        why=""
        if [[ -n "$CONTROL" ]]; then
            [[ "$rc" == 2 ]] || why="$why exit $rc, want 2;"
            [[ "$trip" == *"$(header_of "$game")"* ]] || why="$why no trip at the loop header $(header_of "$game");"
        else
            [[ "$rc" == 0 ]] || why="$why exit $rc;"
            grep -q 'ram-mirror-probe: PASS' "$log" || why="$why no probe PASS line;"
            grep -q 'guest arena live' "$log" && why="$why an arena block was mapped;"
            if [[ "$row" == x ]]; then
                [[ "${mirror:-0}" -gt 0 ]] || why="$why mirror_hit 0 (VACUOUS);"
            else
                grep -q 'guest store of 4 bytes at 0x8[0-9A-F]* reached no device' "$log" ||
                    why="$why no unmapped-store warning;"
            fi
        fi
        if [[ -z "$why" ]]; then
            echo "$head PASS"
        else
            echo "$head FAIL:$why"
            grep -E 'ram-mirror-probe|hang detected|reached no device' "$log" | head -n 8 | sed 's/^/[ram-mirror]   /'
            FAILS=$((FAILS + 1))
        fi
    done
done
(( RAN )) || exit 64
(( FAILS )) && exit 1
exit 0
