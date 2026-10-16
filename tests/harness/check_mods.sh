#!/usr/bin/env bash
# check_mods.sh - the PoC-ladder CI gate (modtest orchestration).
#
# Single-mod rungs declare their own tests in mod.toml [test] blocks and
# run through tools/modtest.py (the identical harness third-party modders
# use). This script adds the CROSS-mod matrix cases that no single
# manifest can express:
#   - chain-duo under both orderings (order IS priority; nesting flips)
#   - observer-neutrality with 4 mods stacked vs the PSX oracle
#   - deps topo-sort / missing-dep UX / dependency-cycle rejection
#   - crash attribution naming the offending mod
#   - decomp promotion: legacy raw alias resolves + warns (churn test)
# plus the tools/modtest.py run over every [test]-block rung.
#
# Usage: tests/harness/check_mods.sh [--game SLUG]
#        (repo root; ~10 min - ctest mod_poc_ladder)
# Env:   OPENPETE_BIN / OPENPETE_DISC - forwarded to runs + oracle.
#
# Fixture sets are per game. Every game runs the mod SDK and object
# identity rungs (tests/harness/mods_sdk_rungs.sh) and the rungs of the
# in-tree mods that load on every game (tests/harness/mods_multigame_rungs.sh:
# lib-and-client, arena-probe, the postfx mods, per-game asset roots,
# [[material]] and [[level]] rows by game). The rest of this
# script is Spyro 1's corpus: its fixtures, level names and golden are
# Spyro 1's, so it runs under --game spyro-1 only.

set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
# Every frame number the rungs pass is the feed key's (--frame-limit,
# --screenshot-on-frame, --state-*-on-frame, OPENPETE_S3_SCAN_POKE). Spyro 1 keys
# on openpete_frame; Spyro 2 and 3 key on openpete_vbl, which counts the boot's
# 573 fast-boot VBLs before the first yield, so a moment written as a Spyro 1
# yield count N is N + KEY0 there.
KEY0=0
case "$PG_GAME" in spyro-2|spyro-3) KEY0=573;; esac

MODS_SRC="$PWD/mods"
WORK="$(mktemp -d /tmp/openpete-modtest.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

BIN="$PG_BIN"
DISC="$PG_DISC"
SMOKE_FRAMES="${OPENPETE_MODTEST_FRAMES:-8000}"

# Fixture manifests pin the CURRENT api version (derived from the SDK
# header so an api bump can never silently disable the fixtures).
API_VER="$(sed -n 's/#define OPENPETE_MOD_API_VERSION \([0-9]*\)u/\1/p' \
           core/include/openpete_mod_api.h)"
[ -n "$API_VER" ] || { echo "cannot derive OPENPETE_MOD_API_VERSION" >&2; exit 1; }

fail=0
say()  { printf '\n[modtest] %s\n' "$*"; }
ok()   { printf '[modtest]   PASS: %s\n' "$*"; }
bad()  { printf '[modtest]   FAIL: %s\n' "$*"; fail=1; }

# Compose a fixture mods dir: symlink the named mods, write enabled.txt
# in the given order.
fixture() { # fixture <dir> <mod>...
    local dir="$1"; shift
    mkdir -p "$dir"
    : > "$dir/enabled.txt"
    local m
    for m in "$@"; do
        ln -sfn "$MODS_SRC/$m" "$dir/$m"
        echo "$m" >> "$dir/enabled.txt"
    done
}

# One headless run through pg_capture, which deletes the log and every named
# output before the run and asserts them after, so no leg can read the
# previous leg's capture.
#
# RUN_OUTS (comma-separated) names the files a leg consumes; RUN_TIMEOUT
# overrides the wall-clock ceiling for a leg that loads a level.
run_game() { # run_game <mods_dir> <frames> [extra flags...] -> stdout+stderr
    # NB: --frame-limit is the engine flag; OPENPETE_FRAME_LIMIT is a
    # check_psx_oracle.sh convention, NOT read by the engine itself.
    local dir="$1" frames="$2"
    shift 2
    local log; log="$(mktemp "$WORK/run.XXXXXX.log")"
    local PG_CAPTURE_TIMEOUT="${RUN_TIMEOUT:-300}"
    pg_capture "$log" "${RUN_OUTS:-}" -- \
        --frame-limit "$frames" --mods-dir "$dir" "$@"
    local rc=$?
    cat "$log"
    return $rc
}

# finish: the verdict line and exit status.
finish() {
    echo
    if [ "$fail" -eq 0 ]; then
        echo "[modtest] ALL GREEN"
    else
        echo "[modtest] FAILURES - see above"
    fi
    exit $fail
}

# ── 0. per-game fixture set: SDK tree, header edits, object identity ────
# shellcheck source=tests/harness/mods_sdk_rungs.sh
source tests/harness/mods_sdk_rungs.sh
# shellcheck source=tests/harness/mods_multigame_rungs.sh
source tests/harness/mods_multigame_rungs.sh
[ "$PG_GAME" = "spyro-1" ] || finish

# ── 1+2. chain-duo matrix (order A,B then B,A) - also observer-neutral ──
say "chain-duo matrix: order [a,b] (b outermost)"
fixture "$WORK/duo1" chain-duo-a chain-duo-b
out="$(run_game "$WORK/duo1" 5000)"
# Later in enabled.txt = higher priority: expect B> A> ... <A <B
seq1="$(printf '%s\n' "$out" | grep -oE 'chain (enter|exit)  ?[<>AB]+' | tr -d ' ' | paste -sd, -)"
if [ "$seq1" = "chainenterB>,chainenterA>,chainexit<A,chainexit<B" ]; then
    ok "nesting B{A{original}} ($seq1)"
else
    bad "unexpected nesting: $seq1"
fi

say "chain-duo matrix: order [b,a] (a outermost)"
fixture "$WORK/duo2" chain-duo-b chain-duo-a
out="$(run_game "$WORK/duo2" 5000)"
seq2="$(printf '%s\n' "$out" | grep -oE 'chain (enter|exit)  ?[<>AB]+' | tr -d ' ' | paste -sd, -)"
if [ "$seq2" = "chainenterA>,chainenterB>,chainexit<B,chainexit<A" ]; then
    ok "nesting A{B{original}} ($seq2)"
else
    bad "unexpected nesting: $seq2"
fi

# ── 2b. --mods overrides enabled.txt (same order-IS-priority rule) ──────
say "--mods override: file says [a,b], flag says [b,a] - flag wins"
fixture "$WORK/duo3" chain-duo-a chain-duo-b
out="$(run_game "$WORK/duo3" 5000 --mods chain-duo-b,chain-duo-a)"
seq3="$(printf '%s\n' "$out" | grep -oE 'chain (enter|exit)  ?[<>AB]+' | tr -d ' ' | paste -sd, -)"
if [ "$seq3" = "chainenterA>,chainenterB>,chainexit<B,chainexit<A" ]; then
    ok "--mods order replaced enabled.txt order ($seq3)"
else
    bad "--mods did not override enabled.txt: $seq3"
fi

# ── 2c. a final line with no trailing newline is still an id ───────────
# Editors that don't append a newline are common, and fgets reports that
# line the same way it reports one that outran the buffer - so the last
# mod the user added used to be dropped with a bogus "exceeds 63
# characters" warning.
say "enabled.txt: last line without a trailing newline still loads"
fixture "$WORK/duo4" chain-duo-a chain-duo-b
printf 'chain-duo-a\nchain-duo-b' > "$WORK/duo4/enabled.txt"
out="$(run_game "$WORK/duo4" 5000)"
seq4="$(printf '%s\n' "$out" | grep -oE 'chain (enter|exit)  ?[<>AB]+' | tr -d ' ' | paste -sd, -)"
if [ "$seq4" = "chainenterB>,chainenterA>,chainexit<A,chainexit<B" ] &&
   ! printf '%s\n' "$out" | grep -q "exceeds 63 characters"; then
    ok "newline-less final id loaded ($seq4)"
else
    bad "newline-less final id dropped or warned: $seq4"
fi

say "--mods \"\": empty list disables every mod"
out="$(run_game "$WORK/duo3" 600 --mods "")"
if printf '%s\n' "$out" | grep -q "none enabled" &&
   ! printf '%s\n' "$out" | grep -q "chain enter"; then
    ok "empty --mods left the run modless"
else
    bad "empty --mods still loaded mods"
fi

# ── 1. observer-neutrality oracle: all log-only mods stacked ────────────
say "observer-neutrality: hello-anchor + raw-probe + chain-duo vs oracle"
fixture "$WORK/observers" hello-anchor raw-probe chain-duo-a chain-duo-b
out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES tests/harness/check_psx_oracle.sh -- \
        --mods-dir "$WORK/observers" 2>&1)"
if printf '%s\n' "$out" | grep -q "verdict: PASS"; then
    ok "oracle PASS with 4 observer mods enabled"
else
    bad "oracle did not PASS with observers enabled"
    printf '%s\n' "$out" | grep -E "verdict|hard_fail" | head -3
fi
if printf '%s\n' "$out" | grep -q "raw probe #"; then
    ok "raw-probe fired (cpu-context args read)"
else
    bad "raw-probe never fired"
fi
if printf '%s\n' "$out" | grep -q "uses raw-tier addresses"; then
    ok "raw-tier warning printed"
else
    bad "raw-tier warning missing"
fi

# ── 5. crash-dummy: attribution names the offending mod ─────────────────
say "crash-dummy: deliberate segfault must be attributed to the mod"
fixture "$WORK/crash" crash-dummy
out="$(run_game "$WORK/crash" 5000)"; rc=$?
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ]; then
    ok "process died from the mod crash (rc=$rc)"
else
    bad "process did not crash (rc=$rc)"
fi
if printf '%s\n' "$out" | grep -q "MOD ATTRIBUTION: .* mod 'crash-dummy'"; then
    ok "crash report names crash-dummy"
else
    bad "attribution line missing"
    printf '%s\n' "$out" | grep -A3 "openpete crash" | head -6
fi

# ── 7. decomp promotion: legacy raw alias resolves + warns (churn test) ─
say "promotion contract: override via legacy raw name of a renamed fn"
mkdir -p "$WORK/aliassrc/alias-probe/src"
printf '[mod]\nid = "alias-probe"\nversion = "0.0.1"\napi = '"$API_VER"'\n' \
    > "$WORK/aliassrc/alias-probe/mod.toml"
