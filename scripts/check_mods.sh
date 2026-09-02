#!/usr/bin/env bash
# check_mods.sh — the PoC-ladder CI gate (modtest orchestration).
# (notes/plans/openpete-modding-framework.md, "modtest harness" P1 item.)
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
# Usage: scripts/check_mods.sh   (repo root; ~10 min — ctest mod_poc_ladder)
# Env:   OPENPETE_BIN / OPENPETE_DISC — forwarded to runs + oracle.

set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/.."

MODS_SRC="$PWD/mods"
WORK="$(mktemp -d /tmp/openpete-modtest.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
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

run_game() { # run_game <mods_dir> <frames> [extra flags...] -> stdout+stderr
    # NB: --frame-limit is the engine flag; OPENPETE_FRAME_LIMIT is a
    # check_psx_oracle.sh convention, NOT read by the engine itself.
    local dir="$1" frames="$2"
    shift 2
    timeout 300 "$BIN" --headless --disc "$DISC" \
        --run-recomp 0x80012204 --frame-limit "$frames" --mods-dir "$dir" \
        "$@" 2>&1
}

# ── 1+2. chain-duo matrix (order A,B then B,A) — also observer-neutral ──
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
say "--mods override: file says [a,b], flag says [b,a] — flag wins"
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
# line the same way it reports one that outran the buffer — so the last
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
out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES scripts/check_psx_oracle.sh -- \
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

# ── 4. lib-and-client: deps topo-sort (rung 5) ──────────────────────────
say "lib-and-client: client listed FIRST — topo-sort must load lib first"
fixture "$WORK/libclient" lib-and-client demo-lib
out="$(run_game "$WORK/libclient" 600)"
lib_line=$(printf '%s\n' "$out" | grep -n "demo-lib: entry ran" | head -1 | cut -d: -f1)
cli_line=$(printf '%s\n' "$out" | grep -n "lib-and-client: entry ran" | head -1 | cut -d: -f1)
if [ -n "$lib_line" ] && [ -n "$cli_line" ] && [ "$lib_line" -lt "$cli_line" ]; then
    ok "dependency entry ran first despite reversed enabled order"
else
    bad "dependency order wrong or entries missing (lib=$lib_line cli=$cli_line)"
fi


say "lib-and-client: missing dep — actionable error, run survives"
fixture "$WORK/nodep" lib-and-client
out="$(run_game "$WORK/nodep" 600)"
if printf '%s\n' "$out" | grep -q 'requires "demo-lib", which is not enabled'; then
    ok "missing-dep error is actionable"
else
    bad "missing-dep error absent/wrong"
fi
if printf '%s\n' "$out" | grep -q "openpete-exit: reason=frame-limit"; then
    ok "run survived the disabled mod"
else
    bad "run did not reach the frame limit"
fi

say "lib-and-client: dependency cycle — both disabled, run survives"
for m in cycle-x cycle-y; do
    other=cycle-y; [ "$m" = cycle-y ] && other=cycle-x
    mkdir -p "$WORK/cycsrc/$m/src"
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = '"$API_VER"'\ndeps = ["%s"]\n' \
        "$m" "$other" > "$WORK/cycsrc/$m/mod.toml"
    printf '#include <openpete_mod_api.h>\nint openpete_mod_entry(const openpete_mod_api_t* a, openpete_mod_t* s){(void)a;(void)s;return 0;}\n' \
        > "$WORK/cycsrc/$m/src/m.c"
done
mkdir -p "$WORK/cycle"
ln -sfn "$WORK/cycsrc/cycle-x" "$WORK/cycle/cycle-x"
ln -sfn "$WORK/cycsrc/cycle-y" "$WORK/cycle/cycle-y"
printf 'cycle-x\ncycle-y\n' > "$WORK/cycle/enabled.txt"
out="$(run_game "$WORK/cycle" 600)"
if printf '%s\n' "$out" | grep -q "dependency cycle through"; then
    ok "cycle detected and reported"
else
    bad "cycle error absent"
fi
if printf '%s\n' "$out" | grep -q "mods:.*mod.* active"; then
    bad "cycle mods loaded anyway"
else
    ok "neither cycle mod loaded"
fi

