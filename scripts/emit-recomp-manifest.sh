#!/usr/bin/env bash
# emit-recomp-manifest.sh - snapshot the CONVERGED recomp inputs into
# committed, ROM-hash-keyed metadata.
#
# The multi-pass discovery in regen-recomp.sh exists to converge
# forced-entry address lists. For a known ROM those lists are facts:
# this script snapshots the final per-binary union list + the overlay
# registry + per-binary input hashes into
#   games/<game>/manifest/recomp/<serial>/
# (the serial is recomp.conf's EXE_FILENAME, SCUS_942.28 -> SCUS-94228)
# so the first-boot pipeline can run a SINGLE psxrecomp pass per binary
# with no discovery and no python. Called by regen-recomp.sh after a full
# regen of a game whose recomp.conf sets FIRST_BOOT=1; runnable standalone
# off the last regen's outputs:
#   GAME=spyro-2 bash scripts/emit-recomp-manifest.sh
# scripts/verify-recomp-manifest.sh proves single-pass == multi-pass.
#
# A game whose recomp.conf sets FIRST_BOOT_CARVE=wad_entry also records,
# per overlay, the index of the WAD.WAD entry holding it (wad_entry), which
# its first-boot extractor carves; tools/wad_entry_map.py finds each index
# on the decomp's rebuilt disc image by content. Spyro 1's extractor reads
# the entries from the WAD header's own layout instead.
#
# A game whose disc copy of a binary went through post-link mastering
# transforms names them in recomp.conf: FIRST_BOOT_DECRYPT=1 (an overlay may
# be XORed with the le32 at its byte 4), FIRST_BOOT_RESTORE_SYMBOLS (psx.map
# symbol templates, {name} = the overlay, whose words the disc copy may
# carry rewritten) and FIRST_BOOT_EXE_RESTORE_SYMBOLS (the same for the main
# executable). tools/wad_entry_map.py finds, per binary, which of them turn
# the disc's bytes into the decomp's, and the manifest records the inverse
# the extractor applies after each carve: `decrypt = true` and
# `restore = [[<file offset>, <word>], ...]` (the decomp's words).
#
# The committed snapshot is a distribution input, so it never changes as a
# side effect of a regen: the snapshot is emitted to a scratch directory and
# compared with the committed one, and a difference fails the run. The only
# way to move the committed snapshot is OPENPETE_MANIFEST_BLESS=1, in the
# change set that moved the inputs (a forced-entry or psyq.toml edit, a
# psxrecomp bump).
set -euo pipefail
cd "$(dirname "$0")/.."

GAME="${GAME:-spyro-1}"
GAME_CONF="games/$GAME/recomp.conf"
[[ -f "$GAME_CONF" ]] || { echo "[emit-manifest] no $GAME_CONF" >&2; exit 1; }
# shellcheck source=/dev/null
source "$GAME_CONF"
SERIAL="${EXE_FILENAME/_/-}"; SERIAL="${SERIAL/./}"
OUT=games/$GAME/generated
COMMITTED=games/$GAME/manifest/recomp/$SERIAL
MAN=$(mktemp -d "${TMPDIR:-/tmp}/openpete-manifest-emit.XXXXXX")
trap 'rm -rf "${MAN:?}"' EXIT
FORCED=games/$GAME/manifest/forced_entries
EXE=$PSX_EXE
REGISTRY_SRC=$OUT/spyro_overlay_table.c
REGISTRY_LIST=$OUT/overlay_registry.txt
DISC_BIN="$DECOMP_DIR/build/$SERIAL.bin"

[[ -f "$OUT/PSX.EXE_full.c" && -f "$REGISTRY_SRC" && -f "$REGISTRY_LIST" ]] || {
    echo "[emit-manifest] generated/ incomplete - run GAME=$GAME scripts/regen-recomp.sh first" >&2
    exit 1
}

mkdir -p "$MAN"