cat > "$WORK/aliassrc/alias-probe/src/m.c" <<'EOF'
#include <openpete_mod_api.h>
static const openpete_mod_api_t* A; static openpete_mod_t* S; static int n;
static void fn(CPUState* cpu) {
    if (!n++) A->log(S, OP_MOD_LOG_INFO, "alias override fired");
    A->base(cpu);
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    A = api; S = self;
    /* func_8003B9D4 is CollectItem's historical raw name - the alias
     * table must resolve it forever (with a migrate warning). */
    return api->override_name(self, "func_8003B9D4", fn);
}
EOF
mkdir -p "$WORK/alias"
ln -sfn "$WORK/aliassrc/alias-probe" "$WORK/alias/alias-probe"
printf 'alias-probe\n' > "$WORK/alias/enabled.txt"
out="$(run_game "$WORK/alias" 8000)"
if printf '%s\n' "$out" | grep -q "is a legacy raw alias"; then
    ok "legacy-alias warning printed"
else
    bad "legacy-alias warning missing"
fi
if printf '%s\n' "$out" | grep -q "alias override fired"; then
    ok "chain attached through the historical name (promotion-safe)"
else
    bad "alias override never fired"
fi

# ── 7b. prebuilt objects: the ABI descriptor decides what may run ──────
# A binary-only mod (no src/, a prebuilt .build/<game>/<id>.so) is the one path
# compile-on-load cannot rebuild, so it is the path a stale vtable shape
# reaches the engine through. Three objects, compiled by hand against the
# staged SDK: no descriptor at all, a descriptor claiming a newer api, a
# descriptor from before the layout freeze (its calls would land on
# shifted slots), and the SDK's own stub - only the last may run.
say "prebuilt objects: ABI descriptor refuses stale and future objects"
SDK="$(dirname "$BIN")/sdk"
CC_MOD="${OPENPETE_MOD_CC:-cc}"
mkdir -p "$WORK/abisrc"
cat > "$WORK/abisrc/body.c" <<'EOF'
#include <openpete_mod_api.h>
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    api->log(self, OP_MOD_LOG_INFO, "ABI PROBE ENTRY RAN");
    return 0;
}
EOF
cat > "$WORK/abisrc/future.c" <<'EOF'
#include <openpete_mod_api.h>
const openpete_mod_abi_t openpete_mod_abi = {
    OPENPETE_MOD_ABI_MAGIC, OPENPETE_MOD_API_VERSION + 1u,
    (uint32_t)sizeof(openpete_mod_api_t) + 8u };
EOF
cat > "$WORK/abisrc/prefreeze.c" <<'EOF'
#include <openpete_mod_api.h>
const openpete_mod_abi_t openpete_mod_abi = {
    OPENPETE_MOD_ABI_MAGIC, 10u, (uint32_t)sizeof(openpete_mod_api_t) };
EOF
abi_fixture() { # abi_fixture <id> <extra .c or ""> - a binary-only mod dir
    local id="$1" extra="$2"
    local d="$WORK/abi/$id"
    mkdir -p "$d/.build/$PG_GAME"
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = %s\n' "$id" "$API_VER" \
        > "$d/mod.toml"
    # shellcheck disable=SC2086
    "$CC_MOD" -shared -fPIC -O2 -fno-strict-aliasing \
        -I"$SDK/games/$PG_GAME" -I"$SDK" \
        -o "$d/.build/$PG_GAME/$id.so" "$WORK/abisrc/body.c" $extra \
        > "$WORK/abi-$id.cc.log" 2>&1 || bad "abi fixture $id did not compile"
}
abi_fixture abi-none   ""
abi_fixture abi-future "$WORK/abisrc/future.c"
abi_fixture abi-prefreeze "$WORK/abisrc/prefreeze.c"
abi_fixture abi-ok     "$SDK/openpete_mod_abi.c"
printf 'abi-none\nabi-future\nabi-prefreeze\nabi-ok\n' > "$WORK/abi/enabled.txt"
out="$(run_game "$WORK/abi" 600)"
if printf '%s\n' "$out" | grep -q "abi-none: .*carries no ABI descriptor"; then
    ok "no-descriptor object refused"
else
    bad "no-descriptor object was not refused"
fi
if printf '%s\n' "$out" | grep -q "abi-future: .*built for api $((API_VER + 1))"; then
    ok "future-api object refused (names both versions)"
else
    bad "future-api object was not refused"
fi
if printf '%s\n' "$out" | grep -q "abi-prefreeze: .*built for api 10, but the api struct layout changed at api 11"; then
    ok "pre-freeze object refused (layout floor)"
else
    bad "pre-freeze (api 10) object was not refused"
fi
if [ "$(printf '%s\n' "$out" | grep -c "ABI PROBE ENTRY RAN")" = 1 ] &&
   printf '%s\n' "$out" | grep -q "loaded abi-ok"; then
    ok "descriptored object ran, and only it"
else
    bad "entry ran $(printf '%s\n' "$out" | grep -c 'ABI PROBE ENTRY RAN') times (want 1)"
    printf '%s\n' "$out" | grep "mods:" | head -6
fi

# ── 7c. a failed entry leaves nothing registered ───────────────────────
# An entry that installs an override and THEN returns nonzero is disabled
# - and its override must go with it, or a mod the loader reports as
# disabled keeps running code on every tick.
say "failed entry: registrations made before the failure are swept"
mkdir -p "$WORK/failsrc/entry-fails/src"
printf '[mod]\nid = "entry-fails"\nversion = "0.0.1"\napi = %s\n' "$API_VER" \
    > "$WORK/failsrc/entry-fails/mod.toml"
cat > "$WORK/failsrc/entry-fails/src/m.c" <<'EOF'
#include <openpete_mod_api.h>
static const openpete_mod_api_t* A; static openpete_mod_t* S;
static void fn(CPUState* cpu) {
    A->log(S, OP_MOD_LOG_INFO, "ORPHAN OVERRIDE FIRED");
    A->base(cpu);
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    A = api; S = self;
    api->override_name(self, "CollectItem", fn);
    return -1; /* registered, then failed */
}
EOF
mkdir -p "$WORK/fail"
ln -sfn "$WORK/failsrc/entry-fails" "$WORK/fail/entry-fails"
printf 'entry-fails\n' > "$WORK/fail/enabled.txt"
out="$(run_game "$WORK/fail" 8000)"
if printf '%s\n' "$out" | grep -q "entry-fails: openpete_mod_entry returned -1"; then
    ok "failed entry reported"
else
    bad "failed entry not reported"
fi
if ! printf '%s\n' "$out" | grep -q "ORPHAN OVERRIDE FIRED"; then
    ok "override registered before the failure never fired"
else
    bad "override from a failed entry is still live"
fi

# ── 8. asset overlay: texture namespace scan + overlay-duel ─────────────
say "asset overlay: mod texture joins the pack (namespace scan)"
fixture "$WORK/tex" hd-single-texture
out="$(run_game "$WORK/tex" 300 \
        --screenshot-on-frame 200 \
        --screenshot-output "$WORK/tex-{n}.png")"
if printf '%s\n' "$out" | grep -q "\[texpack\] + mod hd-single-texture: 1 entry"; then
    ok "mod texture scanned into the replacement pack"
else
    bad "mod texture scan line missing"
    printf '%s\n' "$out" | grep -i texpack | head -3
fi

say "asset overlay: symbolic name binds to content, not to the VRAM rect"
# The title UI atlas and every level's texture page upload to the same
# 512x256 block at VRAM (512,256). The annotation must bind title_ui to
# the atlas content so a level page landing on the rect stays unnamed.
out="$(OPENPETE_TEXPACK_DUMP_REGISTRY=1 RUN_TIMEOUT=600 \
        run_game "$WORK/tex" 3000 --skip-to-level artisans \
        --screenshot-on-frame 2900 \
        --screenshot-output "$WORK/tex7d-{n}.png")"
bound="$(printf '%s\n' "$out" | sed -n 's/.*annotate title_ui: bound to thash=\([0-9a-f]*\).*/\1/p' | head -1)"
if [ -n "$bound" ]; then
    ok "title_ui annotation bound to content $bound"
else
    bad "title_ui annotation never bound (engagement missing)"
    printf '%s\n' "$out" | grep -i "annotate" | head -3
fi
named="$(printf '%s\n' "$out" | grep -c 'asset=title_ui' || true)"
stray="$(printf '%s\n' "$out" | grep 'asset=title_ui' | grep -vc "thash=$bound" || true)"
rect="$(printf '%s\n' "$out" | grep -c '\[texpack\] entry 512,256 512x256' || true)"
if [ "$rect" -ge 1 ] && [ "$stray" -eq 0 ]; then
    ok "after the Artisans load the rect holds $rect entry, $named named title_ui, none with another hash"
else
    bad "title_ui over-claims: rect entries=$rect named=$named other-hash-named=$stray"
    printf '%s\n' "$out" | grep '\[texpack\] entry 512,256' | head -3
fi

say "asset overlay: two mods collide on one asset - last in order wins"
for m in duel-a duel-b; do
    mkdir -p "$WORK/duelsrc/$m/assets/textures"
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = '"$API_VER"'\n' "$m" \
        > "$WORK/duelsrc/$m/mod.toml"
    python3 - "$WORK/duelsrc/$m/assets/textures/title_ui.png" <<'PY'
import sys, zlib, struct
w = h = 8
raw = b''.join(b'\x00' + b'\xff\x00\x00\xff' * w for _ in range(h))
def chunk(t, d):
    return struct.pack('>I', len(d)) + t + d + \
           struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + \
      chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
PY
done
mkdir -p "$WORK/duel"
ln -sfn "$WORK/duelsrc/duel-a" "$WORK/duel/duel-a"
ln -sfn "$WORK/duelsrc/duel-b" "$WORK/duel/duel-b"
run_duel() { # run_duel -> stdout+stderr (short native-init headless run)
    run_game "$WORK/duel" 300 \
        --screenshot-on-frame 200 \
        --screenshot-output "$WORK/duel-{n}.png"
}
printf 'duel-a\nduel-b\n' > "$WORK/duel/enabled.txt"
out="$(run_duel)"
if printf '%s\n' "$out" | grep -q 'mod duel-b overrides "title_ui" (was mod duel-a)'; then
    ok "later-enabled mod wins the asset (b over a)"
else
    bad "duel resolution wrong for order [a,b]"
    printf '%s\n' "$out" | grep -i texpack | head -3
fi
printf 'duel-b\nduel-a\n' > "$WORK/duel/enabled.txt"
out="$(run_duel)"
if printf '%s\n' "$out" | grep -q 'mod duel-a overrides "title_ui" (was mod duel-b)'; then
    ok "flipping the order flips the winner (a over b)"
