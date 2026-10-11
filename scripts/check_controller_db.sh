#!/usr/bin/env bash
# scripts/check_controller_db.sh - prove the controller-mapping precedence
# chain, unattended.
#
# What decides which physical button reaches the game is the mapping SDL
# resolves for a pad's GUID. That is what this checks, per leg, from the
# resolved mapping string the boot log prints - not from "a file was read",
# which is a claim about openpete rather than about SDL's table.
#
# Legs:
#   a  a per-user database outranks the shipped and built-in mappings
#   c  an exported SDL_HINT_GAMECONTROLLERCONFIG_FILE is honoured, and the
#      shipped database is skipped rather than half-overriding it
#   d  with no user database the pad falls back to what it had before
#
# The pad is OPENPETE_INPUT_VGAMEPAD, so this runs on a machine with no
# controller attached. NOTE: that virtual pad's *buttons* do not currently
# reach decode (its writes succeed but SDL reports no button state), so this
# harness deliberately stops at the resolved mapping and does not claim
# anything about latched bits. Fixing that gap belongs to the virtual-pad
# instrument, not to the mapping chain.
#
# Usage: scripts/check_controller_db.sh [--game SLUG]
#
# Every assertion is about SDL's mapping table, which no game owns, so the
# check runs against whichever game --game names (default spyro-1).
#
# Exit 0 PASS, 1 FAIL, 64 SKIP (no binary or no disc).
#
# HOME and XDG_CONFIG_HOME are redirected into the scratch tree: the whole
# point is a per-user file, and a harness that wrote into the developer's own
# ~/.config would both pollute it and read whatever was already there.
set -uo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
# OPENPETE_EXE predates the resolver and still names the binary.
EXE="${OPENPETE_EXE:-$PG_BIN}"
PG_BIN="$EXE"
DISC="$PG_DISC"
# 500 yields on Spyro 1; Spyro 2 and 3 key on VBLs and add the boot's 573
# fast-boot VBLs.
FRAMES_DEFAULT=500
case "$GAME" in spyro-2|spyro-3) FRAMES_DEFAULT=1073;; esac
FRAMES="${OPENPETE_FRAME_LIMIT:-$FRAMES_DEFAULT}"
OUT="${TMPDIR:-/tmp}/openpete_controllerdb"

[[ -x "$EXE"  ]] || { echo "[controllerdb] no binary at $EXE - SKIP" >&2; exit 64; }
[[ -f "$DISC" ]] || { echo "[controllerdb] no disc at $DISC - SKIP" >&2; exit 64; }

rm -rf "$OUT"                       # never read a previous run's logs
mkdir -p "$OUT/home/.config/openpete"

USERDB="$OUT/home/.config/openpete/gamecontrollerdb.txt"

run() {   # run <tag> [env assignments...]
    local tag="$1"; shift
    # pg_capture deletes the log before the run, so a leg can never resolve
    # its mapping out of the previous leg's log and silences
    # vibration for the run.
    local -a PG_CAPTURE_ENV=(HOME="$OUT/home" XDG_CONFIG_HOME="$OUT/home/.config"
                             OPENPETE_INPUT_VGAMEPAD=1 "$@")
    pg_capture "$OUT/$tag.log" "" -- --mute 1 --frame-limit "$FRAMES"
    [[ -s "$OUT/$tag.log" ]] || { echo "[controllerdb] $tag produced no log" >&2; return 1; }
}

# The mapping SDL ended up using for the pad, which is the whole question.
resolved() { sed -n 's/.*pad [0-9a-f]\{32\} mapping: //p' "$OUT/$1.log" | head -1; }
origin()   { sed -n 's/.*mapped from //p' "$OUT/$1.log" | head -1; }

fail=0
note() { echo "[controllerdb] $*"; }
bad()  { echo "[controllerdb] FAIL: $*" >&2; fail=1; }

# --- baseline ------------------------------------------------------------
run base || exit 1
base_map=$(resolved base)
# Engagement: with no pad reported there is no mapping to compare,
# and every leg below would agree with every other by reporting nothing.
[[ -n "$base_map" ]] || { echo "[controllerdb] no pad reported - the virtual pad" \
                               "did not attach, so nothing was measured" >&2; exit 1; }
grep -q "mappings: shipped" "$OUT/base.log" \
    || bad "the shipped database did not load"
note "baseline mapping from $(origin base)"

# --- leg a: a per-user database outranks everything below it -------------
SWAP="$(sed -n 's/.*pad \([0-9a-f]\{32\}\) mapping:.*/\1/p' "$OUT/base.log" | head -1)"
SWAP="$SWAP,openpete virtual pad,a:b1,b:b0,x:b2,y:b3,start:b6,platform:Linux,"
printf '%s\n' "$SWAP" > "$USERDB"
run user || exit 1
user_map=$(resolved user)
[[ "$(origin user)" == user ]] || bad "the pad did not resolve to the per-user database"
case "$user_map" in
    *a:b1,b:b0*) note "leg a: SDL resolved the per-user mapping" ;;
    *) bad "the per-user mapping did not win: $user_map" ;;
esac
[[ "$user_map" != "$base_map" ]] || bad "the per-user database changed nothing"

# --- leg d: removing it restores the baseline ----------------------------
rm -f "$USERDB"
run again || exit 1
[[ "$(resolved again)" == "$base_map" ]] \
    && note "leg d: baseline restored with no per-user database" \
    || bad "removing the per-user database did not restore the baseline mapping"

# --- leg c: an exported hint file is honoured, shipped skipped -----------
printf '%s\n' "$SWAP" > "$OUT/hinted.txt"
run hint SDL_GAMECONTROLLERCONFIG_FILE="$OUT/hinted.txt" || exit 1
grep -q "shipped database skipped" "$OUT/hint.log" \
    || bad "the exported hint file did not suppress the shipped database"
if grep -q "mappings: shipped" "$OUT/hint.log"; then
    bad "the shipped database loaded anyway, and would overwrite the hint file"
fi
case "$(resolved hint)" in
    *a:b1,b:b0*) note "leg c: SDL resolved the exported hint file's mapping" ;;
    *) bad "the exported hint file's mapping did not win: $(resolved hint)" ;;
esac

if (( fail )); then echo "[controllerdb] verdict: FAIL"; exit 1; fi
echo "[controllerdb] verdict: PASS"
