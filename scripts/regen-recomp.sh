#!/usr/bin/env bash
# regen-recomp.sh — re-run psxrecomp on Spyro 1's PSX.EXE and titlescreen
# overlay, install the generated C into games/spyro-1/generated/. Run this
# whenever you bump psxrecomp, change a forced-entry, or pull a new EXE dump.
#
# Inputs (relative to repo root):
#   third_party/psxrecomp/build/PSXRecomp           — recompiler binary
#   games/spyro-1/spyro-1/build/PSX.EXE              — main EXE input
#   games/spyro-1/manifest/psyq.toml                 — PSYQ symbol manifest
#   games/spyro-1/generated/titlescreen.bin          — titlescreen overlay binary
#                                                       (synthesized PS1-EXE
#                                                        wrapper, vaddr 0x8007AA38)
#
# Outputs (overwritten):
#   games/spyro-1/generated/PSX.EXE_full.c
#   games/spyro-1/generated/PSX.EXE_dispatch.c
#   games/spyro-1/generated/discovered_functions.log
#   games/spyro-1/generated/titlescreen/titlescreen.exe_full.c
#   games/spyro-1/generated/titlescreen/titlescreen.exe_dispatch.c

set -euo pipefail

cd "$(dirname "$0")/.."

PSXRECOMP="third_party/psxrecomp/build/PSXRecomp"
if [[ ! -x "$PSXRECOMP" ]]; then
    echo "psxrecomp not found at $PSXRECOMP — run cmake on third_party/psxrecomp first" >&2
    exit 1
fi

OUT="games/spyro-1/generated"
mkdir -p "$OUT/titlescreen"

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

if [[ -f "$OUT/titlescreen.bin" ]]; then
    echo "[regen] Recompiling titlescreen overlay"
    "$PSXRECOMP" "$OUT/titlescreen.bin" \
        --output-dir="$OUT/titlescreen" \
        --external-symbols=games/spyro-1/manifest/psyq.toml \
        --external-prefix=psyq_ \
        --forced-entry=0x8007ABAC \
        --forced-entry=0x8007CEE4
else
    echo "[regen] $OUT/titlescreen.bin missing — skipping titlescreen overlay regen."
    echo "        See plan §Phase 1d.5 for the synth-header extraction recipe;"
    echo "        the existing $OUT/titlescreen/*.c outputs stay valid as-is."
fi

echo "[regen] Done — rebuild the project to pick up the new C output."
