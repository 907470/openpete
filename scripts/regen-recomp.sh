#!/usr/bin/env bash
# regen-recomp.sh — re-run psxrecomp on Spyro 1's PSX.EXE plus every
# overlay in the matching-decomp's build/wad/. Outputs land under
# games/spyro-1/generated/. Run whenever you bump psxrecomp, change a
# forced-entry, or pull a new EXE dump.
#
# Inputs (relative to repo root):
#   third_party/psxrecomp/build/PSXRecomp           — recompiler binary
#   games/spyro-1/spyro-1/build/PSX.EXE              — main EXE input
#   games/spyro-1/spyro-1/build/wad/*.ovl            — overlay binaries
#   games/spyro-1/manifest/psyq.toml                 — PSYQ symbol manifest
#
# All overlays share kseg0 vaddr 0x8007AA38 — only one is in RAM at a
# time, so per-overlay symbol namespaces are disambiguated via
# --func-prefix. The generated dispatch fn is sed-renamed from
# `psx_dispatch_compiled` to `psx_dispatch_<name>` so each overlay can
# coexist at link.
#
# Outputs (overwritten):
#   games/spyro-1/generated/PSX.EXE_full.c
#   games/spyro-1/generated/PSX.EXE_dispatch.c
#   games/spyro-1/generated/discovered_functions.log
#   games/spyro-1/generated/<name>.bin                       (synth EXE)
#   games/spyro-1/generated/<name>/<name>.bin_full.c
#   games/spyro-1/generated/<name>/<name>.bin_dispatch.c
#   games/spyro-1/generated/<name>/jumptable_targets.txt
#   games/spyro-1/generated/spyro_overlay_table.c            (auto table)

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
JTBL_TOOL="tools/extract_jumptable_targets.py"
PSYQ_MANIFEST="games/spyro-1/manifest/psyq.toml"
ASM_TARGETS_DIR="games/spyro-1/manifest/asm_dispatch_targets"

# Refresh the per-binary forced-entry manifests from the current asm
# tree. extract_asm_dispatch_targets.py harvests `glabel D_<addr>`
# jumptable slots; extract_handwritten_ra_returns.py appends return PCs
# for callers of handwritten functions that return via `jr <reg>` (the
# recompiler can't statically prove the target so the return PC has to
# be a known dispatch entry). Order matters — the ra-return tool merges
# into the file the first tool wrote.
echo "[regen] Refreshing $ASM_TARGETS_DIR/"
python3 tools/extract_asm_dispatch_targets.py \
    games/spyro-1/spyro-1/asm "$ASM_TARGETS_DIR" >/dev/null
python3 tools/extract_handwritten_ra_returns.py \
    games/spyro-1/spyro-1/asm "$ASM_TARGETS_DIR" >/dev/null

# Resolve the asm_dispatch_targets file for a binary, or empty string if
# none. Overlays share vaddr 0x8007AA38 with different bytes — passing
# main-EXE entries to an overlay regen would silently fail psxrecomp's
# range check (good), but an overlay-A entry passed to overlay-B would
# pass validation and force psxrecomp to translate garbage. Per-binary
# files keep ownership unambiguous.
asm_targets_for() {
    local f="$ASM_TARGETS_DIR/$1.txt"
    [[ -f "$f" ]] && echo "$f"
}

# --- Overlay regen helper -----------------------------------------------

