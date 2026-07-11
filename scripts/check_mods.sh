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
#   - advisory above/below violation => actionable move-this-mod error
#   - crash attribution naming the offending mod
#   - decomp promotion: legacy raw alias resolves + warns (churn test)
# plus the tools/modtest.py run over every [test]-block rung.
#
# Usage: scripts/check_mods.sh   (repo root; ~10 min — ctest mod_poc_ladder;
#        section 13 rebakes/rebuilds the main EXE around the fade-skip rung)
# Env:   OPENPETE_BIN / OPENPETE_DISC — forwarded to runs + oracle.

set -u
cd "$(dirname "$0")/.."

MODS_SRC="$PWD/mods"
WORK="$(mktemp -d /tmp/openpete-modtest.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

BIN="${OPENPETE_BIN:-build/linux-release/games/spyro-1/openpete-spyro1}"
DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
SMOKE_FRAMES="${OPENPETE_MODTEST_FRAMES:-8000}"

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

run_game() { # run_game <mods_dir> <frames> -> stdout+stderr
    # NB: --frame-limit is the engine flag; OPENPETE_FRAME_LIMIT is a
    # check_psx_oracle.sh convention, NOT read by the engine itself.
    timeout 300 "$BIN" --headless --disc "$DISC" \
        --run-recomp 0x80012204 --frame-limit "$2" --mods-dir "$1" 2>&1
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
if printf '%s\n' "$out" | grep -q "declares raw-tier symbols"; then
    ok "raw-tier warning printed"
else
    bad "raw-tier warning missing"
fi

# ── 4. lib-and-client: deps topo-sort + export/import (rung 5) ──────────
say "lib-and-client: client listed FIRST — topo-sort must load lib first"
fixture "$WORK/libclient" lib-and-client demo-lib
out="$(run_game "$WORK/libclient" 600)"
if printf '%s\n' "$out" | grep -q "imported demo-lib:magic -> 42"; then
    ok "cross-mod import worked despite reversed enabled order"
else
    bad "client did not import demo-lib:magic"
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
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = 1\ndeps = ["%s"]\n' \
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

# ── 6. advisory above/below: violation = actionable error (not silent) ──
say "advisory ordering: above=[] violation disables with a move error"
for m in adv-a adv-b; do
    mkdir -p "$WORK/advsrc/$m/src"
    printf '#include <openpete_mod_api.h>\nint openpete_mod_entry(const openpete_mod_api_t* a, openpete_mod_t* s){(void)a;(void)s;return 0;}\n' \
        > "$WORK/advsrc/$m/src/m.c"
done
printf '[mod]\nid = "adv-a"\nversion = "0.0.1"\napi = 1\n' \
    > "$WORK/advsrc/adv-a/mod.toml"
printf '[mod]\nid = "adv-b"\nversion = "0.0.1"\napi = 1\nabove = ["adv-a"]\n' \
    > "$WORK/advsrc/adv-b/mod.toml"
mkdir -p "$WORK/adv"
ln -sfn "$WORK/advsrc/adv-a" "$WORK/adv/adv-a"
ln -sfn "$WORK/advsrc/adv-b" "$WORK/adv/adv-b"
printf 'adv-b\nadv-a\n' > "$WORK/adv/enabled.txt"   # violates above
out="$(run_game "$WORK/adv" 600)"
if printf '%s\n' "$out" | grep -q 'move "adv-b" AFTER "adv-a"'; then
    ok "violation produced the move-this-mod error"
else
    bad "advisory violation error absent/wrong"
fi
if printf '%s\n' "$out" | grep -q "1 mod active"; then
    ok "only the compliant mod loaded"
else
    bad "unexpected active-mod count"
fi
printf 'adv-a\nadv-b\n' > "$WORK/adv/enabled.txt"   # satisfies above
out="$(run_game "$WORK/adv" 600)"
if printf '%s\n' "$out" | grep -q "2 mods active"; then
    ok "compliant order loads both"
else
    bad "compliant order did not load both"
fi

# ── 7. decomp promotion: legacy raw alias resolves + warns (churn test) ─
say "promotion contract: override via legacy raw name of a renamed fn"
mkdir -p "$WORK/aliassrc/alias-probe/src"
printf '[mod]\nid = "alias-probe"\nversion = "0.0.1"\napi = 1\n' \
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
    mkdir -p "$WORK/duelsrc/$m/assets/textures/replacements"
    printf '[mod]\nid = "%s"\nversion = "0.0.1"\napi = 1\n' "$m" \
        > "$WORK/duelsrc/$m/mod.toml"
    python3 - "$WORK/duelsrc/$m/assets/textures/replacements/title_ui.png" <<'PY'
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
printf '[mod]\nid = "wad-identity"\nversion = "0.0.1"\napi = 1\n' \
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
printf '[mod]\nid = "wad-recolor"\nversion = "0.0.1"\napi = 1\n' \
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

# ── 10. actor render service: ghost-lite record → replay (rung 16) ─────
# Two runs against a COPIED mod (not a symlink) so the data-dir recording
# is fixture-local: run 1 records poses through capture_actor_pose (the
# hook-consistent snapshot works headless), run 2 loads them back (the
# data-dir service round-trip). Draw-side proof is visual/windowed (the
# present hook never runs headless); neutrality-while-capturing is the
# mod's own [test] oracle gate in section 11.
say "ghost-lite: pose recording (run 1) -> race-ghost load (run 2)"
mkdir -p "$WORK/ghost"
cp -r "$MODS_SRC/ghost-lite" "$WORK/ghost/ghost-lite"
rm -rf "$WORK/ghost/ghost-lite/data" "$WORK/ghost/ghost-lite/.build"
printf 'ghost-lite\n' > "$WORK/ghost/enabled.txt"
out="$(run_game "$WORK/ghost" "$SMOKE_FRAMES")"
if printf '%s\n' "$out" | grep -q "ghost-lite: recording" &&
   printf '%s\n' "$out" | grep -q "ghost-lite: ghost armed"; then
    ok "run 1: captured + armed (recording via CameraUpdate tick hook)"
else
    bad "run 1: recording/armed logs missing"
fi
POSE_FILE="$WORK/ghost/ghost-lite/data/ghost.s1pose"
if [ -s "$POSE_FILE" ] && [ $(( $(stat -c%s "$POSE_FILE") % 244 )) -eq 0 ]; then
    ok "run 1: pose file written ($(stat -c%s "$POSE_FILE") bytes, 244-byte records)"
else
    bad "run 1: pose file missing or not a whole number of 244-byte records"
fi
out="$(run_game "$WORK/ghost" "$SMOKE_FRAMES")"
if printf '%s\n' "$out" | grep -qE "ghost-lite: loaded [0-9]+ recorded poses"; then
    ok "run 2: prior recording loaded (data-dir service round-trip)"
else
    bad "run 2: prior recording not loaded"
fi

# ── 11. triangle-flutter dormant gate (rung 13, flagship) ───────────────
# The [test] block (section 12) proves the flutter FIRES on its bundled
# glide+Triangle movie. This case proves the inverse: enabled over the
# stock attract demo — which never glides — the gameplay mod stays
# DORMANT and the oracle stays byte-exact (the fire condition never
# triggers spuriously; the hook itself is observer-neutral).
say "triangle-flutter: dormant over the attract demo (oracle-exact)"
fixture "$WORK/flutter" triangle-flutter
out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES scripts/check_psx_oracle.sh -- \
        --mods-dir "$WORK/flutter" 2>&1)"
