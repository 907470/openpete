#!/usr/bin/env bash
# win-pad-unfocused.sh - --pad-unfocused on the Windows exe, automated.
#
# Launches the windows-mingw exe twice with --background (window created
# without activation, so it has no keyboard focus), the SDL virtual gamepad
# attached (OPENPETE_INPUT_VGAMEPAD=1; its synthetic pattern presses a face
# button or the d-pad every 8 VBLs and sweeps the left stick), and
# --bk2-record. Leg "on" passes --pad-unfocused 1, leg "off" --pad-unfocused 0.
# The recorded Input Log holds the staged port-0 record of every tick.
#
# Pass: leg "on" has non-neutral port-0 records (the engagement count) and
# leg "off" has none, and both legs report input_focus=0 at exit (a focused
# window makes the off leg vacuous). --input-lock hides every physical pad and
# the keyboard; the virtual pad passes the lock. --no-config keeps a user toml
# out of the run.
#
# Usage (from the openpete checkout root, a current windows-mingw build):
#   tests/harness/check_pad_unfocused_win.sh [EXE] [VBLS]
# Exit: 0 PASS, 1 FAIL, 2 VACUOUS (a leg was focused or recorded nothing),
#       64 SKIP (no exe or disc).

# xvfb-guard: windows-only (the run is the Windows .exe on the Windows desktop)
set -u
cd "${OPENPETE_ROOT:-$(pwd)}"

EXE="${1:-build/windows-mingw/openpete.exe}"
VBLS="${2:-1200}"
DISC="games/spyro-1/spyro-1/build/SCUS-94228.cue"
OUT="build/pad-unfocused-win"
[ -x "$EXE" ] || { echo "[pad-unfocused] SKIP: no exe at $EXE"; exit 64; }
[ -f "$DISC" ] || { echo "[pad-unfocused] SKIP: no disc at $DISC"; exit 64; }

# shellcheck source=scripts/lib/wslenv.sh
. scripts/lib/wslenv.sh
# shellcheck source=scripts/lib/scratch_cards.sh
. scripts/lib/scratch_cards.sh   # OPENPETE_CARD_DIR: scratch cards, forwarded by wslenv_forward
mkdir -p "$OUT"
rm -f build/pad-unfocused-win/on.bk2 build/pad-unfocused-win/on.bk2.args \
      build/pad-unfocused-win/on.log build/pad-unfocused-win/off.bk2 \
      build/pad-unfocused-win/off.bk2.args build/pad-unfocused-win/off.log

wslenv_forward OPENPETE_INPUT_VGAMEPAD=1 OPENPETE_VIBRATION=0

run_leg() { # run_leg <name> <0|1>
    env "${WSLENV_ARGS[@]}" "$EXE" --no-config --disc "$DISC" \
        --background --input-lock --vibration 0 --mute 1 \
        --pad-unfocused "$2" --bk2-record "$OUT/$1.bk2" \
        --frame-limit "$VBLS" >"$OUT/$1.log" 2>&1
    echo "[pad-unfocused] leg $1 exit $?"
}
run_leg on 1
run_leg off 0

# Live port-0 records of a bk2: a record with a pressed button, or one whose
# axes differ from the previous record's, not counting the first axis change.
# SDL takes an axis's first reported value as its rest position and, while
# unfocused, drops every later move away from it; a pad attached mid-sweep
# therefore records one constant off-centre value, which is not input.
count_live() { # count_live <bk2>
    python3 - "$1" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
log = z.read("Input Log.txt").decode("ascii", "replace").splitlines()
recs = live = 0
prev = None
first_change = True
for line in log:
    if not line.startswith("|    1,"):
        continue
    f = line.split("|")[2].split(",")
    axes = tuple(int(v) for v in f[:4])
    recs += 1
    moved = prev is not None and axes != prev
    if moved and first_change:
        first_change = False
        moved = False
    if moved or any(c != "." for c in f[4]):
        live += 1
    prev = axes
print(recs, live)
PY
}

rc=0
for leg in on off; do
    [ -f "$OUT/$leg.bk2" ] || { echo "[pad-unfocused] VACUOUS: leg $leg wrote no bk2"; exit 2; }
    grep -q "input_focus=0" "$OUT/$leg.log" || {
        echo "[pad-unfocused] VACUOUS: leg $leg window had focus ($(grep -o 'input_focus=[0-9]' "$OUT/$leg.log" | tail -1))"
        rc=2; }
    grep -q "VIRTUAL gamepad attached" "$OUT/$leg.log" || {
        echo "[pad-unfocused] VACUOUS: leg $leg attached no virtual pad"; rc=2; }
done
read -r on_recs on_live < <(count_live "$OUT/on.bk2")
read -r off_recs off_live < <(count_live "$OUT/off.bk2")
echo "[pad-unfocused] on:  records $on_recs live $on_live"
echo "[pad-unfocused] off: records $off_recs live $off_live"
[ "$rc" = 0 ] || exit "$rc"
if [ "${on_recs:-0}" -le 0 ] || [ "${off_recs:-0}" -le 0 ]; then
    echo "[pad-unfocused] VACUOUS: a leg recorded no tick"; exit 2
fi
if [ "${on_live:-0}" -gt 0 ] && [ "${off_live:-1}" = 0 ]; then
    echo "[pad-unfocused] PASS (engagement: $on_live live records with the knob on)"
    exit 0
fi
echo "[pad-unfocused] FAIL"
exit 1
