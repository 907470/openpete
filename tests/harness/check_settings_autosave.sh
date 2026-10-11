#!/usr/bin/env bash

# tests/harness/check_settings_autosave.sh - the settings-menu autosave gate.
#
# Every menu change marks its row's knob for the settings writer, which
# writes the [saved] zone of the --config file 500 ms after the last change,
# on menu close and at shutdown. Each leg boots Spyro 1 headless with its own
# config file, drives the menu with an injected key script
# (OPENPETE_MOUSE_INJECT) and reads the file and the writer's log lines.
#
#   leg save     the file starts with a hand-written section and no managed
#                zone; --lod original is typed. The script opens the menu and
#                steps FOV, LOD and FREECAM, then leaves the menu open
#                until the frame limit. Pass: a write with cause=debounce
#                (engagement: writes > 0), the zone holds fov and lod (the
#                typed --lod pins lod, and its menu change is saved anyway),
#                no freecam key, and the bytes above the marker equal the
#                original file.
#   leg unsave   the file's zone holds fov = 30; the script steps FOV back
#                to DEFAULT, the value of the hand-written global fov = 0,
#                and closes the menu inside the debounce interval. Pass: a
#                write with cause=menu-close, and the zone no longer holds
#                fov.
#   leg fail     OPENPETE_WRITE_FAIL names the config file. The script steps
#                FOV. Pass: the debounce write fails, the shutdown write
#                retries the same dirty knob and fails, and the file is
#                byte-identical to the original.
#
# The rows are ones whose registry getter reads the live setting headless:
# under --headless the display rows (ASPECT, SSAA, VSYNC and the rest
# behind KNOB_LIVE_OR_STORED) report their boot value, so a headless leg
# cannot show their saves.
#
# Usage: tests/harness/check_settings_autosave.sh [--game SLUG] [--build-dir=DIR]
# Exit:  0 pass, 1 fail, 2 vacuous (no write engaged), 64 skip (no binary /
#        no disc / not Spyro 1).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

for a in "$@"; do
    case "$a" in --build-dir=*) export PG_BUILD_DIR="${a#--build-dir=}" ;; esac
done
ARGS=()
for a in "$@"; do
    case "$a" in --build-dir=*) ;; *) ARGS+=("$a") ;; esac
done

# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game ${ARGS[@]+"${ARGS[@]}"})" || exit 64
pg_resolve "$GAME" || exit 64
[[ -x "$PG_BIN" ]]  || { echo "[autosave] SKIP: no binary $PG_BIN"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "[autosave] SKIP: no disc $PG_DISC"; exit 64; }
# The level name is Spyro 1's.
[[ "$PG_GAME" == "spyro-1" ]] ||
    { echo "[autosave] SKIP: $PG_GAME has no autosave scenario"; exit 64; }

OUT="${SETTINGS_AUTOSAVE_OUT:-${TMPDIR:-/tmp}/openpete-check-settings-autosave}"
mkdir -p "$OUT"
FRAME_LIMIT=3000
MARKER='# >>> openpete managed settings - written by the in-game menus <<<'

# keys FRAME KEY...: one keydown/keyup pair per key, 8 VBLs apart from FRAME.
keys() {
    local f="$1"; shift
    local k
    for k in "$@"; do
        echo "$f keydown $k"
        echo "$((f + 4)) keyup $k"
        f=$((f + 8))
    done
}

# run_leg TAG SCRIPT [ENV...] -- [ARGS...]: one headless run on $OUT/TAG.toml.
run_leg() {
    local tag="$1" script="$2"; shift 2
    local -a env=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do env+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    rm -f "$OUT/$tag.log"
    env OPENPETE_VIBRATION=0 OPENPETE_MOUSE_INJECT="$script" ${env[@]+"${env[@]}"} \
        timeout 600 "$PG_BIN" --headless --config "$OUT/$tag.toml" \
        --disc "$PG_DISC" --disc-cache 0 ${PG_RUN_ARGS[@]+"${PG_RUN_ARGS[@]}"} \
        --renderer vk --no-card --mute 1 --skip-to-level artisans \
        --frame-limit "$FRAME_LIMIT" "$@" >"$OUT/$tag.log" 2>&1 ||
        { echo "[autosave] FAIL: leg $tag exited non-zero (see $OUT/$tag.log)"; return 1; }
}

# zone FILE: the lines below the marker.
zone() { sed -n "/^$(printf '%s' "$MARKER" | sed 's/[][\.*^$/]/\\&/g')\$/,\$p" "$1"; }
# head_of FILE: the bytes above the marker.
head_of() { python3 - "$1" "$MARKER" <<'EOF'
import sys
b = open(sys.argv[1], 'rb').read()
i = b.find(sys.argv[2].encode())
sys.stdout.buffer.write(b if i < 0 else b[:i])
EOF
}
count() { grep -c -- "$2" "$OUT/$1.log" || true; }

HAND='# hand-written settings, kept byte for byte by every save
fov = 0   # the game'"'"'s own projection

[game.spyro-2]
ssaa = 2
'

