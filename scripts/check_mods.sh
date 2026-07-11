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
# Usage: scripts/check_mods.sh   (repo root; ~8 min — ctest mod_poc_ladder)
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

# ── 8. [test]-block rungs through the real modtest runner ───────────────
say "[test] blocks via tools/modtest.py (the harness modders run)"
if python3 tools/modtest.py mods/hello-anchor mods/raw-probe \
        mods/double-gems mods/natural-gems mods/lib-and-client \
        mods/hud-stopwatch mods/state-rewind; then
    ok "modtest runner: all [test] blocks green"
else
    bad "modtest runner reported failures"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "[modtest] ALL GREEN"
else
    echo "[modtest] FAILURES — see above"
fi
exit $fail