else
    bad "duel resolution wrong for order [b,a]"
fi

# ── 8b. asset overlay: sound namespace (assets/sfx/) ─────────────────
# Two declarative mods each ship a symbolic spyroCharge.wav (synthesized
# tones - Spyro charges constantly in the attract demo, gems are never
# collected there). Asserts: a file replaces a keyed voice; last in order
# wins with the same log line the texture namespace prints; flipping the
# order flips the winner. Then the round-trip instrument: a fresh census
# of the disc loaded unmodified must verify bit-identical at every key-on.
say "asset overlay: sound namespace - symbolic replacement + last-wins"
for m in sfxduel-a sfxduel-b; do
    mkdir -p "$WORK/sfxsrc/$m/assets/sfx"
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = '"$API_VER"'\n' "$m" \
        > "$WORK/sfxsrc/$m/mod.toml"
    python3 - "$WORK/sfxsrc/$m/assets/sfx/spyroCharge.wav" "$m" <<'PY'
import math, struct, sys, wave
f = 440.0 if sys.argv[2].endswith('a') else 660.0
rate = 22050; n = int(rate * 0.25)
pcm = b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * f * i / rate))) for i in range(n))
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
w.writeframes(pcm); w.close()
PY
done
mkdir -p "$WORK/sfxduel"
ln -sfn "$WORK/sfxsrc/sfxduel-a" "$WORK/sfxduel/sfxduel-a"
ln -sfn "$WORK/sfxsrc/sfxduel-b" "$WORK/sfxduel/sfxduel-b"
printf 'sfxduel-a\nsfxduel-b\n' > "$WORK/sfxduel/enabled.txt"
out="$(run_game "$WORK/sfxduel" 6000)"
if printf '%s\n' "$out" | grep -q 'mod sfxduel-b overrides "spyroCharge.wav" (was mod sfxduel-a)'; then
    ok "later-enabled mod wins the sound (b over a)"
else
    bad "sound duel resolution wrong for order [a,b]"
    printf '%s\n' "$out" | grep -i sfxpack | head -3
fi
replaced="$(printf '%s\n' "$out" | grep -o '\[sfxpack\].*replaced=[0-9]*' | grep -o 'replaced=[0-9]*' | cut -d= -f2)"
if [ "${replaced:-0}" -ge 1 ]; then
    ok "symbolic spyroCharge.wav replaced a keyed voice (replaced=$replaced)"
else
    bad "no voice was replaced by the sound mod"
    printf '%s\n' "$out" | grep "sfxpack" | head -3
fi
printf 'sfxduel-b\nsfxduel-a\n' > "$WORK/sfxduel/enabled.txt"
out="$(run_game "$WORK/sfxduel" 6000)"
if printf '%s\n' "$out" | grep -q 'mod sfxduel-a overrides "spyroCharge.wav" (was mod sfxduel-b)'; then
    ok "flipping the order flips the sound winner (a over b)"
else
    bad "sound duel resolution wrong for order [b,a]"
fi

say "sfx pack round-trip: a fresh census loaded unmodified verifies bit-identical"
if op_own_run timeout -k 10 300 "$BIN" --disc "$DISC" --disc-cache 0 --dump-sfx-pack "$WORK/sfxdump" > "$WORK/sfxdump.log" 2>&1 &&
   grep -q "no_end=0 failed=0" "$WORK/sfxdump.log"; then
    ok "census: $(grep -o 'entries=[0-9]* sources=[0-9]* dumped=[0-9]*' "$WORK/sfxdump.log")"
    mkdir -p "$WORK/nomods"; : > "$WORK/nomods/enabled.txt"
    out="$(OPENPETE_SFXPACK_VERIFY=1 run_game "$WORK/nomods" 6000 --sfxpack-dir "$WORK/sfxdump")"
    line="$(printf '%s\n' "$out" | grep '^\[sfxpack\]')"
    keyed="$(printf '%s' "$line" | grep -o 'keyed=[0-9]*' | cut -d= -f2)"
    vok="$(printf '%s' "$line" | grep -o 'verify_ok=[0-9]*' | cut -d= -f2)"
    vbad="$(printf '%s' "$line" | grep -o 'verify_bad=[0-9]*' | cut -d= -f2)"
    if [ "${keyed:-0}" -ge 1 ] && [ "${vok:-0}" = "${keyed:-1}" ] && [ "${vbad:-1}" = 0 ]; then
        ok "every key-on replaced and verified ($line)"
    else
        bad "round-trip not bit-identical: $line"
    fi
else
    bad "census dump failed"; tail -3 "$WORK/sfxdump.log"
fi

# ── 9. level-segment overlay: CdRead chokepoint ─────────────────────────
say "wad overlay: byte-identical blob - chokepoint provably transparent"
mkdir -p "$WORK/wadsrc/wad-identity/assets/wad"
printf '[mod]\nid = "wad-identity"\nversion = "0.0.1"\napi = '"$API_VER"'\n' \
    > "$WORK/wadsrc/wad-identity/mod.toml"
if python3 tools/extract_wad_entry.py --disc "$DISC" --level 11 \
        --out "$WORK/wadsrc/wad-identity/assets/wad/level11.data.bin" \
        >/dev/null 2>&1; then
    mkdir -p "$WORK/wadid"
    ln -sfn "$WORK/wadsrc/wad-identity" "$WORK/wadid/wad-identity"
    printf 'wad-identity\n' > "$WORK/wadid/enabled.txt"
    out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES tests/harness/check_psx_oracle.sh -- \
            --mods-dir "$WORK/wadid" 2>&1)"
    if printf '%s\n' "$out" | grep -q "wad overlay: level 11 data served from wad-identity"; then
        ok "level-11 (demo 1) data served through the CdRead chokepoint"
    else
        bad "wad overlay never served"
        printf '%s\n' "$out" | grep -i wadmod | head -3
    fi
    if printf '%s\n' "$out" | grep -q "verdict: PASS"; then
        ok "byte-identical override is oracle-exact (transparent chokepoint)"
    else
        bad "oracle did not PASS with the identity blob"
    fi
else
    bad "extract_wad_entry.py failed against $DISC"
fi

say "wad overlay: recoloured blob loads clean (bg-colour patch)"
mkdir -p "$WORK/wadsrc/wad-recolor/assets/wad"
printf '[mod]\nid = "wad-recolor"\nversion = "0.0.1"\napi = '"$API_VER"'\n' \
    > "$WORK/wadsrc/wad-recolor/mod.toml"
if python3 tools/extract_wad_entry.py --disc "$DISC" --level 10 \
        --recolor-bg 255,0,255 \
        --out "$WORK/wadsrc/wad-recolor/assets/wad/level10.data.bin" \
        >/dev/null 2>&1; then
    mkdir -p "$WORK/wadrc"
    ln -sfn "$WORK/wadsrc/wad-recolor" "$WORK/wadrc/wad-recolor"
    printf 'wad-recolor\n' > "$WORK/wadrc/enabled.txt"
    out="$(run_game "$WORK/wadrc" 2600 --skip-to-level 10)"
    if printf '%s\n' "$out" | grep -q "wad overlay: level 10 data served from wad-recolor"; then
        ok "recoloured blob served"
    else
        bad "recoloured blob not served"
    fi
    if printf '%s\n' "$out" | grep -q "openpete-exit: reason=frame-limit"; then
        ok "level loaded and ran clean with the edited blob"
    else
        bad "run did not reach the frame limit with the edited blob"
    fi
else
    bad "extract_wad_entry.py --recolor-bg failed"
fi

# ── 12. [test]-block rungs through the real modtest runner ──────────────
say "[test] blocks via tools/modtest.py (the harness modders run)"
# Every mod that declares a modtest.toml runs, so adding one to a mod is
# all it takes to enter the gate. Sorted under LC_ALL=C so the run order
# is the same on every machine. A mod whose
# assets are derived locally and never committed declares `requires` and
# the runner skips it by name; the runner exits non-zero if the whole
# list executed nothing. MODTEST_JOBS (default 2) is the runner's --jobs:
# rungs run that many at once, each one game process.
modtest_mods=()
while IFS= read -r manifest; do
    modtest_mods+=("$(dirname "$manifest")")
done < <(find mods -mindepth 2 -maxdepth 2 -name modtest.toml | LC_ALL=C sort)
if [ "${#modtest_mods[@]}" -eq 0 ]; then
    bad "no mods/*/modtest.toml found"
elif python3 tools/modtest.py --game "$PG_GAME" --jobs "${MODTEST_JOBS:-2}" \
        "${modtest_mods[@]}"; then
    ok "modtest runner: ${#modtest_mods[@]} [test] blocks green"
else
    bad "modtest runner reported failures"
fi

# ── 13. arena timeline canary: guest_alloc memory IS timeline state ───
# arena-canary mirrors g_GameTick in a MUTABLE arena block and logs
# DESYNC whenever guest RAM moves back in time without the arena; it also
# holds blocks in the lower DMA-reachable segment and the general pool,
# whose copies of the mirror log DESYNC when a range moves without the
# others. The file load restores all three ranges (64 B mirror plus a
# 4 MiB filler upper, 5 MiB lower, 64 KiB general: 9502784 B). Every
# timeline feature must restore both together: the stress-rewind
# harness, runahead rollback (with the prediction oracle hashing the
# arena), a file save/load, and the reload cases - an Apply that keeps
# the allocation shape replays the row and a pre-Apply file restores;
# an Apply that changes the shape supersedes the row and the pre-Apply
# file is REFUSED. A modset mismatch refuses too.
say "arena canary: mutable arena memory rides rewind / runahead / savestates"
fixture "$WORK/canary" arena-canary
out="$(run_game "$WORK/canary" 7000 --state-stress-rewind 25)"
if printf '%s\n' "$out" | grep -q "arena-canary: armed" && \
   printf '%s\n' "$out" | grep -q "stress-rewind: 1 rollbacks"; then
    ok "canary armed and the harness rolled the timeline back (engaged)"
else
    bad "canary never armed or the stress harness never rolled back"
fi
if printf '%s\n' "$out" | grep -q "arena-canary: pools placed"; then
    ok "canary holds blocks in the upper and lower segments and the general pool"
else
    bad "canary pool blocks missing or placed outside their ranges"
fi
if printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    bad "stress-rewind: DESYNC - the arena did not roll back with guest RAM"
else
    ok "stress-rewind: arena rolled back with guest RAM (no DESYNC)"