# process_overlay <name> <ovl_path> <prefix> <load_addr> [extra-args...]
#
# Two-pass regen: first translate the synthesized PS1-EXE wrapper to
# discover jump tables, extract those table targets, then re-translate
# with each table entry as a forced-entry. Finally sed-rename the
# dispatch fn so it can coexist with other overlays at link.
process_overlay() {
    local name="$1"; local ovl_path="$2"; local prefix="$3"; local load_addr="$4"
    shift 4
    local extra_args=("$@")
    if [[ ! -f "$ovl_path" ]]; then
        echo "[regen] $ovl_path missing — skipping $name (build the matching decomp first)"
        return 1
    fi
    mkdir -p "$OUT/$name"
    local asm_targets
    asm_targets=$(asm_targets_for "$name")
    local base_args=(
        --output-dir="$OUT/$name"
        --external-symbols="$PSYQ_MANIFEST"
        --external-prefix=psyq_
        --func-prefix="$prefix"
        --skip-on-error
    )

    echo "[regen] $name: synth + first-pass recomp"
    python3 "$SYNTH" "$ovl_path" "$OUT/$name.bin" --load-addr="$load_addr"
    # First pass: handwritten-asm dispatch targets only (jtbl doesn't
    # exist yet). psxrecomp accepts a single --extra-funcs= file.
    local first_funcs="$OUT/$name/extra_funcs.first.txt"
    : > "$first_funcs"
    [[ -n "$asm_targets" ]] && cat "$asm_targets" >> "$first_funcs"
    "$PSXRECOMP" "$OUT/$name.bin" "${base_args[@]}" \
        --extra-funcs="$first_funcs" "${extra_args[@]}"

    local jtbl="$OUT/$name/jumptable_targets.txt"
    python3 "$JTBL_TOOL" \
        "$OUT/$name/$name.bin_full.c" \
        "$ovl_path" \
        --load-addr="$load_addr" \
        "$jtbl"

    # Second pass: union of asm dispatch targets + jumptable targets.
    local second_funcs="$OUT/$name/extra_funcs.second.txt"
    : > "$second_funcs"
    [[ -n "$asm_targets" ]] && cat "$asm_targets" >> "$second_funcs"
    [[ -f "$jtbl" ]] && cat "$jtbl" >> "$second_funcs"
    echo "[regen] $name: second-pass recomp with jumptable forced entries"
    "$PSXRECOMP" "$OUT/$name.bin" "${base_args[@]}" \
        --extra-funcs="$second_funcs" \
        --emit-external-jals="$OUT/$name/external_jals.txt" \
        "${extra_args[@]}"
    sed -i "s/\bpsx_dispatch_compiled\b/psx_dispatch_${name}/g" \
        "$OUT/$name/$name.bin_dispatch.c"
    return 0
}

# --- Overlay registry ---------------------------------------------------
#
# Filename convention from the matching decomp's build/wad/:
#   level_<F>_*_code.ovl  — overlay_id = F + 11, section name "level_NN"
#                            where NN = (F/6 + 1)*10 + (F%6).
#   titlescreen_code.ovl  — overlay_id = 48
#   credits.ovl           — overlay_id = 47
#
# Overlay-id is the LONG inserted at the head of each overlay section by
# overlays.ld; psyq_thunks.cpp reads it from RAM at 0x8007AA38 to know
# which overlay's bytes are live and routes dispatch accordingly.

OVERLAY_NAMES=()
declare -A OVERLAY_MAGIC=()

register_overlay() {
    OVERLAY_NAMES+=("$1")
    OVERLAY_MAGIC["$1"]="$2"
}

# Titlescreen — different load pattern (no level-id namespace) and a
# couple of static forced entries that pre-date the multi-entry pass.
if process_overlay titlescreen "$WAD_DIR/titlescreen_code.ovl" \
        func_titlescreen_ 0x8007AA38 \
        --forced-entry=0x8007ABAC --forced-entry=0x8007CEE4; then
    register_overlay titlescreen 48
fi

# Credits screen.
if process_overlay credits "$WAD_DIR/credits.ovl" \
        func_credits_ 0x8007AA38; then
    register_overlay credits 47
fi

# Level overlays — derive section name + overlay-id from filename.
for ovl_path in "$WAD_DIR"/level_*_code.ovl; do
    [[ -f "$ovl_path" ]] || continue
    base=$(basename "$ovl_path")
    F=$(sed -E 's/^level_([0-9]+)_.*$/\1/' <<< "$base")
    [[ "$F" =~ ^[0-9]+$ ]] || { echo "[regen] skipping unrecognized $base"; continue; }
    H=$((F / 6 + 1))
    S=$((F % 6))
    section="level_$((H * 10 + S))"
    magic=$((F + 11))
    extra=()
    # level_10 (Artisans Homeworld) has a forced-entry at the
    # SetOverlayPointers caller's first computed-jump target.
    [[ "$section" == "level_10" ]] && extra+=(--forced-entry=0x8007D9C8)
    # level_12 (Dark Hollow) — runtime jalr targets observed on entry
    # from level_10. Each address is a function pointer loaded from
    # RAM into v0/v1 and dispatched via jalr; the static analyzer
    # can't follow the data flow, so they have to be forced-entered.
    # 0x80086134 and 0x800873E0 are *past* dark_hollow's overlay end
    # (0x80085A38) — they're stale function pointers from level_10's
    # init that survive the overlay swap. Not added here; the calls
    # silently no-op via stub miss, which appears to be benign.
    if [[ "$section" == "level_12" ]]; then
        extra+=(--forced-entry=0x800857CC
                --forced-entry=0x8007D9C8
                --forced-entry=0x8007AE40)
    fi
    # level_13 (Town Square) — same family of function-pointer-table
    # callbacks as level_12. All four addresses fall inside this
    # overlay's range (0x8007AA38..0x80089A38), so unlike level_12 the
    # 0x80086134/0x800873E0 pair are real targets here, not stale
    # pointers — observed as stub misses on Town Square entry.
    if [[ "$section" == "level_13" ]]; then
        extra+=(--forced-entry=0x8007D9C8
                --forced-entry=0x800857CC
                --forced-entry=0x80086134
                --forced-entry=0x800873E0)
    fi
    if process_overlay "$section" "$ovl_path" "func_${section}_" 0x8007AA38 "${extra[@]}"; then
        register_overlay "$section" "$magic"
    fi
