#!/usr/bin/env bash
# scripts/check_ram_diff.sh — whole-RAM diff, DuckStation vs openpete, at a
# translated function's entry.
#
# Usage:
#   scripts/check_ram_diff.sh <spyro-1|spyro-2|spyro-3> [--anchor first|loop|HEX]
#                             [--hit N] [--ref-hit M] [--keep DIR]
#
# Runs the DuckStation regtest module on the game's disc with
# -dump-ram-at-anchor and openpete with --halt-at-pc, then classifies every
# differing range with tools/ram_diff.py against
# games/<game>/manifest/ram_diff_expect.toml. The anchor must be an entry in
# the game's generated dispatch table, and its entry block must not be a
# loop head (DuckStation counts per execution of the instruction, openpete
# per call).
#
# Exit 0 PASS, 2 an unexpected range, a populated mirror that differs, a
# byte still in flight on the reference, a psyq_internal count above its
# baseline, or expected classes over the cap; 64 SKIP (disc, module, binary
# or expectation file missing) or a rejected anchor.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

GAME="${1:-}"; shift || true
[[ -n "$GAME" ]] || { sed -n 's/^# //p' "$0" | head -16; exit 64; }
ANCHOR=first; HIT=1; REF_HIT=""; KEEP=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --anchor) ANCHOR="$2"; shift 2 ;;
        --hit) HIT="$2"; shift 2 ;;
        --ref-hit) REF_HIT="$2"; shift 2 ;;
        --keep) KEEP="$2"; shift 2 ;;
        *) echo "[ram-diff] unknown arg: $1" >&2; exit 64 ;;
    esac
done
[[ -n "$REF_HIT" ]] || REF_HIT="$HIT"

GDIR="$ROOT/games/$GAME"
EXPECT="$GDIR/manifest/ram_diff_expect.toml"
[[ -f "$EXPECT" ]] || { echo "[ram-diff] SKIP: no $EXPECT" >&2; exit 64; }
N="${GAME#spyro-}"
BIN="$ROOT/build/linux-release/games/$GAME/openpete-spyro$N"
DS_BIN="$ROOT/third_party/duckstation/build-regtest/bin/duckstation-regtest"
[[ -x "$BIN" ]]    || { echo "[ram-diff] SKIP: binary missing: $BIN" >&2; exit 64; }
[[ -x "$DS_BIN" ]] || { echo "[ram-diff] SKIP: module missing: $DS_BIN (scripts/setup_duckstation.sh)" >&2; exit 64; }

# Expectation file fields the run needs: disc id, card, anchors, run args.
read_toml() { python3 - "$EXPECT" "$1" <<'PY'
import sys, tomli
d = tomli.load(open(sys.argv[1], "rb"))
v = d
for k in sys.argv[2].split("."):
    v = v.get(k) if isinstance(v, dict) else None
    if v is None: break
if isinstance(v, list): print(" ".join(str(x) for x in v))
elif isinstance(v, int): print(f"0x{v:08X}")
elif v is not None: print(v)
PY
}
DISC_ID="$(read_toml disc.id)"
DISC_REL="$(read_toml disc.path)"
CARD="$(read_toml reference.card)"
RUN_ARGS="$(read_toml openpete.args)"
MAP="$(read_toml map)"
case "$ANCHOR" in
    first) PC="$(read_toml anchors.first)" ;;
    loop)  PC="$(read_toml anchors.loop)" ;;
    0x*)   PC="$ANCHOR" ;;
    *) echo "[ram-diff] --anchor must be first, loop, or 0xHEX" >&2; exit 64 ;;
esac
[[ -n "$PC" ]] || { echo "[ram-diff] SKIP: anchor '$ANCHOR' not named in $EXPECT" >&2; exit 64; }
PC="$(printf '0x%08X' "$PC")"
if [[ -n "$DISC_REL" ]]; then DISC="$ROOT/$DISC_REL"; else DISC="$ROOT/../roms/$DISC_ID/$DISC_ID.cue"; fi
DISC="${OPENPETE_DISC:-$DISC}"
[[ -f "$DISC" ]] || { echo "[ram-diff] SKIP: disc missing: $DISC" >&2; exit 64; }

