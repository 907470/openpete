#!/usr/bin/env bash
# check_overlay_callbacks.sh - the overlay callback gate.
#
# Every overlay function the game reaches by a computed jump must be a
# dispatch entry of the overlay that is resident (a miss stub-returns
# v0=v1=0 and the callback never runs), and every such entry must be one the
# harvester attributed to that overlay from the bytes
# (tools/extract_overlay_func_pointers.py), not one native discovery happened
# to emit. The gate replays a --skip-to-level sweep with the dispatch census
# on (OPENPETE_DISPATCH_CENSUS, games/common/overlay_dispatch.c) and judges:
#
#   G2  no in-window dispatch missed; every sweep row's target overlay became
#       resident; every resident overlay shows at least one computed (`jalr`)
#       dispatch, or none when the harvester attributed it nothing.
#   G3  every cross-binary `jalr` pair (overlay, target) from a slot-fed
#       site (the harvester's list: a `jalr` whose register was loaded from
#       a fixed word; call site outside the overlay window) is in that
#       overlay's attributed set (generated/<overlay>/fn_pointers.txt plus the
#       curated rows of manifest/forced_entries/<overlay>.txt); a pair
#       attributed to another overlay is reported as stale_slot (the slot
#       still holds a previous level's constant) and any other pair is a rule
#       defect. A direct (`jal`) pair, and a `jalr` fed through a table index
#       or a struct field (a moby class table in the level's data), must name
#       a function start of the resident overlay. A `jalr` from inside the
#       window (an overlay calling through its own tables) is counted, not
#       judged: those targets are the recompiler's own discovery.
#
# Spyro 3 has no --skip-to-level, so its row is one plain boot through the
# attract loop, long enough (OPENPETE_BOOT_FRAME_LIMIT, 9000, plus the 573
# fast-boot VBLs Spyro 2 and 3 count) for three demo levels to become resident;
# G2 fails if none does.
#
#   scripts/check_overlay_callbacks.sh [spyro-1|spyro-2|spyro-3 ...]
#
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing). OPENPETE_FRAME_LIMIT
# overrides the 2400 per row (plus 573 on Spyro 2 and 3); OPENPETE_CALLBACK_JOBS
# the parallelism.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAMES=("$@")
[[ ${#GAMES[@]} -eq 0 ]] && GAMES=(spyro-1 spyro-2 spyro-3)
FRAMES="${OPENPETE_FRAME_LIMIT:-2400}"
BOOT_FRAMES="${OPENPETE_BOOT_FRAME_LIMIT:-9000}"
JOBS="${OPENPETE_CALLBACK_JOBS:-8}"
BUILD="${OPENPETE_BUILD_DIR:-build/linux-release}"
OUT="${OPENPETE_CALLBACK_OUT:-$BUILD/overlay-callbacks}"

status=0
for game in "${GAMES[@]}"; do
    # The slug is validated against the resolver's list, and the binary name
    # and disc path come from it, so this gate cannot drift from the one
    # authority for either (games/<slug>/oracle.conf). The resolver's
    # single-binary overrides are blanked for the same reason the loop exists:
    # one row per game, each with its own binary and its own disc.
    game="$(pg_parse_game --game "$game")" || exit 64
    OPENPETE_BIN= OPENPETE_DISC= PG_BUILD_DIR="$BUILD" pg_resolve "$game" || exit 64
    exe="$PG_BIN"
    disc="$PG_DISC"
    if [[ ! -x "$exe" || ! -f "$disc" ]]; then
        echo "[overlay-callbacks] $game: SKIP (missing $exe or $disc)"
        status=64
        continue
    fi
    dir="$OUT/$game"
    rm -rf "$dir"; mkdir -p "$dir"

    # Rows: "<skip name> <overlay name> <flag>" from the game's own level
    # table plus its cutscene table (credits: the short roll covers the
    # overlay); a game without the flag runs one plain boot.
    rows="$dir/rows.txt"
    "$exe" --skip-to-level 2>&1 \
        | awk '/^  [a-z]/ { name=$1; n=$2; gsub(/[()]/, "", n); print name, n }' \
        > "$rows.raw" || true
    : > "$rows"
    while read -r name n; do
        echo "$name level_$n --skip-to-level"
    done < "$rows.raw" >> "$rows"
    cutscenes="$("$exe" --skip-to-cutscene 2>&1 || true)"
    if grep -q '^  credits ' <<< "$cutscenes"; then
        echo "credits credits --skip-to-cutscene" >> "$rows"
    fi
    if [[ ! -s "$rows" ]]; then
        echo "boot title" > "$rows"
    fi

    run_row() {
        local name="$1" exe="$2" disc="$3" dir="$4" frames="$5" flag="$6"
        local skip=()
        [[ "$name" != "boot" ]] && skip=("$flag" "$name")
        [[ "$name" == "boot" ]] && frames="$OPENPETE_BOOT_FRAMES_RESOLVED"
        OPENPETE_DISPATCH_CENSUS="$dir/$name.census" \
        op_own_run timeout -k 10 900 "$exe" --headless --no-card --mute 1 --vibration 0 \
            --disc "$disc" --disc-cache 0 "${skip[@]}" --frame-limit "$frames" --no-log-file \
            > "$dir/$name.log" 2>&1 || echo "[overlay-callbacks] $name: exit $?" >> "$dir/$name.log"
    }
    export -f run_row
    # The limits are the feed key's: Spyro 2 and 3 count the boot's 573
    # fast-boot VBLs before the first yield, Spyro 1 counts yields.
    key0=0
    case "$game" in spyro-2|spyro-3) key0=573;; esac
    export OPENPETE_BOOT_FRAMES_RESOLVED="$((BOOT_FRAMES + key0))"
    while read -r name _ flag; do echo "$name ${flag:---skip-to-level}"; done < "$rows" \
        | xargs -P "$JOBS" -L1 bash -c 'run_row "$5" "$1" "$2" "$3" "$4" "$6"' _ "$exe" "$disc" "$dir" "$((FRAMES + key0))"

    if ! python3 - "$game" "$dir" "$rows" <<'EOF'; then status=1; fi
import re, sys
from pathlib import Path
game, dir_, rows = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
gen = Path(f'games/{game}/generated')
curated_dir = Path(f'games/{game}/manifest/forced_entries')

def addrs(path):
    if not path.exists():
        return set()
    return {int(m, 16) for m in re.findall(r'^(0x[0-9A-Fa-f]+)', path.read_text(), re.M)}

report = (gen / 'overlay_fn_report.txt').read_text()
m = re.search(r'window 0x([0-9A-F]+)\.\.0x([0-9A-F]+)', report)
win_lo, win_hi = int(m.group(1), 16), int(m.group(2), 16)
m = re.search(r'slot-fed sites \(\d+\): (.*)', report)
slot_sites = {int(x, 16) for x in m.group(1).split()} if m else set()
attributed, starts = {}, {}
for d in gen.iterdir():
    if (d / 'fn_pointers.txt').exists():
        attributed[d.name] = addrs(d / 'fn_pointers.txt') | addrs(curated_dir / f'{d.name}.txt')
        starts[d.name] = addrs(d / 'discovered_functions.log')
owner = {}
for o, s in attributed.items():
    for a in s:
        owner.setdefault(a, set()).add(o)

fails, notes = [], []
resident_all, jalr_by_ovl = set(), {}
intra = 0
data_fed = 0
for line in rows.read_text().split('\n'):
    if not line.strip():
        continue
    name, target = line.split()[:2]
    census = dir_ / f'{name}.census'
    if not census.exists():
        fails.append(f'{name}: no census written')
        continue
    resident = set()
    for rec in census.read_text().split('\n'):
        f = rec.split()
        if len(f) != 5:
            continue
        ovl, tgt, ra, cls, res = f[0], int(f[1], 16), int(f[2], 16), f[3], f[4]
        resident.add(ovl)
        if res == 'missed':
            fails.append(f'{name}: {ovl} missed 0x{tgt:08X} ({cls}, ra 0x{ra:08X})')
        if cls == 'jalr':
            if win_lo <= ra < win_hi:
                intra += 1
            elif ra - 8 in slot_sites:
                jalr_by_ovl.setdefault(ovl, set()).add(tgt)
            else:
                data_fed += 1
                if ovl in starts and tgt not in starts[ovl]:
                    fails.append(f'{name}: {ovl} data-fed jalr target 0x{tgt:08X} '
                                 f'(ra 0x{ra:08X}) is not a function start')
        elif cls == 'jal':
            if tgt not in starts.get(ovl, set()):
                fails.append(f'{name}: {ovl} direct jal target 0x{tgt:08X} is not a function start')
    if name != 'boot' and target not in resident:
        fails.append(f'{name}: target overlay {target} never resident (saw {sorted(resident)})')
    resident_all |= resident

for ovl in sorted(resident_all):
    if ovl == 'none':
        continue
    pairs = jalr_by_ovl.get(ovl, set())
    attr = attributed.get(ovl, set())
    harvested = addrs(gen / ovl / 'fn_pointers.txt')
    if not pairs and harvested:
        fails.append(f'{ovl}: resident with {len(harvested)} attributed entries but no jalr dispatch (VACUOUS)')
    for a in sorted(pairs):
        if a in attr:
            continue
        others = owner.get(a, set()) - {ovl}
        if others:
            notes.append(f'stale_slot: {ovl} dispatched 0x{a:08X}, attributed to {sorted(others)}')
        else:
            fails.append(f'G3: {ovl} jalr 0x{a:08X} is attributed to no overlay')

engaged = sum(len(v) for v in jalr_by_ovl.values())
print(f'[overlay-callbacks] {game}: {len(rows.read_text().split())//2} rows, '
      f'{len(resident_all - {"none"})} overlays resident, {engaged} cross-binary jalr pairs, '
      f'{intra} intra-overlay and {data_fed} data-fed jalr dispatches, '
      f'{len(notes)} stale_slot, {len(fails)} failures')
for n in notes:
    print('  ' + n)
for f in fails:
    print('  FAIL ' + f)
if not any(o.startswith('level_') for o in resident_all):
    print('  FAIL G2: no level overlay became resident (VACUOUS)')
    fails.append('vacuous')
sys.exit(1 if fails else 0)
EOF
done
[[ $status -eq 0 ]] && echo "[overlay-callbacks] verdict: PASS" || echo "[overlay-callbacks] verdict: FAIL/SKIP ($status)"
exit $status
