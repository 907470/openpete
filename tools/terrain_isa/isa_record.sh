#!/usr/bin/env bash
# isa_record.sh - the terrain vertex-stage ISA record: instruction, register,
# spill and scratch counts of every terrain vertex variant on three compilers,
# and the cross-pipeline contraction features of each compile.
#
# Measures, per variant x spec constants (DEPTH_REVERSED, AFFINE_UV) x form:
#   llpc  RGA's offline amdllpc, gfx 11.0.0 / 12.0.0 (RGA 2.15 rejects gfx 10.3.0): the vertex
#         stage compiled with terrain.frag or terrain_key_cache.frag as one
#         pipeline (lto) and alone as a pre-raster library (vsonly).
#         Runs locally (the Windows suite through WSL interop, or the Linux
#         suite natively); llpc_record.py.
#   radv  Mesa RADV's ACO ISA and SHADER STATS on an AMD host
#         (RADV_DEBUG=shaders,shaderstats,nocache): openpete boots Spyro 1
#         headless there with the .spv under test (OPENPETE_SHADER_DIR) and
#         the driver compiles the GPL libraries and LTO links.
#   anv   Mesa ANV's INTEL_DEBUG=vs assembly and SIMD statistics on an Intel
#         host, the same runs, under nice -n 19.
# The radv and anv runs happen over ssh on fleet hosts named in the fleet host
# file (scripts/golden-fleet.py; tools/terrain_isa/fleet_ssh.py resolves a
# name). Each needs <remote-root>/{bin,lib,disc1} (openpete and
# spyro1-recomp.so, the shared libraries openpete links that the host lacks,
# the Spyro 1 disc), provisioned by the `setup` leg.
#
# Inputs:
#   <spv-dir>     the compiled terrain stages: <variant>.vert.spv for each
#                 variant, terrain.frag.spv, terrain_prepass(_cutout).frag.spv,
#                 terrain_key_cache.frag.spv. A build's
#                 build/<preset>/core/rhi/shaders holds them for its revision
#                 (glslangValidator, then the SSA passes of core/rhi/CMakeLists.txt).
#                 The openpete binary on a remote host only needs the pipeline
#                 layouts the .spv declare.
#   <record-dir>  output directory, created
#   legs          any of llpc radv anv (default all three), or setup / teardown
# Options:
#   --rga DIR           RGA install (default $RGA_DIR); llpc leg
#   --work-dir DIR      amdllpc work directory (default: llpc_record.py's)
#   --gfx LIST          llpc gfxips (default 11.0.0,12.0.0)
#   --variants LIST     llpc variants (default llpc_record.py's five)
#   --jobs N            concurrent amdllpc compiles (default 1)
#   --radv-host NAME    fleet host of the radv leg (default steam-machine)
#   --anv-host NAME     fleet host of the anv leg (default ptah)
#   --remote-root DIR   run directory on the remote hosts, relative to the
#                       remote home (default openpete-terrain-isa)
#   --arms LIST         mesa_capture.sh arms (default "gpl gplaff")
#   --remote-env WORDS  extra KEY=VALUE words for the remote openpete runs
#   --remote-lib DIR    setup: the shared libraries to copy to <root>/lib
#   --build-dir DIR     setup: the build holding openpete (default
#                       build/linux-release)
#   --dry-run           print the commands of the selected legs, run none
#
# Outputs, under <record-dir>:
#   llpc/<variant>__<gfx>__<config>/{p.pipe,p.elf,isa.txt,notes.txt,llpc.log},
#   llpc/llpc.tsv, llpc/calib.tsv, llpc.log
#   radv/<arm>/*.aco.gz + stats.tsv + calib.tsv (radv_split.py, isa_calib.py)
#   anv/<arm>/*.asm.gz + stats.tsv + calib.tsv (anv_split.py, isa_calib.py)
#   isa_table.tsv  one row per variant x DR x AU x compiler x form x frag
#                  (isa_table.py)
#   calib_pairs.tsv  SAME/DIFF verdicts of the contraction pairs
#                  (calib_pairs.py)
# Two records are compared with isa_compare.py (compare.tsv, summary.tsv).
# A failed or empty amdllpc row is rerun with llpc_redo.py.
#
# Run from the openpete checkout (setup copies build and disc files by their
# repository paths). Exit 0 when every selected leg completed, 1 when one
# failed, 64 on bad usage.
set -u
# This script starts no openpete locally: setup copies the binary and the
# remote runs are mesa_capture.sh's (headless, --vibration 0).
# input-lock-guard: no-launch
export OPENPETE_VIBRATION=0
HERE=$(cd "$(dirname "$0")" && pwd)