fi
# The window is a loaded level, not the boot/attract window the other
# canary runs use: runahead only speculates while gameplay is live
# (g_Gamestate == GS_Playing and a non-zero level id), and a prediction
# is retired only when the committed timeline reaches the frame it was
# recorded for. Booting to the attract demo reaches gameplay so late
# that the run ends before any prediction retires, so the oracle
# retires zero and the leg proves nothing.
out="$(OPENPETE_RA_PREDICT_CHECK=1 RUN_TIMEOUT=900 \
    run_game "$WORK/canary" 3000 --runahead 1 --skip-to-level 10)"
ra_sum="$(printf '%s\n' "$out" |
          grep -oE '\[ra-predict\] checked=[0-9]+ mismatch=[0-9]+' | tail -1)"
ra_checked="${ra_sum#*checked=}"; ra_checked="${ra_checked%% *}"
ra_mismatch="${ra_sum##*mismatch=}"
# Each arm names the condition that failed: one combined pattern cannot
# distinguish "never speculated" from "predicted wrong", and a window
# that retires no prediction must read as VACUOUS, not as a pass.
if ! printf '%s\n' "$out" | grep -q "runahead: armed"; then
    bad "runahead over the canary: runahead never armed"
elif ! printf '%s\n' "$out" | grep -q "arena-canary: armed"; then
    bad "runahead over the canary: the canary never armed (no gameplay tick)"
elif [ -z "$ra_sum" ]; then
    bad "runahead over the canary: VACUOUS - no [ra-predict] summary; the" \
        "prediction oracle was not armed in this binary"
elif [ "${ra_checked:-0}" -eq 0 ]; then
    bad "runahead over the canary: VACUOUS - the prediction oracle retired" \
        "0 predictions in this window; nothing was compared"
elif [ "${ra_mismatch:-1}" -ne 0 ]; then
    bad "runahead over the canary: prediction oracle mismatch ($ra_sum)"
elif printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    bad "runahead over the canary: DESYNC - the arena did not roll back" \
        "with guest RAM"
else
    ok "runahead armed over mutable arena memory; prediction oracle exact" \
       "($ra_checked prediction(s) retired, 0 mismatch); no DESYNC"
fi
# The file legs below run in one loaded level's gameplay. A load applies
# only while the game fiber is parked at the psyq_VSync call site its
# state was saved at, and each game state draws through its own VSync
# call, so a save and a load in different game states (the attract
# demo's level changes, the title screen at boot) never meet: the load
# waits 1800 final yields and is refused for the site, before the modset
# and ledger verdicts run. --skip-to-level 10 reaches gameplay near
# --state-save-on-frame 1200 and Spyro then stands idle at one site, so
# every save and load here, with the config Apply at tick 1400 between
# them, shares it.
rm -f "$WORK/canary.state"
out="$(run_game "$WORK/canary" 2600 --skip-to-level 10 \
        --state-save-on-frame 2000 --state-save-output "$WORK/canary.state")"
if printf '%s\n' "$out" | grep -q "savestate: saved" && [ -e "$WORK/canary.state" ]; then
    ok "file save with mutable arena memory: written"
else
    bad "file save with mutable arena memory failed"
fi
out="$(run_game "$WORK/canary" 3000 --skip-to-level 10 \
        --state-load-on-frame 2400 --state-load-input "$WORK/canary.state")"
if printf '%s\n' "$out" | grep -q "arena ledger applied: .* 9502784 B restored" && \
   printf '%s\n' "$out" | grep -q "savestate: loaded" && \
   ! printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    ok "file load restored the arena with guest RAM (no DESYNC)"
else
    bad "file load: arena not restored, or DESYNC after the load"
fi
fixture "$WORK/canary-ms" double-gems
out="$(run_game "$WORK/canary-ms" 2700 --skip-to-level 10 \
        --state-load-on-frame 2200 --state-load-input "$WORK/canary.state")"
if printf '%s\n' "$out" | grep -q "MODSET MISMATCH.*load refused"; then
    ok "modset mismatch: load refused, running state untouched"
else
    bad "modset mismatch was not refused"
fi
# Reload cases need a writable copy: Apply rewrites config.toml.
for v in same diff; do
    rm -rf "$WORK/canary-$v"; mkdir -p "$WORK/canary-$v"
    cp -r "$MODS_SRC/arena-canary" "$WORK/canary-$v/arena-canary"
    rm -rf "$WORK/canary-$v/arena-canary/.build"
    echo arena-canary > "$WORK/canary-$v/enabled.txt"
done
rm -f "$WORK/same.state" "$WORK/diff.state"
out="$(OPENPETE_MODS_CFG_APPLY=1400:arena-canary:slots=4 \
       run_game "$WORK/canary-same" 3300 --skip-to-level 10 \
        --state-save-on-frame 2000 --state-save-output "$WORK/same.state" \
        --state-load-on-frame 2800 --state-load-input "$WORK/same.state")"
if printf '%s\n' "$out" | grep -q "arena-canary call #0 replayed" && \
   printf '%s\n' "$out" | grep -q "savestate: loaded" && \
   ! printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    ok "Apply (same shape): allocation replayed, pre-Apply file restored faithfully"
else
    bad "Apply (same shape): no replay, load refused, or DESYNC"
fi
out="$(OPENPETE_MODS_CFG_APPLY=1400:arena-canary:slots=8 \
       run_game "$WORK/canary-diff" 3300 --skip-to-level 10 \
        --state-save-on-frame 2000 --state-save-output "$WORK/diff.state" \
        --state-load-on-frame 2800 --state-load-input "$WORK/diff.state")"
if printf '%s\n' "$out" | grep -q "row #0 superseded" && \
   printf '%s\n' "$out" | grep -q 'entry #0 .* was superseded by a reload .* REFUSING the load'; then
    ok "Apply (changed shape): row superseded, pre-Apply file REFUSED naming the mod and entry"
else
    bad "Apply (changed shape): file written against a superseded row was not refused"
fi
# A file the running mods can never load is refused before the park-site
# wait. Loaded at frame 1000 of the attract run, where the fiber is not
# parked at the files' in-level site, a modset mismatch (canary.state
# under double-gems) and a ledger prefix mismatch (diff.state, saved with
# slots=4, under the slots=8 config the Apply above wrote) are each
# refused at the first final yield: the census counts no park-site
# deferral and 0 final yields waited. A refusal held behind the site wait
# instead logs no verdict within the run and counts the yields it waited.
early_refusal() { # early_refusal <label> <mods_dir> <state> <verdict regex>
    local o
    o="$(run_game "$2" 1500 --state-load-on-frame 1000 --state-load-input "$3")"
    local census; census="$(printf '%s\n' "$o" | grep -o 'park site: loads=[0-9]* ([0-9]* final yield(s) waited)')"
    if printf '%s\n' "$o" | grep -q "$4" && \
       [ "$census" = "park site: loads=0 (0 final yield(s) waited)" ] && \
       ! printf '%s\n' "$o" | grep -q "savestate: loaded"; then
        ok "$1 at another park site: refused at once ($census)"
    else
        bad "$1 at another park site: not refused before the site wait (${census:-no census})"
    fi
}
early_refusal "modset mismatch" "$WORK/canary-ms" "$WORK/canary.state" \
    "MODSET MISMATCH.*load refused"
early_refusal "ledger prefix mismatch" "$WORK/canary-diff" "$WORK/diff.state" \
    "arena ledger: row #0 diverges.*REFUSING"

# ── 13c. rewind keyframes share arena pages ─────────────────────────────
# A rewind keyframe stores the arena as reference-counted 4 KiB pages: a
# full copy at the keyframe, then a compare against the newest other valid
# keyframe, sliced over the ticks before the next one, that shares equal
# pages. Rung 13's --state-stress-rewind never enters the keyframe ring,
# so these legs drive --rewind with a held key over arena-canary (9.5 MB
# used, mostly static) and read the ring's exit counters (`rewind: arena
# pages:`). OPENPETE_REWIND_HOLD_PHASE starts the hold at a fixed VBL
# after a keyframe: 0 is the keyframe's own iteration, whose seek restores
# fully compared keyframes only; 10 restores the newest keyframe while its
# compare is still running, then discards that compare when the seek moves
# behind it. Every leg also requires no canary DESYNC.
say "rewind keyframes: arena pages shared, restores exact"
rw_field() { # rw_field <log text> <counter name> -> value (empty when absent)
    printf '%s\n' "$1" | grep -o "rewind: arena pages: .*" | tail -1 |
        grep -oE "(^| )$2=[0-9]+" | tail -1 | sed 's/.*=//'
}
rw_hold_leg() { # rw_hold_leg <label> <phase> <pending-restore check: zero|nonzero>
    local o sh rs pr dc
    o="$(OPENPETE_REWIND_HOLD_PHASE=$2 RUN_TIMEOUT=900 run_game "$WORK/canary" 3400 \
        --skip-to-level 10 --rewind 30 --rewind-hold 2200:40)"
    sh="$(rw_field "$o" shared)"; rs="$(rw_field "$o" restores)"
    pr="$(rw_field "$o" pending-restores)"; dc="$(rw_field "$o" discarded)"
    if ! printf '%s\n' "$o" | grep -q "hold phase met" || \
       ! printf '%s\n' "$o" | grep -q "hold 40/40"; then
        bad "$1: VACUOUS - the hold never started at its phase or never finished"
    elif [ "${sh:-0}" -eq 0 ] || [ "${rs:-0}" -eq 0 ]; then
        bad "$1: VACUOUS - shared=${sh:-none} restores=${rs:-none}"
    elif printf '%s\n' "$o" | grep -q "arena-canary: DESYNC"; then
        bad "$1: DESYNC after a keyframe restore"
    elif [ "$3" = zero ] && [ "${pr:-1}" -ne 0 ]; then
        bad "$1: the hold restored a partly compared keyframe (pending-restores=$pr)"
    elif [ "$3" = nonzero ] && { [ "${pr:-0}" -eq 0 ] || [ "${dc:-0}" -eq 0 ]; }; then
        bad "$1: VACUOUS - pending-restores=${pr:-none} discarded=${dc:-none}"
    else
        ok "$1: shared=$sh restores=$rs pending-restores=$pr discarded=$dc, no DESYNC"
    fi
}
rw_hold_leg "hold on a keyframe iteration" 0 zero
rw_hold_leg "hold 10 VBLs after a keyframe" 10 nonzero
# --rewind 1 holds two keyframes, so each new keyframe overwrites the
# other slot's predecessor; the hold runs back past the oldest. A compare
# target is never pending, invalid, or newer than the new keyframe.
o="$(OPENPETE_REWIND_HOLD_PHASE=10 RUN_TIMEOUT=900 run_game "$WORK/canary" 3400 \
    --skip-to-level 10 --rewind 1 --rewind-hold 2200:120)"