# Anchor validity: a dispatch-table entry whose entry block is not a loop head.
python3 - "$GDIR/generated" "$PC" <<'PY' || exit 64
import glob, re, sys
gen, pc = sys.argv[1], int(sys.argv[2], 16)
rows = {}
for f in glob.glob(f"{gen}/*dispatch*.c"):
    for m in re.finditer(r"case 0x([0-9A-Fa-f]{8})u:\s*(\w+)\(cpu\)", open(f).read()):
        rows[int(m.group(1), 16)] = m.group(2)
if pc not in rows:
    print(f"[ram-diff] anchor 0x{pc:08X} is not a translated function entry", file=sys.stderr)
    sys.exit(1)
name = rows[pc]
body = None
for f in glob.glob(f"{gen}/*.c"):
    s = open(f).read()
    m = re.search(rf"^void {name}\(CPUState\* cpu\)\s*\{{", s, re.M)
    if m:
        end = re.search(r"^void \w+\(CPUState\* cpu\)\s*\{", s[m.end():], re.M)
        body = s[m.end(): m.end() + (end.start() if end else len(s))]
        break
if body is None:
    print(f"[ram-diff] anchor {name}: no generated body found", file=sys.stderr)
    sys.exit(1)
if re.search(rf"goto block_{pc:08x};|goto block_{pc:08X};", body):
    print(f"[ram-diff] anchor {name}: its entry block is a loop head (per-iteration on the "
          f"reference, per-call here); choose another entry", file=sys.stderr)
    sys.exit(1)
print(f"[ram-diff] anchor {name} @ 0x{pc:08X}: dispatch entry, not a loop head")
PY

WORK="${KEEP:-${TMPDIR:-/tmp}/openpete-ram-diff.$$}"
mkdir -p "$WORK"
[[ -n "$KEEP" ]] || trap 'rm -rf "$WORK"' EXIT
REF="$WORK/ref.bin"; OP="$WORK/op.bin"; OPLOG="$WORK/op.log"; DSLOG="$WORK/ds.log"
rm -f "$REF" "$OP" "$OPLOG" "$DSLOG" "$WORK/throwaway.psxtrace"

DS_CARD=()
[[ "$CARD" == "none" ]] && DS_CARD=(-card1 none)
OP_CARD=()
[[ "$CARD" == "none" ]] && OP_CARD=(--no-card)

echo "[ram-diff] $GAME anchor=$ANCHOR pc=$PC hit=$HIT ref-hit=$REF_HIT card=${CARD:-nonpersistent}"
ulimit -c 0
# The module aborts on exit after a capture; its exit status is not the verdict.
LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=llvmpipe \
sh -c '"$@" >/dev/null 2>&1; exit 0' _ "$DS_BIN" \
    -frames 20000 \
    -psxtrace "$WORK/throwaway.psxtrace" \
    -psxtrace-anchor-pc "$PC" \
    -psxtrace-anchor-hit "$REF_HIT" \
    -psxtrace-frames 1 \
    -dump-ram-at-anchor "$REF" \
    "${DS_CARD[@]}" \
    -console \
    "$DISC" &
DS_PID=$!

set +e
OPENPETE_VIBRATION=0 timeout 900 "$BIN" --headless --no-config --mods "" --mute 1 --vibration 0 \
    "${OP_CARD[@]}" --disc "$DISC" $RUN_ARGS \
    --halt-at-pc "$PC" --halt-hit "$HIT" --ram-dump-output "$OP" \
    --frame-limit 20000 > "$OPLOG" 2>&1
OP_RC=$?
wait "$DS_PID"
set -e

[[ -s "$REF" ]] || { echo "[ram-diff] FAIL: reference dump not produced (anchor never hit?)" >&2; exit 2; }
[[ -s "$OP"  ]] || { echo "[ram-diff] FAIL: openpete dump not produced (rc=$OP_RC; see $OPLOG)" >&2; exit 2; }

REGIONS=()
[[ "$GAME" == "spyro-1" ]] && REGIONS=(--regions "$ROOT/manifest/gameplay_state_regions.toml")
MAPARG=()
[[ -n "$MAP" && -f "$ROOT/$MAP" ]] && MAPARG=(--map "$ROOT/$MAP")
# The ratchet is keyed by the anchor as spelled (first, loop, or 0xHEX), so a
# hex anchor needs its own baseline row too.
python3 "$ROOT/tools/ram_diff.py" --ref "$REF" --op "$OP" --expect "$EXPECT" \
    --anchor "$ANCHOR" --hit "$HIT" \
    --op-log "$OPLOG" "${MAPARG[@]}" "${REGIONS[@]}"