done

# --- Main EXE -----------------------------------------------------------
#
# Done LAST so we can fold each overlay's external-JAL targets into the
# main-EXE forced-entry set. Overlays JAL into main-EXE bytes (gem
# accounting, level setup, audio helpers); the main-EXE static JAL pass
# can't see those callers, so without this union the called functions
# stub-miss at runtime as multi-entry / fall-through fast-paths.
#
# Forced entries:
#   0x80032AB0 — fast-path entry only reached from the titlescreen
#                  overlay; the new multi-entry pass picks it up too,
#                  but kept as belt-and-braces.
#   0x8004F8C8, 0x80050060, 0x800506EC, 0x80038620, 0x8005A7B0 —
#                  runtime stub-miss targets observed on level_10 and at
#                  Stone Hill entry. Re-validated against
#                  bk2/stone-hill-entry.bk2: byte-identical screenshot
#                  vs no-forced-entries baseline at frame 3600, so the
#                  earlier "breaks geometry past Artisans" report could
#                  not be reproduced.
echo "[regen] Recompiling PSX.EXE (with overlay-discovered cross-binary JAL targets)"
main_exe_extra="$OUT/main_exe_extra_funcs.txt"
: > "$main_exe_extra"
main_exe_targets=$(asm_targets_for main_exe)
[[ -n "$main_exe_targets" ]] && cat "$main_exe_targets" >> "$main_exe_extra"
for f in "$OUT"/*/external_jals.txt; do
    [[ -f "$f" ]] && cat "$f" >> "$main_exe_extra"
done
main_exe_args=(
    --output-dir="$OUT"
    --external-symbols="$PSYQ_MANIFEST"
    --external-prefix=psyq_
    --forced-entry=0x80032AB0
    --forced-entry=0x8004F8C8
    --forced-entry=0x80050060
    --forced-entry=0x800506EC
    --forced-entry=0x80038620
    --forced-entry=0x8005A7B0
    --forced-entry=0x8005A848
    --extra-funcs="$main_exe_extra"
    --skip-on-error
)
"$PSXRECOMP" games/spyro-1/spyro-1/build/PSX.EXE "${main_exe_args[@]}"
sed -i 's/\bpsx_dispatch_compiled\b/psx_dispatch_main_exe/g' \
    "$OUT/PSX.EXE_dispatch.c"

# --- Generate the dispatch table ----------------------------------------

echo "[regen] Generating $OUT/spyro_overlay_table.c (${#OVERLAY_NAMES[@]} overlays)"
{
    echo "/* AUTO-GENERATED by scripts/regen-recomp.sh — do not edit. */"
    echo "#include \"spyro_overlay_table.h\""
    echo
    for name in "${OVERLAY_NAMES[@]}"; do
        echo "extern int psx_dispatch_${name}(CPUState* cpu, uint32_t addr);"
    done
    echo
    echo "const spyro_overlay_entry_t spyro_overlay_table[] = {"
    for name in "${OVERLAY_NAMES[@]}"; do
        printf '    { %3d, "%s", psx_dispatch_%s },\n' \
            "${OVERLAY_MAGIC[$name]}" "$name" "$name"
    done
    echo "};"
    echo "const size_t spyro_overlay_table_count = sizeof(spyro_overlay_table) / sizeof(spyro_overlay_table[0]);"
} > "$OUT/spyro_overlay_table.c"

echo "[regen] Done — rebuild the project to pick up the new C output."