# Final converged list for a binary: the last --extra-funcs file the
# regen's final pass consumed, plus the hand-curated forced entries
# (regen now folds those into the passes too; the union keeps this
# correct against pre-fold generated/ trees). Sorted -u for stable diffs
# (psxrecomp treats the list as a set - see FunctionAnalyzer::add_forced_entry).
emit_funcs() {  # emit_funcs <name> <final_funcs_file>
    local name="$1" src="$2"
    { grep -h '^0x' "$src" 2>/dev/null || true
      if [[ -f "$FORCED/$name.txt" ]]; then grep -h '^0x' "$FORCED/$name.txt"; fi
    } | tr 'A-F' 'a-f' | sort -u > "$MAN/$name.funcs.txt"
}

sha() { sha256sum "$1" | cut -d' ' -f1; }

# restore_toml <offset:word,...|->: the `restore = [...]` line, or nothing.
restore_toml() {
    [[ "$1" == "-" ]] && return 0
    local out="" p
    local -a rows
    IFS=',' read -r -a rows <<< "$1"
    for p in "${rows[@]}"; do out+="${out:+, }[${p%%:*}, ${p#*:}]"; done
    echo "restore = [$out]"
}

# --- disc map: WAD entry and mastering inverse per binary ------------------
declare -A WAD_ENTRY=() DECRYPT=() RESTORE=()
MAIN_RESTORE="-"
if [[ "${FIRST_BOOT_CARVE:-}" == "wad_entry" ]]; then
    [[ -f "$DISC_BIN" ]] || { echo "[emit-manifest] $DISC_BIN missing" >&2; exit 1; }
    map_args=()
    if [[ -n "${FIRST_BOOT_DECRYPT:-}${FIRST_BOOT_RESTORE_SYMBOLS:-}${FIRST_BOOT_EXE_RESTORE_SYMBOLS:-}" ]]; then
        map_args=(--map "$DECOMP_DIR/build/psx.map" --load-addr "$OVERLAY_LOAD_ADDR")
        if [[ "${FIRST_BOOT_DECRYPT:-0}" == "1" ]]; then map_args+=(--decrypt); fi
        for s in ${FIRST_BOOT_RESTORE_SYMBOLS:-}; do map_args+=(--restore-symbol "$s"); done
        if [[ -n "${FIRST_BOOT_EXE_RESTORE_SYMBOLS:-}" ]]; then
            map_args+=(--exe "$EXE" --exe-name "$EXE_FILENAME")
            for s in $FIRST_BOOT_EXE_RESTORE_SYMBOLS; do map_args+=(--exe-restore-symbol "$s"); done
        fi
    fi
    MAP_LINES=$(python3 tools/wad_entry_map.py "$DISC_BIN" "$REGISTRY_LIST" "${map_args[@]}") \
        || { echo "[emit-manifest] tools/wad_entry_map.py: not every binary maps to the disc" >&2; exit 1; }
    while read -r _name _entry _decrypt _restore; do
        if [[ "$_name" == "@main_exe" ]]; then MAIN_RESTORE=$_entry; continue; fi
        WAD_ENTRY[$_name]=$_entry
        DECRYPT[$_name]=${_decrypt:-0}
        RESTORE[$_name]=${_restore:--}
    done <<< "$MAP_LINES"
fi

# --- main EXE -------------------------------------------------------------
emit_funcs main_exe "$OUT/main_exe_extra_funcs.txt"
MAIN_SHA=$(sha "$EXE")

# --- overlays: registry parsed from the converged overlay table ----------
# spyro_overlay_table.c rows: `    {  21, "level_10", psx_dispatch_level_10 },`
mapfile -t ROWS < <(grep -oP '\{\s*\d+,\s*"[^"]+"' "$REGISTRY_SRC" \
                    | sed -E 's/\{\s*([0-9]+),\s*"([^"]+)"/\1 \2/')

