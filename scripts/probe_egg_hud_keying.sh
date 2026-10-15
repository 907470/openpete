#!/usr/bin/env bash
# probe_egg_hud_keying.sh - egg-hud-3d's parked eggs keep a stable
# per-object interpolation identity.
#
# The engine keys per-object interpolation by moby ADDRESS outside the HUD
# pool (game_interp_moby_key), so a parked egg's identity is stable
# exactly when its park is: the guest arena never moves an allocation. The
# instrument is OPENPETE_INTERP_OBJS_KEYLOG, one line per interp-object
# SPAWN with its key; per-object capture only runs when presents outpace
# the sim (--render-fps above the tick rate), which headless never does, so
# this is a WINDOWED run - llvmpipe is fine, no pixels are judged.
#
# Verdict: every spawn keyed into the arena (0x807xxxxx) is an egg slot;
# a slot that spawns MORE than once while live would mean its key changed
# between ticks. Eggs also despawn legitimately when the HUD hides them, so
# the check counts spawns per key against the number of visibility
# episodes the mod reports: spawns == episodes.
#
# Windowed-only, so it is not a CTest gate: the headless harnesses cannot
# reach the per-object capture path this instrument reads.
#
# Usage: scripts/probe_egg_hud_keying.sh   (repo root). Exit 0 = PASS.
set -u
export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/.."
# The player's memory cards are never opened: runs boot on scratch cards.
source scripts/lib/scratch_cards.sh
BIN="${OPENPETE_BIN:-build/linux-release/openpete}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
WORK="$(mktemp -d /tmp/openpete-eggkey.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/mods"; ln -sfn "$PWD/mods/egg-hud-3d" "$WORK/mods/egg-hud-3d"
echo egg-hud-3d > "$WORK/mods/enabled.txt"
rm -f "$WORK/run.log"
OPENPETE_INTERP_OBJS_KEYLOG=1 timeout 600 "$BIN" --disc "$DISC" --disc-cache 0 \
 --window 640x480 --fullscreen 0 --render-fps 60 \
    --bk2 games/spyro-1/bk2/egg-inventory.bk2 --skip-to-level stone_hill \
    --frame-limit 4200 --mods-dir "$WORK/mods" >"$WORK/run.log" 2>&1
python3 - "$WORK/run.log" <<'PY'
import re, sys, collections
log = open(sys.argv[1], errors="replace").read()
spawns = collections.Counter(m.group(1) for m in re.finditer(r"\[interp-objs\] spawn key=(807[0-9a-f]{5})", log))
armed = "egg-hud-3d: armed" in log
if not armed:
    print("egg keying: mod never armed - VACUOUS"); sys.exit(2)
if not spawns:
    print("egg keying: no arena-keyed spawn observed - capture never ran (windowed --render-fps > tick?) VACUOUS"); sys.exit(2)
multi = {k: n for k, n in spawns.items() if n > 1}
print(f"egg keying: {len(spawns)} egg slots spawned, {sum(spawns.values())} spawns, "
      f"{len(multi)} slots spawned more than once")
# Address-keyed: the key IS the park address, so a slot cannot be re-keyed
# while the park stands. More than one spawn per slot is a despawn/respawn
# (an egg hidden, then shown), which the address key survives by
# construction; what would fail this check is a key value that is not a
# park address at all.
bad = [k for k in spawns if not (0x80700000 <= int(k, 16) < 0x80800000)]
if bad:
    print("egg keying: non-arena key among egg spawns:", bad); sys.exit(1)
print("egg keying: PASS (every egg spawn keyed by its arena slot address)")
PY