cb="$(rw_field "$o" compares)"; pt="$(rw_field "$o" pending-target)"
bt="$(rw_field "$o" bad-target)"; nt="$(rw_field "$o" no-target)"
if [ "${cb:-0}" -eq 0 ] || [ -z "$pt" ] || [ -z "$bt" ]; then
    bad "--rewind 1: VACUOUS - compares=${cb:-none}"
elif [ "$pt" -ne 0 ] || [ "$bt" -ne 0 ]; then
    bad "--rewind 1: compared against a pending ($pt) or dropped/overwritten ($bt) slot"
elif printf '%s\n' "$o" | grep -q "arena-canary: DESYNC"; then
    bad "--rewind 1: DESYNC after a keyframe restore"
else
    ok "--rewind 1 + hold past the oldest: compares=$cb no-target=$nt pending-target=0 bad-target=0"
fi
# Turning rewind off through the live knob releases the page store.
o="$(OPENPETE_REWIND_OFF_AT=3000 RUN_TIMEOUT=900 run_game "$WORK/canary" 3400 \
    --skip-to-level 10 --rewind 30)"
rel="$(printf '%s\n' "$o" | grep -oE 'page store [0-9]+ slab\(s\) \([0-9]+ MiB\) unmapped; RSS [0-9]+ -> [0-9]+' | tail -1)"
if [ -z "$rel" ]; then
    bad "rewind off: no page-store release line"
else
    slabs="$(printf '%s' "$rel" | sed -E 's/page store ([0-9]+) .*/\1/')"
    rss0="$(printf '%s' "$rel" | sed -E 's/.*RSS ([0-9]+) -> .*/\1/')"
    rss1="$(printf '%s' "$rel" | sed -E 's/.*-> ([0-9]+)$/\1/')"
    if [ "$slabs" -gt 0 ] && [ $((rss0 - rss1)) -ge $((slabs * 2048 * 9 / 10)) ]; then
        ok "rewind off: $slabs slab(s) unmapped, RSS $rss0 -> $rss1 KiB"
    else
        bad "rewind off: RSS did not fall by the store ($rel)"
    fi
fi
# A file load replaces the timeline and clears the ring; the keyframes
# taken after it compare and restore the loaded arena bytes.
o="$(OPENPETE_REWIND_HOLD_PHASE=10 RUN_TIMEOUT=900 run_game "$WORK/canary" 3600 \
    --skip-to-level 10 --rewind 30 \
    --state-load-on-frame 2400 --state-load-input "$WORK/canary.state" \
    --rewind-hold 3000:40)"
sh="$(rw_field "$o" shared)"; rs="$(rw_field "$o" restores)"
if ! printf '%s\n' "$o" | grep -q "savestate: loaded" || [ "${rs:-0}" -eq 0 ] || \
   [ "${sh:-0}" -eq 0 ]; then
    bad "rewind across a file load: VACUOUS - load, restores=${rs:-none} or shared=${sh:-none} missing"
elif printf '%s\n' "$o" | grep -q "arena-canary: DESYNC"; then
    bad "rewind across a file load: DESYNC after a keyframe restore"
else
    ok "rewind across a file load: restores=$rs shared=$sh, no DESYNC"
fi

# ── 13b. stream handles: every 21-bit field reader resolves handles ───
# vstream-rehandle rewrites EVERY animated model's frame words in the level
# to stream handles - mode 1 bound to the streams in place, mode 2 bound to
# arena copies (the total-conversion shape). The rendered frame must equal
# stock byte-for-byte: the readers are the 21 substituted sites plus the
# native walks, so a pixel difference is a reader that resolved a handle
# differently from an address. Engagement: the census line must show
# handle resolutions and no unresolved field.
say "stream handles: rehandled level renders pixel-identical to stock"
SHOT_FRAME=2800
for v in stock mode1 mode2; do
    rm -rf "$WORK/vh-$v"; mkdir -p "$WORK/vh-$v"
    : > "$WORK/vh-$v/enabled.txt"
done
cp -r "$MODS_SRC/vstream-rehandle" "$WORK/vh-mode1/vstream-rehandle"
cp -r "$MODS_SRC/vstream-rehandle" "$WORK/vh-mode2/vstream-rehandle"
rm -rf "$WORK"/vh-mode*/vstream-rehandle/.build
echo vstream-rehandle > "$WORK/vh-mode1/enabled.txt"
echo vstream-rehandle > "$WORK/vh-mode2/enabled.txt"
printf 'mode = 2\n' > "$WORK/vh-mode2/vstream-rehandle/config.toml"
vh_run() { # vh_run <variant> -> log on stdout; the native capture lands
           # at $WORK/vh-<variant>-vk.png (the renderer suffixes its variant)
    rm -f "$WORK/vh-$1-vk.png"
    run_game "$WORK/vh-$1" 3000 --skip-to-level stone_hill \
        --screenshot-on-frame $SHOT_FRAME --screenshot-native-only \
        --screenshot-output "$WORK/vh-$1.png"
}
out_stock="$(vh_run stock)"
out_m1="$(vh_run mode1)"
out_m2="$(vh_run mode2)"
if [ -s "$WORK/vh-stock-vk.png" ] && [ -s "$WORK/vh-mode1-vk.png" ] && [ -s "$WORK/vh-mode2-vk.png" ]; then
    ok "three captures written"
else
    bad "a capture is missing (stock/mode1/mode2)"
fi
for v in m1 m2; do
    eval "o=\$out_$v"
    if printf '%s\n' "$o" | grep -q "vstream-rehandle: level rehandled" && \
       printf '%s\n' "$o" | grep -qE "\[vstream\] stock=[0-9]+ handle=[1-9][0-9]* unresolved=0 "; then
        ok "$v: level rehandled; handle resolutions counted, none unresolved"
    else
        bad "$v: fixture never engaged, or a field resolved to nothing"
    fi
done
if cmp -s "$WORK/vh-stock-vk.png" "$WORK/vh-mode1-vk.png"; then
    ok "mode 1 (handles in place): pixel-identical to stock"
else
    bad "mode 1 (handles in place): DIFFERS from stock"
fi
if cmp -s "$WORK/vh-stock-vk.png" "$WORK/vh-mode2-vk.png"; then
    ok "mode 2 (streams in the arena): pixel-identical to stock"
else
    bad "mode 2 (streams in the arena): DIFFERS from stock"
fi

# supercharge-anywhere injects a captured Spyro animation whose frame words
# are halved-encoding fields: capture on a ramp level, then inject on a
# level without one - every frame word must carry a handle.
say "stream handles: supercharge injection carries halved-encoding handles"
rm -rf "$WORK/sc"; mkdir -p "$WORK/sc"
cp -r "$MODS_SRC/supercharge-anywhere" "$WORK/sc/supercharge-anywhere"
rm -rf "$WORK/sc/supercharge-anywhere/.build" "$WORK/sc/supercharge-anywhere/data"
echo supercharge-anywhere > "$WORK/sc/enabled.txt"
# The user's config.toml may ship the force switched off; the injection
# only runs while it is on.
printf 'start_enabled = true\n' > "$WORK/sc/supercharge-anywhere/config.toml"
out="$(run_game "$WORK/sc" 3000 --skip-to-level tree_tops)"
if printf '%s\n' "$out" | grep -q "captured authentic anim-45"; then
    ok "ramp level: authentic animation captured"
else
    bad "ramp level: no capture"
fi
out="$(OPENPETE_VSTREAM_FATAL=1 run_game "$WORK/sc" 3500 --skip-to-level stone_hill)"
if printf '%s\n' "$out" | grep -qE "[0-9]+ frame words carry stream handles" && \
   printf '%s\n' "$out" | grep -q "openpete-exit: reason=frame-limit"; then
    ok "injected animation: frame words carry handles; run clean with unresolved fatal"
else
    bad "injected animation did not carry handles, or the run aborted"
fi

# ── 14. per-modset save safety: tag + warn + adopt (memcard sidecar) ────
# The modset tag ("id@ver+...", non-mutative mods excluded) is checked at
# engine startup against the card's sidecar. Five flows, ~300 frames each:
# fresh-vanilla silence, vanilla-card+mods warn, mismatch warn, explicit
# adopt (sidecar rewritten), tagged-card+vanilla warn. The last uses an
# observer to prove a mod that writes no guest RAM doesn't taint the tag.
say "save safety: modset tag / warn / adopt on the memcard sidecar"
CARDS="$WORK/cards"; mkdir -p "$CARDS"
fixture "$WORK/ss-gems" double-gems
fixture "$WORK/ss-observer" hello-anchor
mc_run() { # mc_run [extra flags...] -> stdout+stderr
    local log; log="$(mktemp "$WORK/mc.XXXXXX.log")"
    local -a PG_CAPTURE_ENV=(OPENPETE_CARD_DIR="$CARDS")
    local PG_CAPTURE_TIMEOUT=120
    pg_capture "$log" "" -- --frame-limit 300 "$@"
    cat "$log"
}
out="$(mc_run)"
if [ ! -e "$CARDS/card1.mcr.modset" ] && \
   ! printf '%s\n' "$out" | grep -q "memcard.*modset"; then
    ok "vanilla + fresh dir: silent, no sidecar"
else
    bad "vanilla fresh-dir run warned or created a sidecar"
fi
dd if=/dev/zero of="$CARDS/card1.mcr" bs=1024 count=128 2>/dev/null
out="$(mc_run --mods-dir "$WORK/ss-gems")"
if printf '%s\n' "$out" | grep -q "holds VANILLA progress but mods are active"; then
    ok "vanilla card + divergent mod: warned"
else
    bad "vanilla-card warning missing"
fi
echo "other-mod@9.9" > "$CARDS/card1.mcr.modset"
out="$(mc_run --mods-dir "$WORK/ss-gems")"
if printf '%s\n' "$out" | grep -q "modset MISMATCH"; then
    ok "mismatched tag: warned (tag preserved)"
else
    bad "mismatch warning missing"
fi
out="$(mc_run --mods-dir "$WORK/ss-gems" --adopt-saves)"
if printf '%s\n' "$out" | grep -q "modset tag adopted" && \
   grep -q "^double-gems@" "$CARDS/card1.mcr.modset"; then
    ok "--adopt-saves: adopted + sidecar rewritten"