# Leg save: open (row 0), FOV (row 3) +1, LOD (row 11) +1, FREECAM +1.
rm -f "$OUT/save.toml" "$OUT/save.orig" "$OUT/save.txt"
printf '%s' "$HAND" >"$OUT/save.toml"
cp "$OUT/save.toml" "$OUT/save.orig"
{
    keys 1500 Escape
    keys 1540 Down Down Down Right
    keys 1600 Down Down Down Down Down Down Down Down Right
    keys 1720 Down Right
} >"$OUT/save.txt"

# Leg unsave: the zone holds fov = 30; FOV -1 is DEFAULT (0).
rm -f "$OUT/unsave.toml" "$OUT/unsave.txt"
{ printf '%s' "$HAND"; printf '%s\n%s\n[saved]\nfov = 30\n' "$MARKER" \
    '# Anything below this line is REGENERATED on save; edit above it.'; } >"$OUT/unsave.toml"
{ keys 1500 Escape; keys 1540 Down Down Down Left; keys 1580 Escape; } >"$OUT/unsave.txt"

# Leg fail: every write of the config file fails.
rm -f "$OUT/fail.toml" "$OUT/fail.orig" "$OUT/fail.txt"
printf '%s' "$HAND" >"$OUT/fail.toml"
cp "$OUT/fail.toml" "$OUT/fail.orig"
{ keys 1500 Escape; keys 1540 Down Down Down Right; } >"$OUT/fail.txt"

# Two game processes at a time.
rc=0
run_leg save "$OUT/save.txt" -- --lod original & p1=$!
run_leg fail "$OUT/fail.txt" OPENPETE_WRITE_FAIL=fail.toml -- & p2=$!
wait "$p1" || rc=1
wait "$p2" || rc=1
run_leg unsave "$OUT/unsave.txt" -- || rc=1
[[ $rc -eq 0 ]] || exit 1

writes_save="$(count save 'settings: saved cause=')"
echo "[autosave] save: writes=$writes_save" \
     "debounce=$(count save 'saved cause=debounce')" \
     "freecam_toggled=$(count save 'set: \[FREECAM\] = ON')" \
     "lod_set=$(count save 'set: \[LOD\] = EXTENDED')" \
     "fov_set=$(count save 'set: \[FOV\] = 30V')"
echo "[autosave] unsave: writes=$(count unsave 'settings: saved cause=')" \
     "menu_close=$(count unsave 'saved cause=menu-close')" \
     "fov_set=$(count unsave 'set: \[FOV\] = DEFAULT')"
echo "[autosave] fail: failed=$(count fail 'settings: save FAILED')" \
     "shutdown_retry=$(count fail 'save FAILED cause=shutdown ids=1')" \
     "injected=$(count fail 'injected failure (OPENPETE_WRITE_FAIL)')"
echo "[autosave] save zone:"; zone "$OUT/save.toml" | sed 's/^/    /'
echo "[autosave] unsave zone:"; zone "$OUT/unsave.toml" | sed 's/^/    /'

if [[ "$writes_save" -eq 0 ]]; then
    echo "[autosave] VACUOUS: the save leg performed no write"
    exit 2
fi

fail=0
[[ "$(count save 'saved cause=debounce')" -ge 1 ]] ||
    { echo "[autosave] FAIL: no write at the debounce deadline"; fail=1; }
[[ "$(count save 'set: \[FREECAM\] = ON')" -ge 1 &&
   "$(count save 'set: \[LOD\] = EXTENDED')" -ge 1 &&
   "$(count save 'set: \[FOV\] = 30V')" -ge 1 ]] ||
    { echo "[autosave] FAIL: the script did not step FOV, LOD and FREECAM"; fail=1; }
zone "$OUT/save.toml" | grep -qx 'fov = 30' ||
    { echo "[autosave] FAIL: fov not saved"; fail=1; }
zone "$OUT/save.toml" | grep -qx 'lod = "extended"' ||
    { echo "[autosave] FAIL: the --lod pinned knob's menu change was not saved"; fail=1; }
if zone "$OUT/save.toml" | grep -q '^freecam'; then
    echo "[autosave] FAIL: freecam was saved"; fail=1
fi
cmp -s <(head_of "$OUT/save.toml") "$OUT/save.orig" ||
    { echo "[autosave] FAIL: the hand-written section above the marker changed"; fail=1; }

[[ "$(count unsave 'set: \[FOV\] = DEFAULT')" -ge 1 &&
   "$(count unsave 'saved cause=menu-close')" -ge 1 ]] ||
    { echo "[autosave] FAIL: the unsave leg did not step FOV and write on menu close"; fail=1; }
if zone "$OUT/unsave.toml" | grep -q '^fov'; then
    echo "[autosave] FAIL: fov equal to the global value stayed saved"; fail=1
fi
cmp -s <(head_of "$OUT/unsave.toml") "$OUT/save.orig" ||
    { echo "[autosave] FAIL: the unsave leg changed the hand-written section"; fail=1; }

[[ "$(count fail 'settings: save FAILED')" -ge 2 &&
   "$(count fail 'save FAILED cause=shutdown ids=1')" -ge 1 ]] ||
    { echo "[autosave] FAIL: the failed write did not keep its dirty knob for the retry"; fail=1; }
cmp -s "$OUT/fail.toml" "$OUT/fail.orig" ||
    { echo "[autosave] FAIL: a failed write changed the file"; fail=1; }

[[ $fail -eq 0 ]] || exit 1
echo "[autosave] PASS (writes=$writes_save)"