if printf '%s\n' "$out" | grep -q "triangle-flutter: armed"; then
    ok "hook armed (dispatcher override live)"
else
    bad "flutter hook never armed"
fi
if printf '%s\n' "$out" | grep -q "triangle-flutter: flutter"; then
    bad "flutter fired during the attract demo (spurious trigger)"
else
    ok "no spurious fire (demo has no player glides)"
fi
if printf '%s\n' "$out" | grep -q "verdict: PASS"; then
    ok "oracle PASS with the gameplay mod enabled but dormant"
else
    bad "oracle did not PASS with dormant triangle-flutter"
fi

# ── 12. [test]-block rungs through the real modtest runner ──────────────
# (fade-skip is NOT in this sweep: its [test] needs its sites BAKED into
#  the binary — section 13 orchestrates bake → test → restore around it.)
say "[test] blocks via tools/modtest.py (the harness modders run)"
if python3 tools/modtest.py mods/hello-anchor mods/raw-probe \
        mods/double-gems mods/natural-gems mods/lib-and-client \
        mods/hud-stopwatch mods/state-rewind mods/ghost-lite \
        mods/triangle-flutter mods/new-moby mods/mirror-level \
        mods/arena-probe \
        mods/hd-single-texture mods/music-swap; then
    ok "modtest runner: all [test] blocks green"