{
    echo "# AUTO-GENERATED by scripts/emit-recomp-manifest.sh - do not edit."
    echo "# Converged recomp inputs for a single-pass per-binary recompile."
    echo "schema = 1"
    echo
    echo "[main_exe]"
    echo "input = \"PSX.EXE\""
    echo "sha256 = \"$MAIN_SHA\""
    echo "funcs = \"main_exe.funcs.txt\""
    if [[ -n "${CHANNEL_MANIFEST:-}" ]]; then echo "channel_sites = true"
    else echo "channel_sites = false"; fi
    restore_toml "$MAIN_RESTORE"
} > "$MAN/manifest.toml"

# The overlay blob regen translated, by section name (overlay_registry.txt
# rows: `<magic> <name> <ovl path>`).
declare -A OVL_PATH=()
while read -r _magic _name _path; do
    [[ -n "${_name:-}" ]] && OVL_PATH[$_name]=$_path
done < "$REGISTRY_LIST"

count=0
for row in "${ROWS[@]}"; do
    magic="${row%% *}"; name="${row#* }"
    ovl="${OVL_PATH[$name]:-}"
    [[ -f "$ovl" ]] || { echo "[emit-manifest] missing overlay blob for $name" >&2; exit 1; }

    final="$OUT/$name/extra_funcs.combined.txt"
    [[ -f "$final" ]] || final="$OUT/$name/extra_funcs.second.txt"
    [[ -f "$final" ]] || { echo "[emit-manifest] no converged list for $name" >&2; exit 1; }
    emit_funcs "$name" "$final"

    {
        echo
        echo "[[overlay]]"
        echo "name = \"$name\""
        echo "magic = $magic"
        echo "func_prefix = \"func_${name}_\""
        echo "load_addr = $OVERLAY_LOAD_ADDR"
        echo "wad_file = \"$(basename "$ovl")\""
        if [[ "${FIRST_BOOT_CARVE:-}" == "wad_entry" ]]; then
            [[ -n "${WAD_ENTRY[$name]:-}" ]] || { echo "[emit-manifest] no WAD entry holds $name" >&2; exit 1; }
            echo "wad_entry = ${WAD_ENTRY[$name]}"
            if [[ "${DECRYPT[$name]:-0}" == "1" ]]; then echo "decrypt = true"; fi
            restore_toml "${RESTORE[$name]:--}"
        fi
        echo "sha256 = \"$(sha "$ovl")\""
        echo "funcs = \"$name.funcs.txt\""
    } >> "$MAN/manifest.toml"
    count=$((count + 1))
done

echo "[emit-manifest] emitted main_exe + $count overlays"

# --- compare with the committed snapshot ----------------------------------
if [[ ! -d "$COMMITTED" ]]; then
    mkdir -p "$COMMITTED"
fi
if diff -rq "$MAN" "$COMMITTED" >/dev/null; then
    echo "[emit-manifest] $COMMITTED matches the regen"
    exit 0
fi
echo "[emit-manifest] $COMMITTED differs from what this regen converged to:"
diff -rq "$MAN" "$COMMITTED" | sed 's/^/  /' || true
for f in "$MAN"/*.funcs.txt; do
    c="$COMMITTED/$(basename "$f")"
    [[ -f "$c" ]] || continue
    added=$(comm -23 "$f" "$c" | wc -l); removed=$(comm -13 "$f" "$c" | wc -l)
    if (( added + removed )); then echo "  $(basename "$f"): +$added -$removed"; fi
done
if [[ "${OPENPETE_MANIFEST_BLESS:-0}" != "0" ]]; then
    rm -rf "$COMMITTED"
    cp -a "$MAN" "$COMMITTED"
    echo "[emit-manifest] BLESSED: $COMMITTED rewritten; commit it with the change that moved the inputs"
    exit 0
fi
echo "[emit-manifest] FAIL: the committed snapshot is stale. Re-run with" \
     "OPENPETE_MANIFEST_BLESS=1 and commit $COMMITTED alongside the input change." >&2
exit 1