else
    bad "adopt flow failed (log or sidecar content)"
fi
out="$(mc_run --mods-dir "$WORK/ss-observer")"
if printf '%s\n' "$out" | grep -q "but no mods are active"; then
    ok "tagged card + observer-only modset: warned (non-mutative excluded from tag)"
else
    bad "tagged-card/vanilla warning missing (non-mutative exclusion broken?)"
fi

# Whether a mod joins the tag is DERIVED when the manifest is silent: a
# mod that loads no code object still changes gameplay if it claims a
# level slot. Contrast two mods of the identical shape (declarative, no
# .so, no sim_mutative key) whose only difference is [[level]] - the
# level mod must be tagged and the asset mod must not.
mkdir -p "$WORK/ss-level/level-only"
printf '[mod]\nid = "level-only"\nversion = "0.0.1"\ngame = "spyro-1"\napi = %s\n\n[[level]]\nname = "Level Only"\nfiles = "level_only"\ndonor = "dark_hollow"\n' \
    "$API_VER" > "$WORK/ss-level/level-only/mod.toml"
echo level-only > "$WORK/ss-level/enabled.txt"
fixture "$WORK/ss-assets" music-swap
rm -f "$CARDS/card1.mcr.modset"
dd if=/dev/zero of="$CARDS/card1.mcr" bs=1024 count=128 2>/dev/null
mc_run --mods-dir "$WORK/ss-level" --adopt-saves >/dev/null 2>&1
if grep -q "^level-only@" "$CARDS/card1.mcr.modset" 2>/dev/null; then
    ok "custom-level mod joins the tag (derived mutative without code)"
else
    bad "level mod absent from the tag - the [[level]] derivation broke"
fi
rm -f "$CARDS/card1.mcr.modset"
mc_run --mods-dir "$WORK/ss-assets" --adopt-saves >/dev/null 2>&1
if [ ! -e "$CARDS/card1.mcr.modset" ]; then
    ok "asset-only mod stays out of the tag (derived non-mutative)"
else
    bad "asset-only mod tagged the card: $(cat "$CARDS/card1.mcr.modset")"
fi

# ── 15. zip mods: folder or zip, identical (build + load) ───────────────
# hello-anchor zipped, dropped as <id>.zip, loads end-to-end (unpack ->
# content-hash-cached compile -> dlopen); a second boot reuses both
# caches (no re-extract, no recompile).
say "zip mods: <id>.zip unpack + content-hash caches"
mkdir -p "$WORK/zipmods"
python3 - "$WORK/zipmods/hello-anchor.zip" <<'PYEOF'
import sys, zipfile, os
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    z.write('mods/hello-anchor/mod.toml', 'mod.toml')
    for root, dirs, files in os.walk('mods/hello-anchor/src'):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, 'mods/hello-anchor'))
PYEOF
echo hello-anchor > "$WORK/zipmods/enabled.txt"
out="$(run_game "$WORK/zipmods" 100)"
if printf '%s\n' "$out" | grep -q "unpacked 2 files from hello-anchor.zip" &&
   printf '%s\n' "$out" | grep -q "loaded hello-anchor"; then
    ok "zip mod unpacked + loaded"
else
    bad "zip mod failed to unpack/load"
fi
out="$(run_game "$WORK/zipmods" 100)"
if ! printf '%s\n' "$out" | grep -qE "unpacked [0-9]+ file|: compiling " &&
   printf '%s\n' "$out" | grep -q "loaded hello-anchor"; then
    ok "second boot: both caches hit (no re-extract, no recompile)"
else
    bad "zip/build caches missed on the second boot"
fi

# ── 16. live modset toggle: tick-boundary disable/enable ────────────────
# hello-anchor's CameraUpdate hook is scheduled off at canonical tick
# 2200 and back on at 3300 (OPENPETE_MODS_TOGGLE); the per-mod timer
# windows prove the hook actually stopped (a 0-call window) and came
# back (a >0-call window strictly after the ENABLED line).
say "live toggle: disable/enable at tick boundaries (timer-window proof)"
fixture "$WORK/toggle" hello-anchor
out="$(OPENPETE_MOD_TIMERS=1 \
       OPENPETE_MODS_TOGGLE='2200:hello-anchor:0,3300:hello-anchor:1' \
       run_game "$WORK/toggle" 8600)"
if printf '%s\n' "$out" | grep -q "live toggle: hello-anchor DISABLED (tick 2200)" &&
   printf '%s\n' "$out" | grep -q "live toggle: hello-anchor ENABLED (tick 3300)"; then
    ok "toggles applied at the scheduled tick boundaries"
else
    bad "toggle schedule did not apply"
fi
if printf '%s\n' "$out" | \
   awk '/live toggle: hello-anchor DISABLED/{d=1}
        d && /timer hello-anchor: .*\(0 calls\)/{found=1}
        END{exit !found}'; then
    ok "hook silent while disabled (0-call timer window)"
else
    bad "no 0-call window after disable"
fi
if printf '%s\n' "$out" | \
   awk '/live toggle: hello-anchor ENABLED/{e=1}
        e && /timer hello-anchor: .*\([1-9][0-9]* calls\)/{found=1}
        END{exit !found}'; then
    ok "hook live again after re-enable (>0-call timer window)"
else
    bad "no active window after re-enable"
fi

# ── 17. process handoff: checkpoint -> exec -> restore ────────────────────
# OPENPETE_HANDOFF_AT=2000 checkpoints the session and execs the same
# image mid-run; the fresh process resumes the state and runs to the
# frame limit. Asserts exactly ONE exec (the resume consumes the
# trigger), the resume landing on the checkpoint frame, mod hooks live
# after the exec, and a normal frame-limit exit.
say "process handoff: exec mid-run, session continuity"
fixture "$WORK/handoff" hello-anchor
out="$(OPENPETE_HANDOFF_AT=2000 run_game "$WORK/handoff" 6000)"
execs=$(printf '%s\n' "$out" | grep -c "handoff: exec at frame")
if [ "$execs" = "1" ] &&
   printf '%s\n' "$out" | grep -q "handoff: session resumed at frame 2000"; then
    ok "one exec + resume at the checkpoint frame"
else
    bad "handoff exec/resume broken (execs=$execs)"
fi
if printf '%s\n' "$out" | \
   awk '/handoff: session resumed/{r=1}
        r && /hello from the override chain/{found=1}
        END{exit !found}'; then
    ok "mod hooks live in the resumed process"
else
    bad "mod hook never fired after resume"
fi
if printf '%s\n' "$out" | grep -q "reason=frame-limit frames=6000"; then
    ok "resumed session ran to the frame limit"
else
    bad "resumed session did not complete"
fi

# ── 18. dev hot reload: edit a mod's source mid-run, new code goes live ──
# A purpose-built mod logs "v1 alive"; its source is edited to v2 while
# the game runs with OPENPETE_MOD_HOT_RELOAD=1. The watcher (content
# build key, every 64 canonical ticks) rebuilds, dlopens a fresh object,
# re-runs entry - "v2 alive" must appear in the SAME session.
say "hot reload: mid-run source edit goes live in-session"
mkdir -p "$WORK/hotsrc/hot-greeter/src"
printf '[mod]\nid = "hot-greeter"\nversion = "0.0.1"\napi = '"$API_VER"'\n' \
    > "$WORK/hotsrc/hot-greeter/mod.toml"
cat > "$WORK/hotsrc/hot-greeter/src/greet.c" <<'CEOF'
#include <openpete_mod_api.h>
static const openpete_mod_api_t* g_api; static openpete_mod_t* g_self;
static int g_greeted;
static void on_update(CPUState* cpu) {
    g_api->base(cpu);
    if (!g_greeted) { g_greeted = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO, "hot-greeter: v1 alive"); }
}
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    return api->override_name(self, "GamestateUpdate", on_update);
}
CEOF
mkdir -p "$WORK/hot"
ln -sfn "$WORK/hotsrc/hot-greeter" "$WORK/hot/hot-greeter"
echo hot-greeter > "$WORK/hot/enabled.txt"
OPENPETE_MOD_HOT_RELOAD=1 run_game "$WORK/hot" 9000 > "$WORK/hot.log" 2>&1 &
hotpid=$!
sleep 6
sed -i 's/v1 alive/v2 alive/' "$WORK/hotsrc/hot-greeter/src/greet.c"
wait "$hotpid"
if grep -q "hot-greeter: v1 alive" "$WORK/hot.log" &&
   grep -q "hot reload: hot-greeter live" "$WORK/hot.log" &&
   grep -q "hot-greeter: v2 alive" "$WORK/hot.log"; then
    ok "v1 ran, reload fired, v2 ran - same session"
else
    bad "hot reload sequence incomplete (see $WORK/hot.log)"
    grep -E "hot" "$WORK/hot.log" | head -5
fi

# ── mod shaders: engine-compiled GLSL postfx visibly present in the
# headless dump. The headless
# screenshot path composes native_rtt, which BYPASSES off_image - a
# chain wired only into the live path passes a boots-and-screenshots
# gate vacuously, so this rung asserts CONTENT: the crt scanlines make
# the adjacent-row luma delta exceed the skip-row delta (alternating
# rows); natural content is smoother row-to-row (measured: ratio ~2.5
# with the filter, ~0.6 without).
say "mod shaders: crt-filter postfx visible AND upright in the headless dump"
fixture "$WORK/crt" crt-filter
run_game "$WORK/crt" 400 --screenshot-on-frame 350 \
    --screenshot-output "$WORK/crt/shot.png" > "$WORK/crt.log" 2>&1 || true
mkdir -p "$WORK/crtoff"; : > "$WORK/crtoff/enabled.txt"
run_game "$WORK/crtoff" 400 --screenshot-on-frame 350 \
    --screenshot-output "$WORK/crtoff/shot.png" > "$WORK/crtoff.log" 2>&1 || true
# "crt filter armed" = shader_register succeeded (fresh compile OR
# cache hit - a hit logs no compile line) AND postfx_register/enable
# went through. Grepping for the compile line alone false-fails on
# every cache-hit run.
if grep -q "crt filter armed" "$WORK/crt.log"; then
    ok "crt.frag registered + postfx armed (compile or cache hit)"
else
    bad "crt filter did not arm (see $WORK/crt.log)"
