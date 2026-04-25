#!/usr/bin/env bash
# regen-recomp.sh — re-run psxrecomp on Spyro 1's PSX.EXE and titlescreen
# overlay, install the generated C into games/spyro-1/generated/. Run this
# whenever you bump psxrecomp, change a forced-entry, or pull a new EXE dump.
#
# Inputs (relative to repo root):
#   third_party/psxrecomp/build/PSXRecomp           — recompiler binary
#   games/spyro-1/spyro-1/build/PSX.EXE              — main EXE input
#   games/spyro-1/manifest/psyq.toml                 — PSYQ symbol manifest
#
# Overlay binaries are synthesized on-demand from the matching-decomp's
# build/wad/*.ovl outputs by tools/synth_overlay_exe.py. They share the
# same kseg0 vaddr (0x8007AA38) — only one overlay is in RAM at a time, so
# their function-symbol namespaces are disambiguated via --func-prefix.
#
# Outputs (overwritten):
#   games/spyro-1/generated/PSX.EXE_full.c
#   games/spyro-1/generated/PSX.EXE_dispatch.c
#   games/spyro-1/generated/discovered_functions.log
#   games/spyro-1/generated/titlescreen.bin
#   games/spyro-1/generated/titlescreen/titlescreen.bin_full.c
#   games/spyro-1/generated/titlescreen/titlescreen.bin_dispatch.c
#   games/spyro-1/generated/level_10.bin
#   games/spyro-1/generated/level_10/level_10.bin_full.c
#   games/spyro-1/generated/level_10/level_10.bin_dispatch.c

set -euo pipefail

cd "$(dirname "$0")/.."

PSXRECOMP="third_party/psxrecomp/build/PSXRecomp"
if [[ ! -x "$PSXRECOMP" ]]; then
    echo "psxrecomp not found at $PSXRECOMP — run cmake on third_party/psxrecomp first" >&2
    exit 1
fi

OUT="games/spyro-1/generated"
WAD_DIR="games/spyro-1/spyro-1/build/wad"
SYNTH="tools/synth_overlay_exe.py"

mkdir -p "$OUT/titlescreen" "$OUT/level_10"

# Main EXE forced entries:
#   0x80024054 — code label inside r_pete renderer (computed-jump target;
#                  recompiler's static analysis can't see it, so add explicitly)
#   0x80032AB0 — small util only reached from the titlescreen overlay's
#                  cutscene-frame-pump path; main-EXE callers use a different
#                  entry, so the recompiler classified 0x80032AB8 as the
#                  function start and missed 0x80032AB0
echo "[regen] Recompiling PSX.EXE"
"$PSXRECOMP" games/spyro-1/spyro-1/build/PSX.EXE \
    --output-dir="$OUT" \
    --external-symbols=games/spyro-1/manifest/psyq.toml \
    --external-prefix=psyq_ \
    --forced-entry=0x80024054 \
    --forced-entry=0x80032AB0 \
    --skip-on-error

# Per-overlay regen — synth a PS1-EXE wrapper from the matching-decomp's
# build/wad output, then translate it with a unique --func-prefix so per-
# overlay symbols don't collide at link (overlays share vaddr 0x8007AA38).
regen_overlay() {
    local name="$1"        # short tag (titlescreen, level_10, ...)
    local ovl_path="$2"    # source bytes (relative to repo root)
    local load_addr="$3"   # kseg0 vaddr
    local prefix="$4"      # --func-prefix=
    shift 4
    if [[ ! -f "$ovl_path" ]]; then
        echo "[regen] $ovl_path missing — skipping $name overlay regen."
        echo "        Build the matching decomp first ($WAD_DIR)."
        return
    fi
    echo "[regen] Synthesizing $name PS1-EXE wrapper"
    python3 "$SYNTH" "$ovl_path" "$OUT/$name.bin" --load-addr="$load_addr"
    echo "[regen] Recompiling $name overlay"
    "$PSXRECOMP" "$OUT/$name.bin" \
        --output-dir="$OUT/$name" \
        --external-symbols=games/spyro-1/manifest/psyq.toml \
        --external-prefix=psyq_ \
        --func-prefix="$prefix" \
        --skip-on-error \
        "$@"
}

# Forced entries for each overlay are addresses the static analyzer can't
# reach via JAL/prologue heuristics — usually computed-jump or function-
# pointer targets referenced from the main EXE's SetOverlayPointers.
regen_overlay titlescreen \
    "$WAD_DIR/titlescreen_code.ovl" \
    0x8007AA38 \
    func_titlescreen_ \
    --forced-entry=0x8007ABAC \
    --forced-entry=0x8007CEE4

regen_overlay level_10 \
    "$WAD_DIR/level_0_artisans_home_code.ovl" \
    0x8007AA38 \
    func_level_10_ \
    --forced-entry=0x8007D9C8

echo "[regen] Done — rebuild the project to pick up the new C output."