RGA="${RGA_DIR:-}" WORK="" GFX="" VARIANTS="" JOBS=1
RADV_HOST=steam-machine ANV_HOST=ptah RROOT=openpete-terrain-isa
ARMS="gpl gplaff" XENV="" RLIB="" BUILD=build/linux-release DRY=0
pos=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rga|--work-dir|--gfx|--variants|--jobs|--radv-host|--anv-host|--remote-root|--arms|--remote-env|--remote-lib|--build-dir)
            [[ $# -ge 2 ]] || { echo "isa_record: $1 needs a value" >&2; exit 64; }
            case "$1" in
                --rga) RGA=$2;; --work-dir) WORK=$2;; --gfx) GFX=$2;; --variants) VARIANTS=$2;;
                --jobs) JOBS=$2;; --radv-host) RADV_HOST=$2;; --anv-host) ANV_HOST=$2;;
                --remote-root) RROOT=$2;; --arms) ARMS=$2;; --remote-env) XENV=$2;;
                --remote-lib) RLIB=$2;; --build-dir) BUILD=$2;;
            esac
            shift 2;;
        --dry-run) DRY=1; shift;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 64;;
        --*) echo "isa_record: unknown option $1" >&2; exit 64;;
        *) pos+=("$1"); shift;;
    esac
