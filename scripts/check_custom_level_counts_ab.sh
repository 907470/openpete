#!/usr/bin/env bash
# Custom-level target totals: assert the three EXE count tables directly.
#
# The oracle cannot see these bytes — its regions start at 0x800756C8 and
# none covers 0x8006F414..0x8006F493 — so correctness is established here,
# by diffing whole table extents against a no-mods baseline rather than by
# reading a value back through the same code that wrote it.
#
# g_MemCardSaveHeader begins at 0x8006F494, immediately after the 18-entry
# egg table's last row. Every leg asserts those 512 bytes are untouched:
# an egg write one index too far lands in the memory-card header, and that
# is the one failure mode of this feature that is not merely cosmetic.
#
# Exit 0 PASS, 1 FAIL, 64 SKIP (binary or disc missing).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXE="$ROOT/build/linux-release/games/spyro-1/openpete-spyro1"
DISC="$ROOT/games/spyro-1/spyro-1/build/SCUS-94228.cue"
WORK="${TMPDIR:-/tmp}/openpete-counts-ab.$$"

[ -x "$EXE" ]  || { echo "SKIP: no $EXE"; exit 64; }
[ -f "$DISC" ] || { echo "SKIP: no $DISC"; exit 64; }

rm -rf "$WORK"; mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

fails=0
pass() { echo "  PASS: $*"; }
fail() { echo "  FAIL: $*"; fails=$((fails + 1)); }

# A declarative level mod: <dir> <id> <name> <stem> <donor> <replace> <extra toml>
make_mod() {
    local dir="$1" id="$2" name="$3" stem="$4" donor="$5" replace="$6" extra="$7"
    mkdir -p "$dir/$id/assets/levels"
    { echo '[mod]'
      echo "id      = \"$id\""
      echo "name    = \"$name\""
      echo 'version = "1.0.0"'
      echo 'game    = "spyro-1"'
      echo 'api     = 1'
      echo
      echo '[[level]]'
      echo "name  = \"$name\""
      echo "files = \"$stem\""
      echo "donor = \"$donor\""
      [ "$replace" = "1" ] && echo 'replace = true'
      echo "$extra"
    } > "$dir/$id/mod.toml"
    # Any nonzero size registers the row as playable; these legs never
    # stream the level, they only exercise the declaration path.
    head -c 4096 /dev/zero > "$dir/$id/assets/levels/$stem.wad"
}

# run <mods-dir> <dump-out> <log-out> <dump-frame> <frame-limit> [warp]
run() {
    local mods="$1" dump="$2" log="$3" at="$4" limit="$5" warp="${6:-}"
    rm -f "$dump" "$log"
    local -a env=()
    [ -n "$warp" ] && env=(env "OPENPETE_CUSTOM_WARP=$warp")
    "${env[@]}" timeout 300 "$EXE" --headless --disc "$DISC" --disc-cache 0 \
        --mods-dir "$mods" --vibration 0 \
        --ram-dump-on-frame "$at" --ram-dump-output "$dump" \
        --frame-limit "$limit" > "$log" 2>&1
    [ -s "$dump" ] || { echo "  (no RAM dump produced; see $log)"; return 1; }
    return 0
}

# cmp_tables <baseline> <candidate> <expect-spec>
# expect-spec: comma-separated table:index pairs expected to differ, or
# "none". The save header is always required identical.
#
# Compares whole TABLE ENTRIES, not raw bytes: a declared gem total whose
# high byte matches the stock value changes one byte of a two-byte field,
# so a byte-set comparison would call a correct write a failure.
cmp_tables() {
    python3 - "$1" "$2" "$3" <<'PY'
import sys, struct
base, cand, spec = open(sys.argv[1],'rb').read(), open(sys.argv[2],'rb').read(), sys.argv[3]
T = {"dragons": (0x6F414, 36, 1), "gems": (0x6F438, 36, 2), "eggs": (0x6F480, 18, 1)}
def entry(buf, off, i, w):
    return buf[off+i] if w == 1 else struct.unpack_from('<H', buf, off + 2*i)[0]
want = set()
if spec != "none":
    for pair in spec.split(","):
        t, i = pair.split(":")
        want.add((t, int(i)))
got = set()
for name, (off, n, w) in T.items():
    for i in range(n):
        if entry(base, off, i, w) != entry(cand, off, i, w):
            got.add((name, i))
ok = True
hdr = [i for i in range(512) if base[0x6F494+i] != cand[0x6F494+i]]
if hdr:
    print(f"    g_MemCardSaveHeader CHANGED at {hdr[:8]} — an egg write "
          f"landed past the table")
    ok = False
if got != want:
    print(f"    expected entries changed: {sorted(want)}")
    print(f"    actual   entries changed: {sorted(got)}")
    ok = False
sys.exit(0 if ok else 1)
PY
}