else
    bad "modtest runner reported failures"
fi

# ── 13. fade-skip: the instruction-site hook tier (rung 14) ─────────────
# Bake the mod's [[site]] set into the recompiled C (scoped main-EXE
# regen, ~9 s), rebuild (~10 s), prove the three inertness tiers, then
# restore stock and assert BYTE-IDENTITY of the regenerated C:
#   a) baked + mod loaded  → modtest: all 6 slti loop-bound sites FIRE
#   b) baked + mod ABSENT  → oracle smoke PASS (guarded identity arm)
#   c) restored (set empty)→ generated C byte-identical to pre-bake
# Needs the regen toolchain (PSXRecomp + PSX.EXE); skip cleanly if absent.
say "fade-skip: site-hook tier — bake, fire, guarded-inert, restore"
if [[ -x third_party/psxrecomp/build/PSXRecomp &&
      -f games/spyro-1/spyro-1/build/PSX.EXE ]]; then
    GEN=games/spyro-1/generated
    stock_full=$(sha256sum "$GEN/PSX.EXE_full.c" | cut -d' ' -f1)
    fixture "$WORK/fadeskip" fade-skip
    mkdir -p "$WORK/empty-mods"; : > "$WORK/empty-mods/enabled.txt"
    if OPENPETE_MODS_DIR="$WORK/fadeskip" bash scripts/regen-mod-sites.sh \
            >"$WORK/bake.log" 2>&1 \
       && cmake --build --preset linux-release >>"$WORK/bake.log" 2>&1; then
        ok "bake: 6 sites regenerated + rebuilt"
        # (a) baked + loaded: the mod's own [test] block, live sites
        if python3 tools/modtest.py mods/fade-skip; then
            ok "baked+loaded: all six loop-bound sites fired"
        else
            bad "fade-skip modtest failed with sites baked"
        fi
        # (b) baked + absent: guarded arm must be sim-inert vs hardware
        out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES scripts/check_psx_oracle.sh \
                -- --mods-dir "$WORK/empty-mods" 2>&1)"
        if printf '%s\n' "$out" | grep -q "verdict: PASS"; then
            ok "baked+absent: oracle PASS (guarded identity arm)"
        else
            bad "oracle failed on the baked-but-inactive binary"
        fi
    else
        bad "site bake (regen-mod-sites.sh + rebuild) failed — see $WORK/bake.log"
    fi
    # (c) restore: empty site set must regenerate byte-identical stock C
    if OPENPETE_MODS_DIR="$WORK/empty-mods" bash scripts/regen-mod-sites.sh \
            >"$WORK/restore.log" 2>&1 \
       && cmake --build --preset linux-release >>"$WORK/restore.log" 2>&1; then
        restored_full=$(sha256sum "$GEN/PSX.EXE_full.c" | cut -d' ' -f1)
        if [[ "$restored_full" == "$stock_full" ]]; then
            ok "restore: generated C byte-identical to pre-bake stock"
        else
            bad "restore left generated C differing from stock (inertness gate)"
        fi
    else
        bad "site restore failed — see $WORK/restore.log; run scripts/regen-mod-sites.sh manually"
    fi
else
    ok "SKIP (regen toolchain not present — PSXRecomp / PSX.EXE missing)"
fi

# ── 14. per-modset save safety: tag + warn + adopt (memcard sidecar) ────
# The modset tag ("id@ver+...", sim_neutral mods excluded) is checked at
# engine startup against the card's sidecar. Five flows, ~300 frames each:
# fresh-vanilla silence, vanilla-card+mods warn, mismatch warn, explicit
# adopt (sidecar rewritten), tagged-card+vanilla warn. The last uses a
# sim_neutral observer to prove neutral mods don't taint the tag.
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
    ok "tagged card + observer-only modset: warned (sim_neutral excluded from tag)"
else
    bad "tagged-card/vanilla warning missing (sim_neutral exclusion broken?)"
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
printf '[mod]\nid = "hot-greeter"\nversion = "0.0.1"\napi = 1\n' \
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

echo
if [ "$fail" -eq 0 ]; then
    echo "[modtest] ALL GREEN"
else
    echo "[modtest] FAILURES — see above"
fi
exit $fail