fi
if [ -f "$WORK/crt/shot-vk.png" ] && [ -f "$WORK/crtoff/shot-vk.png" ] &&
   python3 -c "import PIL" 2>/dev/null; then
    # Two assertions from one image pair:
    #  1. presence - crt scanlines alternate rows, so the adjacent-row
    #     luma delta exceeds the skip-row delta (~2.5x with the filter,
    #     ~0.6x without);
    #  2. orientation - the filtered dump's row-luma profile must
    #     correlate better with the unfiltered dump's profile than with
    #     its vertical REVERSAL (the first crt shot shipped upside down:
    #     the fullscreen triangle ignored the engine's Y-flipped
    #     viewport; a row-delta metric is orientation-blind by itself).
    verdict=$(python3 - "$WORK/crt/shot-vk.png" "$WORK/crtoff/shot-vk.png" <<'PYEOF'
import sys, statistics
from PIL import Image
def rows_of(p):
    im = Image.open(p).convert("L")
    w, h = im.size
    px = im.load()
    return [sum(px[x, y] for x in range(0, w, 4)) for y in range(h)]
on, off = rows_of(sys.argv[1]), rows_of(sys.argv[2])
h = min(len(on), len(off))
on, off = on[:h], off[:h]
d1 = statistics.mean(abs(on[y] - on[y + 1]) for y in range(h - 2))
d2 = statistics.mean(abs(on[y] - on[y + 2]) for y in range(h - 2))
ratio = d1 / (d2 + 1e-9)
def corr(a, b):
    ma, mb = statistics.mean(a), statistics.mean(b)
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = sum((x - ma) ** 2 for x in a) ** 0.5
    db = sum((y - mb) ** 2 for y in b) ** 0.5
    return num / (da * db + 1e-9)
straight, flipped = corr(on, off), corr(on, off[::-1])
print(f"{ratio:.2f} {straight:.3f} {flipped:.3f} "
      f"{'OK' if ratio > 1.5 and straight > flipped else 'BAD'}")
PYEOF
)
    if [ "${verdict##* }" = "OK" ]; then
        ok "postfx present + upright (ratio/corr: $verdict)"
    else
        bad "postfx content check failed (ratio corr corr-flipped verdict: $verdict)"
    fi
else
    if [ ! -f "$WORK/crt/shot-vk.png" ]; then
        bad "no shot-vk.png from the crt rung (see $WORK/crt.log)"
    else
        ok "PIL unavailable - content assertion skipped (compile assert only)"
    fi
fi

# ── mod shaders: the SCENE injection point processes the 3D scene
# UNDER the HUD. scene-sepia sepia-tones the scene at the SCENE point,
# then draws a cyan HUD text marker AFTER. The decisive proof is that
# the marker stays CYAN, a colour a sepia tone cannot produce: if the
# SCENE split were broken (effect ran over everything, or the
# load-rebegin lost the scene), the marker would be sepia'd or the scene
# black. Assert cyan glyph pixels exist in the marker's region
# (under-HUD) AND a scene sample is sepia (SCENE ran). Windowed + a
# real level (--skip-to-level 10) so the frame has 3D content.
# NB --skip-to-level fast-forwards presents until the level is playable
# (~f1700 for the induced Artisans demo), so the recording only spans
# the in-level tail - size the frame limit past the load and sample
# near the END of the video, never at a fixed offset from its start.
say "mod shaders: SCENE point processes the scene under the HUD"
if command -v ffmpeg >/dev/null 2>&1 && python3 -c "import PIL" 2>/dev/null; then
    fixture "$WORK/sepia" scene-sepia
    op_own_run timeout -k 10 300 "$BIN" --disc "$DISC" --disc-cache 0 \
        --skip-to-level 10 --frame-limit 2600 --mods-dir "$WORK/sepia" \
        --record "$WORK/sepia/probe.mkv" > "$WORK/sepia.log" 2>&1 || true
    if [ -s "$WORK/sepia/probe.mkv" ] &&
       ffmpeg -y -loglevel error -sseof -2 -i "$WORK/sepia/probe.mkv" \
              -frames:v 1 "$WORK/sepia/frame.png" 2>/dev/null &&
       [ -s "$WORK/sepia/frame.png" ]; then
        sepia_v=$(python3 - "$WORK/sepia/frame.png" <<'PYEOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size; px = im.load()
# The mod's "SCENE" marker: cyan glyphs at overlay (0.04, 0.04), cap 0.05.
# Glyph strokes are thin, so scan the region and count cyan pixels
# (R low, G and B high); sepia output always has R >= G >= B.
cyan = 0
for yy in range(int(h*0.02), int(h*0.12)):
    for xx in range(int(h*0.02), int(h*0.45)):
        r, g, b = px[xx, yy]
        if r < 80 and g > 120 and b > 120:
            cyan += 1
sc  = px[int(w*0.5),  int(h*0.6)]            # 3D scene
scene_sepia = sc[0] >= sc[1] >= sc[2] and (sc[0] - sc[2]) > 15 and sc[0] > 40
print("OK" if cyan >= 20 and scene_sepia else f"BAD cyan_px={cyan} scene={sc}")
PYEOF
)
        if [ "$sepia_v" = "OK" ]; then
            ok "HUD marker crisp cyan over a sepia scene - SCENE runs under the HUD"
        else
            bad "SCENE under-HUD not confirmed ($sepia_v - see $WORK/sepia.log)"
        fi
    elif grep -qiE "no vulkan|no display|surface|failed to (create|open)" "$WORK/sepia.log"; then
        ok "no display for the windowed SCENE record - assertion skipped"
    else
        bad "recording failed for the SCENE rung (see $WORK/sepia.log)"
    fi
else
    ok "ffmpeg/PIL unavailable - SCENE recording assertion skipped"
fi

# ── declarative materials: a shader mod with NO C at all ───────────────
# The zero-C tier's whole claim is that a manifest plus a .frag is a
# complete mod, so the rung asserts exactly that: the mod loads down the
# declarative path (never compiled), its [[material]] rows arm, and the
# geometry they select actually changes on screen. The pixel leg is a
# controlled A/B (same frame and seed, mod present vs absent) rather than
# a colour classifier - "some pixels moved" is the structural claim; which
# colour they moved to is the mod's business, not the gate's.
say "declarative materials: manifest-only mod arms rows without compiling"
fixture "$WORK/decl" declarative-reshade
out="$(run_game "$WORK/decl" 400 --renderer vk 2>&1)"
if printf '%s\n' "$out" | grep -q "declarative-reshade: 3 declarative material row(s)"; then
    ok "manifest [[material]] rows parsed"
else
    bad "declarative rows not parsed"
    printf '%s\n' "$out" | grep -i "declarative-reshade" | head -3
fi
if printf '%s\n' "$out" | grep -q "loaded declarative-reshade .*declarative"; then
    ok "loaded down the declarative path - nothing was compiled"
else
    bad "declarative mod did not take the declarative path"
fi
if [ "$(printf '%s\n' "$out" | grep -c "declarative-reshade: material selector armed")" = "3" ]; then
    ok "all three selectors armed (incl. the enabled=false row)"
else
    bad "wrong number of armed selectors"
fi

# A bad channel name must be REFUSED, never defaulted to "any": silently
# widening a typo'd selector would repaint geometry the author never asked
# for, which is the one failure mode the selector contract exists to stop.
say "declarative materials: an unknown channel is refused, not widened"
mkdir -p "$WORK/declbad/typo/shaders"
cp mods/declarative-reshade/shaders/tint.frag "$WORK/declbad/typo/shaders/"
cat > "$WORK/declbad/typo/mod.toml" <<'TOML'
[mod]
id = "typo"
version = "0.0.1"
api = 3
[[material]]
frag = "shaders/tint.frag"
channel = "terrian"
TOML
mkdir -p "$WORK/decl2"
ln -sfn "$WORK/declbad/typo" "$WORK/decl2/typo"
echo "typo" > "$WORK/decl2/enabled.txt"
out="$(run_game "$WORK/decl2" 300 2>&1)"
if printf '%s\n' "$out" | grep -q "unknown channel \"terrian\""; then
    ok "typo'd channel rejected by name"
else
    bad "typo'd channel was not rejected"
fi
if printf '%s\n' "$out" | grep -q "typo: material selector armed"; then
    bad "the refused row armed anyway - it would match every channel"
else
    ok "no selector armed from the refused row"
fi

say "declarative materials: the rows actually repaint their geometry"
if python3 -c "import PIL" 2>/dev/null; then
    mkdir -p "$WORK/declnone"; : > "$WORK/declnone/enabled.txt"
    for leg in none decl; do
        d="$WORK/decl"; [ "$leg" = none ] && d="$WORK/declnone"
        RUN_OUTS="$WORK/decl-$leg-1800-vk.png" \
        run_game "$d" 1802 --renderer vk --mute 1 \
            --bk2 games/spyro-1/bk2/flame.bk2 --screenshot-native-only \
            --screenshot-on-frame 1800 \
            --screenshot-output "$WORK/decl-$leg-{n}.png" \
            --max-runtime-seconds 280 \
            > "$WORK/decl-$leg.log" 2>&1 || true
    done
    if [ -s "$WORK/decl-none-1800-vk.png" ] && [ -s "$WORK/decl-decl-1800-vk.png" ]; then
        n_changed=$(python3 - "$WORK/decl-none-1800-vk.png" "$WORK/decl-decl-1800-vk.png" <<'PYEOF'
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
print(sum(1 for p in ImageChops.difference(a, b).getdata() if p != (0, 0, 0)))
PYEOF
)
        # The player + flame rows cover a few percent of this frame; a
        # threshold well under that catches "armed but never drew" without
        # pinning the gate to an exact pixel count.
        if [ "${n_changed:-0}" -gt 2000 ]; then
            ok "manifest rows repainted $n_changed px vs no mods"
        else
            bad "declarative rows armed but changed only ${n_changed:-0} px"
        fi
    else
        ok "no headless vk capture available - pixel assertion skipped"
    fi
else
    ok "PIL unavailable - declarative pixel assertion skipped"
fi

# ── table census: every converted table grows past its former cap ──────
#
# One fixture, one run, one row per table. The assertion is peak > was,
# not count > was: a table drained at the tick boundary (the toggle
# queues, the draw-text staging) reads count=0 at exit, so the
# high-water is what proves the run crossed the old fixed capacity.
#
# The fixture is generated, never committed: 65 small mods for the
# process-global tables, one mod carrying every per-mod row kind, one
# with more sources than the old compile line held, and one registering
# more shaders and post-process passes than the old tables did.
say "table census: every converted table grows past its former cap"