echo "[counts] baseline: no mods"
mkdir -p "$WORK/none"
run "$WORK/none" "$WORK/base.bin" "$WORK/base.log" 600 660 \
    || { echo "FAIL: baseline run produced no dump"; exit 1; }
pass "baseline captured"

echo
echo "[counts] replace row declares its donor's index, and only that"
make_mod "$WORK/rep" rep "Rep Fixture" rep_fx dark_hollow 1 \
    "$(printf 'dragons = 7\ngems    = 1234\neggs    = 2')"
if run "$WORK/rep" "$WORK/rep.bin" "$WORK/rep.log" 600 660; then
    # dark_hollow is level id 12 -> index (12/10-1)*6 + 12%10 = 2.
    if cmp_tables "$WORK/base.bin" "$WORK/rep.bin" "dragons:2,gems:2,eggs:2"; then
        pass "index 2 carries the declared totals; nothing else moved"
    else
        fail "replace-row write did not match the declaration"
    fi
    grep -q 'level 12 target totals' "$WORK/rep.log" \
        && pass "engagement: the write is logged" \
        || fail "engagement: no write logged — the leg proved nothing"
    n=$(grep -c 'level 12 target totals' "$WORK/rep.log")
    [ "$n" = "1" ] \
        && pass "idempotent: wrote once across 660 frames" \
        || fail "wrote $n times — the reconcile is not idempotent"
else
    fail "replace-row run produced no dump"
fi

echo
echo "[counts] slot row without a warp owns no index"
make_mod "$WORK/slot" slot "Slot Fixture" slot_fx dark_hollow 0 \
    "$(printf 'dragons = 9\ngems    = 555\neggs    = 3')"
if run "$WORK/slot" "$WORK/slot.bin" "$WORK/slot.log" 600 660; then
    if cmp_tables "$WORK/base.bin" "$WORK/slot.bin" none; then
        pass "an unclaimed slot row wrote nothing"
    else
        fail "slot row wrote a table it does not own"
    fi
    grep -q 'eggs = 3 dropped' "$WORK/slot.log" \
        && pass "eggs dropped by name: the free slot has no egg row" \
        || fail "eggs were not dropped for a slot-mode row"
    grep -q 'custom level "Slot Fixture" registered' "$WORK/slot.log" \
        && pass "the row still registered after the drop" \
        || fail "dropping eggs cost the whole row"
else
    fail "slot-row run produced no dump"
fi

echo
echo "[counts] warping a slot row declares index 35, never the egg table"
# The warp knob counts TICKS; --ram-dump-on-frame counts VBLs, which run
# at twice the tick rate. Dump well after tick 200 has been serviced.
if run "$WORK/slot" "$WORK/warp.bin" "$WORK/warp.log" 500 560 "200:Slot Fixture"; then
    if cmp_tables "$WORK/base.bin" "$WORK/warp.bin" "dragons:35,gems:35"; then
        pass "index 35 carries the declared totals; egg table untouched"
    else
        fail "slot claim did not write index 35 as declared"
    fi
    grep -q 'level 65 target totals' "$WORK/warp.log" \
        && pass "engagement: the slot write is logged" \
        || fail "engagement: the slot claim wrote nothing"
else
    fail "warp run produced no dump"
fi

echo
echo "[counts] two slot rows: only the row holding the slot declares"
make_mod "$WORK/two" a "Alpha Fixture" a_fx dark_hollow 0 \
    "$(printf 'dragons = 1\ngems    = 111')"
make_mod "$WORK/two" b "Beta Fixture" b_fx dark_hollow 0 \
    "$(printf 'dragons = 2\ngems    = 222')"
if run "$WORK/two" "$WORK/two.bin" "$WORK/two.log" 500 560 "200:Beta Fixture"; then
    if cmp_tables "$WORK/base.bin" "$WORK/two.bin" "dragons:35,gems:35"; then
        got=$(python3 -c "
import sys,struct
r=open('$WORK/two.bin','rb').read()
print(r[0x6F414+35], struct.unpack_from('<H',r,0x6F438+70)[0])")
        [ "$got" = "2 222" ] \
            && pass "the warped row won index 35 (got $got)" \
            || fail "index 35 holds $got, expected the warped row's 2 222"
    else
        fail "two slot rows did not resolve to a single index-35 write"
    fi
else
    fail "two-row run produced no dump"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "[counts] ALL GREEN"
    exit 0
fi
echo "[counts] $fails FAILED"
exit 1
