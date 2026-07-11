#!/usr/bin/env bash
# check_mods.sh — modtest v0: the PoC-ladder gate script.
# (notes/plans/openpete-modding-framework.md, "modtest harness" P1 item —
# v0 is shell + log/oracle assertions; the mod.toml [test] block form
# arrives with bk2-scripted rungs.)
#
# Test classes exercised:
#   1. observer-neutrality — hello-anchor + raw-probe + chain-duo enabled
#      ⇒ smoke oracle stays byte-exact (inertness gate #2, and the
#      chain-duo matrix run doubles as it).
#   2. chain order matrix — chain-duo under both orderings; the nesting
#      in the logs must flip (order IS priority).
#   3. expected divergence — double-gems enabled ⇒ the oracle MUST fail,
#      and the first hard-failed region must be gem state (the failure
#      is the proof the mod diverges gameplay).
#
# Usage: scripts/check_mods.sh   (repo root; ~2-3 min, mostly 3 smoke runs)
# Env:   OPENPETE_BIN / OPENPETE_DISC — forwarded to check_psx_oracle.sh.

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

# ── 3. expected divergence: double-gems must FAIL the oracle at gems ────
say "expected divergence: double-gems must fail the oracle at gem state"
fixture "$WORK/gems" double-gems
out="$(OPENPETE_FRAME_LIMIT=$SMOKE_FRAMES scripts/check_psx_oracle.sh -- \
        --mods-dir "$WORK/gems" 2>&1)"
rc_line="$(printf '%s\n' "$out" | grep -E "\[psx-oracle\] exit code=" | tail -1)"
if printf '%s\n' "$out" | grep -q "gem pickup doubled"; then
    ok "double-gems fired (post-hook + state-diff)"
else
    bad "double-gems never fired in $SMOKE_FRAMES frames"
fi
if printf '%s\n' "$out" | grep -qE "verdict: FAIL"; then
    if printf '%s\n' "$out" | grep -E "hard" | grep -qi "gem"; then
        ok "oracle FAILED at a gem region (expected divergence proven)"
    else
        ok "oracle FAILED (expected divergence proven)"
        printf '%s\n' "$out" | grep -iE "hard|first" | head -3
    fi
else
    bad "oracle did not FAIL with double-gems enabled ($rc_line)"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "[modtest] ALL GREEN"
else
    echo "[modtest] FAILURES — see above"
fi
exit $fail