# ── 5. crash-dummy: attribution names the offending mod (rung 6) ────────
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
    /* func_8003B9D4 is CollectItem's historical raw name — the alias
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

# ── 8. asset overlay: texture namespace scan + overlay-duel (rungs 7+10) ─
say "asset overlay: mod texture joins the pack (rung 7 scan)"
fixture "$WORK/tex" hd-single-texture
out="$(timeout 300 "$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
        --frame-limit 300 --mods-dir "$WORK/tex" \
        --screenshot-on-frame 200 \
        --screenshot-output "$WORK/tex-{n}.png" 2>&1)"
if printf '%s\n' "$out" | grep -q "\[texpack\] + mod hd-single-texture: 1 entry"; then
    ok "mod texture scanned into the replacement pack"
else
    bad "mod texture scan line missing"
    printf '%s\n' "$out" | grep -i texpack | head -3
fi

say "asset overlay: two mods collide on one asset — last in order wins (rung 10)"
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
    timeout 300 "$BIN" --headless --disc "$DISC" --run-recomp 0x80012204 \
        --frame-limit 300 --mods-dir "$WORK/duel" \
        --screenshot-on-frame 200 \
        --screenshot-output "$WORK/duel-{n}.png" 2>&1
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

# ── 9. level-segment overlay: CdRead chokepoint (rung 9) ────────────────
say "wad overlay: byte-identical blob — chokepoint provably transparent"
mkdir -p "$WORK/wadsrc/wad-identity/assets/wad"
printf '[mod]\nid = "wad-identity"\nversion = "0.0.1"\napi = '"$API_VER"'\n' \
    > "$WORK/wadsrc/wad-identity/mod.toml"
if python3 tools/extract_wad_entry.py --disc "$DISC" --level 11 \
        --out "$WORK/wadsrc/wad-identity/assets/wad/level11.data.bin" \
        >/dev/null 2>&1; then
    mkdir -p "$WORK/wadid"
    ln -sfn "$WORK/wadsrc/wad-identity" "$WORK/wadid/wad-identity"
    printf 'wad-identity\n' > "$WORK/wadid/enabled.txt"
    out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES scripts/check_psx_oracle.sh -- \
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
    out="$(timeout 300 "$BIN" --headless --disc "$DISC" \
            --run-recomp 0x80012204 --skip-to-level 10 --frame-limit 2600 \
            --mods-dir "$WORK/wadrc" 2>&1)"
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
if python3 tools/modtest.py mods/hello-anchor mods/raw-probe \
        mods/double-gems mods/natural-gems mods/lib-and-client \
        mods/hud-stopwatch \
        mods/arena-probe mods/arena-canary mods/vstream-rehandle \
        mods/hd-single-texture mods/music-swap mods/spyro3 \
        mods/supercharge-anywhere; then
    ok "modtest runner: all [test] blocks green"
else
    bad "modtest runner reported failures"
fi

# ── 13. arena timeline canary: guest_alloc memory IS timeline state ───
# arena-canary mirrors g_GameTick in a MUTABLE arena block and logs
# DESYNC whenever guest RAM moves back in time without the arena. Every
# timeline feature must restore both together: the stress-rewind
# harness, runahead rollback (with the prediction oracle hashing the
# arena), a file save/load, and the reload cases — an Apply that keeps
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
if printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    bad "stress-rewind: DESYNC — the arena did not roll back with guest RAM"
else
    ok "stress-rewind: arena rolled back with guest RAM (no DESYNC)"
fi
out="$(OPENPETE_RA_PREDICT_CHECK=1 run_game "$WORK/canary" 7000 --runahead 1)"
if printf '%s\n' "$out" | grep -q "runahead: armed" && \
   printf '%s\n' "$out" | grep -q "arena-canary: armed" && \
   printf '%s\n' "$out" | grep -qE "\[ra-predict\] checked=[1-9][0-9]* mismatch=0" && \
   ! printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    ok "runahead armed over mutable arena memory; prediction oracle exact; no DESYNC"
else
    bad "runahead over the canary: not armed, oracle mismatch, or DESYNC"
fi
rm -f "$WORK/canary.state"
out="$(run_game "$WORK/canary" 5000 --state-save-on-frame 4500 \
        --state-save-output "$WORK/canary.state")"
if printf '%s\n' "$out" | grep -q "savestate: saved" && [ -e "$WORK/canary.state" ]; then
    ok "file save with mutable arena memory: written"
else
    bad "file save with mutable arena memory failed"
fi
out="$(run_game "$WORK/canary" 6000 --state-load-on-frame 5000 \
        --state-load-input "$WORK/canary.state")"
if printf '%s\n' "$out" | grep -q "arena ledger applied: .* 64 B restored" && \
   printf '%s\n' "$out" | grep -q "savestate: loaded" && \
   ! printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    ok "file load restored the arena with guest RAM (no DESYNC)"
else
    bad "file load: arena not restored, or DESYNC after the load"
fi
fixture "$WORK/canary-ms" double-gems
out="$(run_game "$WORK/canary-ms" 1500 --state-load-on-frame 1000 \
        --state-load-input "$WORK/canary.state")"
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
out="$(OPENPETE_MODS_CFG_APPLY=2700:arena-canary:slots=4 \
       run_game "$WORK/canary-same" 6500 \
        --state-save-on-frame 4500 --state-save-output "$WORK/same.state" \
        --state-load-on-frame 5500 --state-load-input "$WORK/same.state")"
if printf '%s\n' "$out" | grep -q "arena-canary call #0 replayed" && \
   printf '%s\n' "$out" | grep -q "savestate: loaded" && \
   ! printf '%s\n' "$out" | grep -q "arena-canary: DESYNC"; then
    ok "Apply (same shape): allocation replayed, pre-Apply file restored faithfully"
else
    bad "Apply (same shape): no replay, load refused, or DESYNC"
fi
out="$(OPENPETE_MODS_CFG_APPLY=2700:arena-canary:slots=8 \
       run_game "$WORK/canary-diff" 6500 \
        --state-save-on-frame 4500 --state-save-output "$WORK/diff.state" \
        --state-load-on-frame 5500 --state-load-input "$WORK/diff.state")"
if printf '%s\n' "$out" | grep -q "row #0 superseded" && \
   printf '%s\n' "$out" | grep -q 'entry #0 .* was superseded by a reload .* REFUSING the load'; then
    ok "Apply (changed shape): row superseded, pre-Apply file REFUSED naming the mod and entry"
else
    bad "Apply (changed shape): file written against a superseded row was not refused"
fi

# ── 13b. stream handles: every 21-bit field reader resolves handles ───
# vstream-rehandle rewrites EVERY animated model's frame words in the level
# to stream handles — mode 1 bound to the streams in place, mode 2 bound to
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
# level without one — every frame word must carry a handle.
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
mc_run() { OPENPETE_CARD_DIR="$CARDS" timeout 120 "$BIN" --headless \
    --disc "$DISC" --run-recomp 0x80012204 --frame-limit 300 "$@" 2>&1; }
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
# .so, no sim_mutative key) whose only difference is [[level]] — the
# level mod must be tagged and the asset mod must not.
fixture "$WORK/ss-level" grim-garden
fixture "$WORK/ss-assets" music-swap
rm -f "$CARDS/card1.mcr.modset"
dd if=/dev/zero of="$CARDS/card1.mcr" bs=1024 count=128 2>/dev/null
mc_run --mods-dir "$WORK/ss-level" --adopt-saves >/dev/null 2>&1
if grep -q "^grim-garden@" "$CARDS/card1.mcr.modset" 2>/dev/null; then
    ok "custom-level mod joins the tag (derived mutative without code)"
else
    bad "level mod absent from the tag — the [[level]] derivation broke"
fi
rm -f "$CARDS/card1.mcr.modset"
mc_run --mods-dir "$WORK/ss-assets" --adopt-saves >/dev/null 2>&1
if [ ! -e "$CARDS/card1.mcr.modset" ]; then
    ok "asset-only mod stays out of the tag (derived non-mutative)"
else
    bad "asset-only mod tagged the card: $(cat "$CARDS/card1.mcr.modset")"
fi

# ── 15. zip mods: folder or zip, identical (P1 build+load) ──────────────
# hello-anchor zipped, dropped as <id>.zip, loads end-to-end (unpack →
# content-hash-cached compile → dlopen); a second boot reuses both
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

# ── 17. process handoff: checkpoint → exec → restore ────────────────────
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
# re-runs entry — "v2 alive" must appear in the SAME session.
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
    ok "v1 ran, reload fired, v2 ran — same session"
else
    bad "hot reload sequence incomplete (see $WORK/hot.log)"
    grep -E "hot" "$WORK/hot.log" | head -5
fi

# ── mod shaders: engine-compiled GLSL postfx visibly present in the
# headless dump (plans/openpete-mod-shaders.md S3). The headless
# screenshot path composes native_rtt, which BYPASSES off_image — a
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
# cache hit — a hit logs no compile line) AND postfx_register/enable
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
    #  1. presence — crt scanlines alternate rows, so the adjacent-row
    #     luma delta exceeds the skip-row delta (~2.5x with the filter,
    #     ~0.6x without);
    #  2. orientation — the filtered dump's row-luma profile must
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
        ok "PIL unavailable — content assertion skipped (compile assert only)"
    fi
fi

# ── mod shaders S5: the SCENE injection point processes the 3D scene
# UNDER the HUD. scene-sepia sepia-tones the scene at the SCENE point,
# then draws a yellow HUD marker AFTER. The decisive proof is that the
# marker stays PURE YELLOW: if the SCENE split were broken (effect ran
# over everything, or the load-rebegin lost the scene), the marker
# would be sepia'd or the scene black. Assert the marker is yellow
# (under-HUD) AND a scene sample is sepia (SCENE ran). Windowed + a
# real level (--skip-to-level 10) so the frame has 3D content.
# NB --skip-to-level fast-forwards presents until the level is playable
# (~f1700 for the induced Artisans demo), so the recording only spans
# the in-level tail — size the frame limit past the load and sample
# near the END of the video, never at a fixed offset from its start.
say "mod shaders: SCENE point processes the scene under the HUD"
if command -v ffmpeg >/dev/null 2>&1 && python3 -c "import PIL" 2>/dev/null; then
    fixture "$WORK/sepia" scene-sepia
    timeout 300 "$BIN" --disc "$DISC" --run-recomp 0x80012204 \
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
hud = px[int(w*0.09), int(h*0.09)]           # the mod's HUD marker (post-SCENE)
sc  = px[int(w*0.5),  int(h*0.6)]            # 3D scene
hud_yellow = hud[0] > 200 and hud[1] > 200 and hud[2] < 70
scene_sepia = sc[0] >= sc[1] >= sc[2] and (sc[0] - sc[2]) > 15 and sc[0] > 40
print("OK" if hud_yellow and scene_sepia else f"BAD hud={hud} scene={sc}")
PYEOF
)
        if [ "$sepia_v" = "OK" ]; then
            ok "HUD marker crisp yellow over a sepia scene — SCENE runs under the HUD"
        else
            bad "SCENE under-HUD not confirmed ($sepia_v — see $WORK/sepia.log)"
        fi
    elif grep -qiE "no vulkan|no display|surface|failed to (create|open)" "$WORK/sepia.log"; then
        ok "no display for the windowed SCENE record — assertion skipped"
    else
        bad "recording failed for the SCENE rung (see $WORK/sepia.log)"
    fi
else
    ok "ffmpeg/PIL unavailable — SCENE recording assertion skipped"
fi

# ── declarative materials: a shader mod with NO C at all ───────────────
# The zero-C tier's whole claim is that a manifest plus a .frag is a
# complete mod, so the rung asserts exactly that: the mod loads down the
# declarative path (never compiled), its [[material]] rows arm, and the
# geometry they select actually changes on screen. The pixel leg is a
# controlled A/B (same frame and seed, mod present vs absent) rather than
# a colour classifier — "some pixels moved" is the structural claim; which
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
    ok "loaded down the declarative path — nothing was compiled"
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
    bad "the refused row armed anyway — it would match every channel"
else
    ok "no selector armed from the refused row"
fi

say "declarative materials: the rows actually repaint their geometry"
if python3 -c "import PIL" 2>/dev/null; then
    mkdir -p "$WORK/declnone"; : > "$WORK/declnone/enabled.txt"
    for leg in none decl; do
        d="$WORK/decl"; [ "$leg" = none ] && d="$WORK/declnone"
        timeout 300 "$BIN" --headless --no-config --disc "$DISC" \
            --run-recomp 0x80012204 --renderer vk --mute 1 \
            --bk2 games/spyro-1/bk2/flame.bk2 --screenshot-native-only \
            --mods-dir "$d" --screenshot-on-frame 1800 \
            --screenshot-output "$WORK/decl-$leg-{n}.png" \
            --frame-limit 1802 --max-runtime-seconds 280 \
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
        ok "no headless vk capture available — pixel assertion skipped"
    fi
else
    ok "PIL unavailable — declarative pixel assertion skipped"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "[modtest] ALL GREEN"
else
    echo "[modtest] FAILURES — see above"
fi
exit $fail