done
[[ ${#pos[@]} -ge 2 ]] || { echo "usage: isa_record.sh [options] <spv-dir> <record-dir> [legs...]" >&2; exit 64; }
SPV=${pos[0]} REC=${pos[1]}
LEGS=("${pos[@]:2}")
[[ ${#LEGS[@]} -gt 0 ]] || LEGS=(llpc radv anv)
for leg in "${LEGS[@]}"; do
    case "$leg" in llpc|radv|anv|setup|teardown) ;; *) echo "isa_record: unknown leg $leg" >&2; exit 64;; esac
done
[[ "$RROOT" != /* && "$RROOT" != *..* && -n "$RROOT" ]] ||
    { echo "isa_record: --remote-root must be a relative path under the remote home" >&2; exit 64; }
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "isa_record: --jobs expects a positive integer" >&2; exit 64; }

# x CMD...: run, or print under --dry-run.
x() {
    if (( DRY )); then printf '+'; printf ' %q' "$@"; printf '\n'; return 0; fi
    "$@"
}

needs_spv() {
    [[ -d "$SPV" ]] || { echo "isa_record: no spv directory $SPV" >&2; exit 64; }
    ls "$SPV"/*.vert.spv >/dev/null 2>&1 || { echo "isa_record: no .vert.spv in $SPV" >&2; exit 64; }
    SPV=$(cd "$SPV" && pwd)
}

# host_of NAME: the ssh target of fleet host NAME.
host_of() {
    local t
    t=$(python3 "$HERE/fleet_ssh.py" "$1") || exit 1
    echo "$t"
}

if (( ! DRY )); then mkdir -p "$REC"; REC=$(cd "$REC" && pwd); fi
tag=$(basename "$REC")
fail=0

# remote LEG HOST DBG NICE: one capture on a Mesa host, split there, copied back.
remote() {
    local leg=$1 host=$2 dbg=$3 nice=$4 split=radv_split.py
    [[ "$leg" == anv ]] && split=anv_split.py
    local r="$RROOT"
    x ssh "$host" "mkdir -p $r/spv-$tag" || return 1
    x scp -q "$SPV"/terrain*.spv "$host:$r/spv-$tag/" || return 1
    x scp -q "$HERE/mesa_capture.sh" "$HERE/radv_split.py" "$HERE/anv_split.py" "$host:$r/" || return 1
    x ssh "$host" "cd $r && DRVDBG='$dbg $XENV' NICE='$nice' ARMS='$ARMS' bash mesa_capture.sh \"\$HOME/$r\" \"\$HOME/$r/spv-$tag\" \"\$HOME/$r/cap-$tag\" && grep -a -o 'vs=terrain[a-z0-9_]*' cap-$tag/drv-*.err | sort | uniq -c && for a in $ARMS; do python3 $split cap-$tag/drv-\$a.err split-$tag/\$a || exit 1; done" || return 1
    x mkdir -p "$REC/$leg"
    local a
    for a in $ARMS; do x scp -q -r "$host:$r/split-$tag/$a" "$REC/$leg/" || return 1; done
    # The raw dumps are hundreds of MB; the split keeps the per-pipeline programs.
    x ssh "$host" "rm -rf -- \"${r:?}/cap-${tag:?}\" \"${r:?}/split-${tag:?}\" \"${r:?}/spv-${tag:?}\""
}

# calib LEG: the contraction features of each arm's vertex programs.
calib() {
    local leg=$1 a pat='*.aco.gz'
    [[ "$leg" == anv ]] && pat='*.asm.gz'
    for a in $ARMS; do
        local -a files=()
        if (( DRY )); then files=("$REC/$leg/$a/$pat"); else
            mapfile -t files < <(find "$REC/$leg/$a" -maxdepth 1 -name "$pat" \
                                   \( -name '*_vs_*' -o -name '*.asm.gz' \) | sort)
        fi
        if (( DRY )); then echo "+ python3 $HERE/isa_calib.py ${files[*]} > $REC/$leg/$a/calib.tsv"; continue; fi
        (( ${#files[@]} )) || { echo "isa_record: $leg/$a holds no vertex program" >&2; return 1; }
        python3 "$HERE/isa_calib.py" "${files[@]}" > "$REC/$leg/$a/calib.tsv" || return 1
    done
}

for leg in "${LEGS[@]}"; do
    case $leg in
        setup)
            [[ -n "$RLIB" && -d "$RLIB" ]] || { echo "isa_record: setup needs --remote-lib DIR" >&2; exit 64; }
            for name in "$RADV_HOST" "$ANV_HOST"; do
                h=$(host_of "$name")
                x ssh "$h" "mkdir -p $RROOT/bin $RROOT/lib $RROOT/disc1" &&
                x scp -q "$BUILD/openpete" "$BUILD/spyro1-recomp.so" "$h:$RROOT/bin/" &&
                x scp -q "$RLIB"/* "$h:$RROOT/lib/" &&
                x scp -q games/spyro-1/spyro-1/build/SCUS-94228.cue \
                         games/spyro-1/spyro-1/build/SCUS-94228.bin "$h:$RROOT/disc1/" ||
                    { echo "isa_record: setup of $name failed" >&2; fail=1; }
            done;;
        teardown)
            for name in "$RADV_HOST" "$ANV_HOST"; do
                h=$(host_of "$name")
                x ssh "$h" "rm -rf -- \"${RROOT:?}\"" || fail=1
            done;;
        llpc)
            needs_spv
            [[ -n "$RGA" ]] || { echo "isa_record: the llpc leg needs --rga DIR or RGA_DIR" >&2; exit 64; }
            args=("$SPV" "$REC/llpc" --rga "$RGA" --jobs "$JOBS")
            [[ -n "$WORK" ]] && args+=(--work-dir "$WORK")
            [[ -n "$GFX" ]] && args+=(--gfx "$GFX")
            [[ -n "$VARIANTS" ]] && args+=(--variants "$VARIANTS")
            if (( DRY )); then
                echo "+ python3 $HERE/llpc_record.py ${args[*]} > $REC/llpc.log 2>&1"
                echo "+ python3 $HERE/isa_calib.py $REC/llpc/*/isa.txt > $REC/llpc/calib.tsv"
            else
                python3 "$HERE/llpc_record.py" "${args[@]}" > "$REC/llpc.log" 2>&1 ||
                    { echo "isa_record: llpc_record.py failed (see $REC/llpc.log)" >&2; fail=1; continue; }
                python3 "$HERE/isa_calib.py" "$REC"/llpc/*/isa.txt > "$REC/llpc/calib.tsv" || fail=1
            fi;;
        radv)
            needs_spv
            h=$(host_of "$RADV_HOST")
            { remote radv "$h" "RADV_DEBUG=shaders,shaderstats,nocache" "" && calib radv; } || fail=1;;
        anv)
            needs_spv
            h=$(host_of "$ANV_HOST")
            { remote anv "$h" "INTEL_DEBUG=vs" "nice -n 19" && calib anv; } || fail=1;;
    esac
done

record=0
for leg in "${LEGS[@]}"; do [[ "$leg" == llpc || "$leg" == radv || "$leg" == anv ]] && record=1; done
if (( record )); then
    if (( DRY )); then
        echo "+ python3 $HERE/isa_table.py $REC"
        echo "+ python3 $HERE/calib_pairs.py $REC > $REC/calib_pairs.tsv"
    else
        python3 "$HERE/isa_table.py" "$REC" > /dev/null || fail=1
        python3 "$HERE/calib_pairs.py" "$REC" > "$REC/calib_pairs.tsv" || fail=1
        echo "wrote $REC/isa_table.tsv $REC/calib_pairs.tsv"
    fi
fi
exit "$fail"