CEN="$WORK/census"
mkdir -p "$CEN"
: > "$CEN/enabled.txt"

cen_mod() { # cen_mod <id> -- manifest header + enabled.txt row
    local id="$1"
    mkdir -p "$CEN/$id"
    printf '[mod]\nid = "%s"\nname = "%s"\nversion = "0.1.0"\n' "$id" "$id" \
        > "$CEN/$id/mod.toml"
    printf 'game = "spyro-1"\napi = %s\nstate = "none"\nrunahead = "pure"\n' \
        "$API_VER" >> "$CEN/$id/mod.toml"
    echo "$id" >> "$CEN/enabled.txt"
}

# 65 mods: a present hook each, a link each on one shared chain, a chain
# each of their own, a config row and a declarative level. cen00 also
# fills the guest arena ledger past its former 8192 rows.
for i in $(seq 0 64); do
    id="$(printf 'cen%02d' "$i")"
    cen_mod "$id"
    printf '\n[[config]]\nkey = "on"\ntype = "bool"\ndefault = true\n' \
        >> "$CEN/$id/mod.toml"
    printf '\n[[level]]\nname = "Cen %d"\nfiles = "cen%02d"\ndonor = "dark_hollow"\n' \
        "$i" "$i" >> "$CEN/$id/mod.toml"
    mkdir -p "$CEN/$id/src"
    {
        echo '#include <openpete_mod_api.h>'
        echo 'static const openpete_mod_api_t* g_api;'
        echo 'static void present(const openpete_present_ctx_t* c) { (void)c; }'
        echo 'static void shared(CPUState* cpu) { g_api->base(cpu); }'
        echo 'static void mine(CPUState* cpu) { g_api->base(cpu); }'
        echo 'int openpete_mod_entry(const openpete_mod_api_t* api,'
        echo '                       openpete_mod_t* self) {'
        echo '    g_api = api;'
        echo '    api->register_present_hook(self, present);'
        echo '    api->override_name(self, "CameraUpdate", shared);'
        printf '    api->override_addr(self, 0x%08Xu, mine);\n' \
            "$((0x80010000 + i * 4))"
        # One allocation each: the arena's owner ledger carries a row per
        # allocating mod, so passing its former 64 needs 65 owners, not
        # 65 allocations from one owner.
        echo '    (void)api->guest_alloc(self, 16, 0, 0, 0);'
        if [ "$i" -eq 0 ]; then
            echo '    for (int k = 0; k < 8300; k++)'
            echo '        if (!api->guest_alloc(self, 16, 0, 0, 0)) break;'
        fi
        echo '    return 0;'
        echo '}'
    } > "$CEN/$id/src/probe.c"
done

# Every per-mod row kind at once: 9 deps, 9 bindings, 33 config rows of
# 9 items, 17 material rows, 5 level blocks.
cen_mod cenrows
{
    printf 'deps = ['
    for i in $(seq 0 8); do
        printf '"cen%02d"' "$i"; [ "$i" -lt 8 ] && printf ', '
    done
    printf ']\n'
    for i in $(seq 1 9); do
        printf '\n[[binding]]\nname = "act%d"\nkey = "F%d"\npurpose = "row %d"\n' \
            "$i" "$i" "$i"
    done
    for r in $(seq 1 33); do
        printf '\n[[config]]\nkey = "opt%02d"\ntype = "string"\ndefault = "c1"\nitems = [' "$r"
        for k in $(seq 1 9); do
            printf '"c%d"' "$k"; [ "$k" -lt 9 ] && printf ', '
        done
        printf ']\n'
    done
    for m in $(seq 1 17); do
        printf '\n[[material]]\nfrag = "shaders/tint.frag"\nchannel = "player"\nparams = [1.0, 0.5, 0.2, 0.01]\n'
    done
    for l in $(seq 1 5); do
        printf '\n[[level]]\nname = "Rows %d"\nfiles = "rows%d"\ndonor = "dark_hollow"\n' "$l" "$l"
    done
} >> "$CEN/cenrows/mod.toml"
mkdir -p "$CEN/cenrows/shaders"
cp "$MODS_SRC/declarative-reshade/shaders/tint.frag" "$CEN/cenrows/shaders/"

# 16 more material rows, so the engine-wide material table passes 32.
cen_mod cenmats
for m in $(seq 1 16); do
    printf '\n[[material]]\nfrag = "shaders/tint.frag"\nchannel = "moby"\nparams = [0.2, 0.9, 0.4, 0.01]\n' \
        >> "$CEN/cenmats/mod.toml"
done
mkdir -p "$CEN/cenmats/shaders"
cp "$MODS_SRC/declarative-reshade/shaders/tint.frag" "$CEN/cenmats/shaders/"

# More translation units than the old compile line could carry.
cen_mod censrcs
mkdir -p "$CEN/censrcs/src"
for i in $(seq 1 70); do
    printf 'int unit_%02d(void) { return %d; }\n' "$i" "$i" \
        > "$CEN/censrcs/src/u$(printf '%02d' "$i").c"
done
{
    echo '#include <openpete_mod_api.h>'
    echo 'int unit_70(void);'
    echo 'int openpete_mod_entry(const openpete_mod_api_t* api,'
    echo '                       openpete_mod_t* self) {'
    echo '    /* Links only if the 70th unit reached the compiler. */'
    echo '    api->log(self, 2, "censrcs: unit_70 = %d", unit_70());'
    echo '    return 0;'
    echo '}'
} > "$CEN/censrcs/src/entry.c"

# More shaders and post-process passes than the old tables held.
cen_mod cenfx
mkdir -p "$CEN/cenfx/shaders" "$CEN/cenfx/src"
{
    echo '#version 450'
    echo '#extension GL_GOOGLE_include_directive : require'
    echo '#define OP_POSTFX_STAGE_FRAGMENT 1'
    echo '#include "openpete_shader_api.glsl"'
    echo 'void main() { op_color = texture(op_prev_pass, op_uv); }'
} > "$CEN/cenfx/shaders/p.frag"
{
    echo '#include <openpete_mod_api.h>'
    echo 'int openpete_mod_entry(const openpete_mod_api_t* api,'
    echo '                       openpete_mod_t* self) {'
    echo '    int frag = -1;'
    echo '    for (int i = 0; i < 300; i++) {'
    echo '        int h = api->shader_register(self, "shaders/p.frag");'
    echo '        if (h >= 0 && frag < 0) frag = h;'
    echo '    }'
    echo '    if (frag < 0) return 0;'
    echo '    for (int i = 0; i < 70; i++) {'
    echo '        int fx = api->postfx_register(self, frag,'
    echo '                                      OP_POSTFX_COMPOSITE, i);'
    echo '        if (fx >= 0) api->postfx_enable(self, fx, 1);'
    echo '    }'
    echo '    return 0;'
    echo '}'
} > "$CEN/cenfx/src/fx.c"

# 33 owner toggles at one tick: the chain owner-state table records an
# entry per owner ever toggled, and the queues carry them in one batch.
cen_toggles=""
for i in $(seq 0 32); do
    cen_toggles="${cen_toggles}200:cen$(printf '%02d' "$i"):0,"
done
cen_toggles="${cen_toggles%,}"

# The boot compiles 69 mods before the run starts. That measured 9 s here
# (the 300 shader registrations name one file, so it compiles once and the
# content-key cache serves the rest), well inside the helper's default
# ceiling; the ceiling is still named rather than inherited, because this
# leg loads a level and a cold machine has neither the disc cache nor the
# compile cache. CEN_SECS reports the run's own wall clock.
# The census is written to a file, not read out of the run's output:
# stderr is dup2'd onto a pipe that the log pump drains, so a burst of
# lines at exit races the pump and loses its tail. A file is not on that
# pipe, so the report arrives whole.
CEN_TXT="$WORK/census.txt"
rm -f "$CEN_TXT"
cen_t0=$SECONDS
OPENPETE_MOD_TABLE_CENSUS="$CEN_TXT" OPENPETE_MODS_TOGGLE="$cen_toggles" \
    OPENPETE_TERRAIN_CACHE_FORCE=1 RUN_TIMEOUT=300 \
    run_game "$CEN" 900 --skip-to-level artisans \
    --screenshot-on-frame 880 \
    --screenshot-output "$WORK/census-{n}.png" > /dev/null 2>&1
CEN_SECS=$((SECONDS - cen_t0))
cen_out="$(cat "$CEN_TXT" 2>/dev/null || true)"

# A short report reads as a table that was never measured rather than as
# a failure, so the row count is asserted against the header the report
# prints for exactly that purpose.
cen_tables="$(printf '%s\n' "$cen_out" | sed -n 's/.*mod-table: tables=\([0-9]*\).*/\1/p' | head -1)"
cen_rows="$(printf '%s\n' "$cen_out" | grep -c 'mod-table: [^ =]* count=' || true)"
if [ -n "$cen_tables" ] && [ "$cen_rows" -eq "$cen_tables" ]; then
    ok "census complete: $cen_rows rows, header says $cen_tables (${CEN_SECS}s)"
else
    bad "census truncated: $cen_rows rows printed, header says ${cen_tables:-none}"
fi

# name:former-capacity. Each row must show a high-water above the cap the
# table used to carry.
for row in \
    mods:64 cfg_slots:64 mod_time:24 present_hooks:16 chains:64 \
    chain_owner_state:32 toggle_queue:16 toggle_sched:16 \
    custom_levels:64 arena_rows:8192 arena_owners:64 materials:32 \
    shaders:256 postfx:64 mod_deps:8 mod_bindings:8 mod_cfg_rows:32 \
    mod_cfg_items:8 mod_material_rows:16 mod_levels:4 mod_srcs:64
do
    name="${row%%:*}"; was="${row##*:}"
    line="$(printf '%s\n' "$cen_out" | grep -m1 "mod-table: $name count=")"
    if [ -z "$line" ]; then
        bad "census row $name missing (former cap $was)"
        continue
    fi
    peak="$(printf '%s\n' "$line" | sed -n 's/.*peak=\([0-9]*\).*/\1/p')"
    if [ "${peak:-0}" -gt "$was" ]; then
        ok "$name peak=$peak > was=$was"
    else
        bad "$name peak=${peak:-0} did not pass its former cap $was"
    fi
done

# The sound cache is the one converted table this run cannot reach: it
# fills only when a WAV decodes, and a headless run has no audio device
# to open. Covered by the mod_vec unit test on the same container.
ok "snd_cache CARVE-OUT: no audio device headless, covered by mod_vec_test"

finish
