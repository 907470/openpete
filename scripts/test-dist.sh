#!/usr/bin/env bash
# test-dist.sh - build and package a test distribution of this checkout.
#
#   scripts/test-dist.sh [windows|linux|all] [--container] [--out DIR]
#                        [--toolchain-windows DIR] [--no-fetch] [--preflight-only]
#
# One command from a checkout (main or a linked worktree) to the archives a
# tester runs: preflight, configure and build the *-launcher presets, then
# scripts/package-dist.sh with the flags a release passes (--with-mod-sdk,
# the bundled toolchain of each platform, --appimage on Linux). The default
# target is all. Packaging does not run the game; every OPENPETE_* name here
# is this script's or the build's own, never a Windows child's -
# wslenv-guard: no-launch.
#
# Preflight runs before any build and reports every failure at once, each
# with its fix:
#   - cmake, ninja and python3 (tomllib or tomli) on PATH;
#   - mkdocs and mkdocs-material importable at the versions the release image
#     pins (packaging/linux-release.Dockerfile), since every package ships
#     the docs site the `docs` target builds;
#   - each game's untracked build inputs (games/<g>/generated, the decomp's
#     build directory; a worktree links them from the main checkout);
#   - windows: the mingw-w64 cross gcc, windres, strip and dlltool on PATH,
#     and the llvm-mingw tree the zip bundles (--toolchain-windows, default
#     <spyro>/toolchains/llvm-mingw-20260616-ucrt-x86_64 beside the main
#     checkout);
#   - linux: .tools/clang and appimagetool with its runtime current, by the
#     setup scripts' own --check; a stale or missing tool is fetched by its
#     setup script unless --no-fetch, which fails instead. Without
#     --container, also third_party/psxrecomp/build/PSXRecomp (the host
#     recompiler the tarball ships) and the host readelf/ldd the AppImage
#     closure uses;
#   - --container: a container engine (scripts/lib/container.sh);
#   - for each bundled toolchain, the licence staging
#     (scripts/dist_compliance.py licences) package-dist.sh runs after the
#     build, into a scratch directory.
# .tools/ is the main checkout's (scripts/lib/tools_dir.sh), so a fresh
# worktree needs no copy of its own. --preflight-only stops after it.
#
# The version is OPENPETE_DIST_VERSION when set, else the short HEAD with
# -dirty when tracked files differ; -test is appended unless it already ends
# so. It names every archive (openpete-<ver>-windows-x86_64.zip,
# openpete-<ver>-linux-x86_64.tar.xz, OpenPete-<ver>-x86_64.AppImage) and is
# the version line the executable shows. The configure passes it with -D,
# so a build directory configured under another version takes the new one.
#
# --container builds and packages Linux in the release container
# (scripts/release-container.sh --test): the Debian 11 glibc floor, a build
# from empty directories, the surface-baseline compare of each shipped game
# and the bundled sysroot header check, as a release runs them. The test
# mode skips the release's tag, HEAD-is-tag and clean-tree refusals, and
# reports a surface difference without refusing.
# Packages land under build/test-dist-container/ (the container sees only
# the tree) and are moved to --out. Without --container the Linux package
# is a dev-box build, whose glibc floor is the dev box's.
#
# A test package is not a release: create-release.sh additionally runs the
# docs build of every section, the release-commit and docs-coverage checks,
# the leak scan, signing, and the Windows first-boot e2e with the oracle.
#
# Exit: 0 every requested archive written; 1 preflight or a step failed
# (named on stderr); 64 usage.
set -euo pipefail

# Packaging runs the built exe (--compile-mods, the surface dump); no player
# holds the pad (scripts/fences/rumble_guard.py).
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."
ROOT=$PWD
# shellcheck source=scripts/lib/tools_dir.sh
source scripts/lib/tools_dir.sh
# shellcheck source=scripts/lib/mingw.sh
source scripts/lib/mingw.sh
TOOLS=$(openpete_tools_dir .)

usage() { sed -n '3,5p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 64; }

TARGET=all; CONTAINER=0; OUT=dist; FETCH=1; PREFLIGHT_ONLY=0
TCW="$(mingw_spyro_root "$ROOT")/toolchains/llvm-mingw-20260616-ucrt-x86_64"
while [[ $# -gt 0 ]]; do
    case "$1" in
        windows|linux|all) TARGET=$1; shift ;;
        --container) CONTAINER=1; shift ;;
        --out) OUT="$2"; shift 2 ;;
        --toolchain-windows) TCW="$2"; shift 2 ;;
        --no-fetch) FETCH=0; shift ;;
        --preflight-only) PREFLIGHT_ONLY=1; shift ;;
        -h|--help) usage ;;
        *) echo "[test-dist] unknown argument: $1" >&2; usage ;;
    esac
done
WIN=0; LIN=0
[[ $TARGET == windows || $TARGET == all ]] && WIN=1
[[ $TARGET == linux || $TARGET == all ]] && LIN=1
if [[ $CONTAINER -eq 1 && $LIN -eq 0 ]]; then
    echo "[test-dist] --container packages Linux; the target is $TARGET" >&2; exit 64
fi
OUT=$(realpath -m "$OUT")
TCW=$(realpath -m "$TCW")

VERSION="${OPENPETE_DIST_VERSION:-}"
if [[ -z "$VERSION" ]]; then
    VERSION=$(git rev-parse --short HEAD)
    git diff --quiet HEAD -- || VERSION+=-dirty
fi
[[ "$VERSION" == *-test ]] || VERSION+=-test
export OPENPETE_DIST_VERSION="$VERSION"

T0=$SECONDS
# phase <name>: prints the phase and the seconds since the script started.
phase() { echo "[test-dist] == $1 (t+$((SECONDS - T0)) s)"; }

# ---- preflight ---------------------------------------------------------------
FAIL=()
# refuse <words...>: records one preflight failure, its fix in the words.
refuse() { FAIL+=("$*"); }
# need <condition command...> -- <fix>: records the fix when the command fails.
need() {
    local args=() fix
    while [[ "$1" != -- ]]; do args+=("$1"); shift; done
    shift; fix="$*"
    "${args[@]}" >/dev/null 2>&1 || FAIL+=("$fix")
}
# pin_of <package>: the version the release image pins for a pip package.
pin_of() {
    grep -oE "(^|[[:space:]])$1==[^[:space:]\\\\]+" packaging/linux-release.Dockerfile \
        | head -1 | sed 's/.*==//'
}
# pyver <distribution>: the installed version of a Python distribution.
pyver() {
    python3 -c 'import sys; from importlib.metadata import version; print(version(sys.argv[1]))' \
        "$1" 2>/dev/null || true
}
# tool_current <setup script>: runs the script's --check; fetches through the
# script when stale and fetching is allowed, else records the fix.
tool_current() {
    local s="$1" log
    log=$(mktemp)
    if "scripts/$s" --check >"$log" 2>&1; then rm -f "${log:?}"; return 0; fi
    sed 's/^/[test-dist]   /' "$log" >&2; rm -f "${log:?}"
    if [[ $FETCH -eq 1 ]]; then
        echo "[test-dist] $s: stale or missing, fetching into $TOOLS"
        "scripts/$s" || refuse "scripts/$s failed (network?); rerun it and read its error"
    else
        refuse "run scripts/$s (or drop --no-fetch): $TOOLS is stale or incomplete"
    fi
}

MODE=$TARGET; [[ $CONTAINER -eq 1 ]] && MODE+=" (linux in the container)"
phase "preflight $MODE, version $VERSION, tools $TOOLS"
need command -v cmake -- "install cmake (>= 3.22)"
need command -v ninja -- "install ninja (apt install ninja-build)"
need bash -c "python3 -c 'import tomllib' || python3 -c 'import tomli'" -- \
    "python3 needs tomllib (3.11+) or tomli (pip install tomli)"
if [[ $WIN -eq 1 || $CONTAINER -eq 0 ]]; then
    want_mk=$(pin_of mkdocs); want_mat=$(pin_of mkdocs-material)
    have_mk=$(pyver mkdocs); have_mat=$(pyver mkdocs-material)
    if [[ "$have_mk" != "$want_mk" || "$have_mat" != "$want_mat" ]] \
       || ! python3 -c 'import mkdocs, material' 2>/dev/null; then
        refuse "pip install mkdocs==$want_mk mkdocs-material==$want_mat" \
               "(have mkdocs '${have_mk:-none}', mkdocs-material '${have_mat:-none}';" \
               "the docs target builds no site without them)"
    fi
fi
# A checked-out submodule at a commit other than the pin the tree records
# (`+` in git submodule status; `U` a merge conflict) would package sources
# the commit does not name. An uninitialised one (`-`, duckstation in a
# worktree) is not read by the build.
while IFS= read -r line; do
    read -r sha path _ <<<"${line:1}"
    case "${line:0:1}" in
        [+U]) refuse "submodule $path is at ${sha:0:9}, not the pin HEAD records:" \
                     "git -C $path checkout --detach" \
                     "$(git ls-tree HEAD "$path" | awk '{print $3}')" ;;
    esac
done < <(git submodule status 2>/dev/null)
for g in games/*/; do
    g=$(basename "$g")
    [[ -f "games/$g/CMakeLists.txt" && -d "games/$g/$g" ]] || continue
    for p in "games/$g/generated" "games/$g/$g/build"; do
        [[ -d "$p" && -n "$(ls -A "$p" 2>/dev/null)" ]] \
            || refuse "$p is missing or empty: in a worktree link it from the main" \
                      "checkout (scripts/worktree.sh inputs lists every input)"
    done
done
if [[ $WIN -eq 1 ]]; then
    for t in gcc-posix g++-posix windres strip dlltool; do
        need command -v "x86_64-w64-mingw32-$t" -- \
            "x86_64-w64-mingw32-$t is not on PATH (apt install mingw-w64)"
    done
    if [[ ! -f "$TCW/bin/x86_64-w64-mingw32-clang.exe" || ! -f "$TCW/LICENSE.TXT" ]]; then
        refuse "no llvm-mingw (ucrt, Windows-hosted) tree at $TCW: unpack" \
               "llvm-mingw-20260616-ucrt-x86_64.zip there or pass --toolchain-windows DIR"
    fi
fi
if [[ $LIN -eq 1 ]]; then
    tool_current setup_linux_clang.sh
    tool_current setup_appimagetool.sh
    if [[ $CONTAINER -eq 1 ]]; then
        # shellcheck source=scripts/lib/container.sh
        source scripts/lib/container.sh
        ( ctr_engine test-dist ) >/dev/null 2>&1 \
            || refuse "no container engine: install podman (or set OPENPETE_CONTAINER_ENGINE)"
    else
        [[ -x third_party/psxrecomp/build/PSXRecomp ]] \
            || refuse "third_party/psxrecomp/build/PSXRecomp missing: cmake -S" \
                      "third_party/psxrecomp/recompiler -B third_party/psxrecomp/build -G Ninja" \
                      "-DCMAKE_BUILD_TYPE=Release && cmake --build third_party/psxrecomp/build"
        need command -v readelf -- "readelf is not on PATH (apt install binutils)"
        need command -v ldd -- "ldd is not on PATH"
    fi
fi
# The licence staging package-dist.sh runs after the build, run now against
# the toolchains that will be bundled: a toolchain tree without its licence
# texts fails here instead of an hour in.
lic=$(mktemp -d)
if [[ $WIN -eq 1 && -d "$TCW" ]]; then
    python3 scripts/dist_compliance.py licences --stage "$lic/w" --scope all \
        --scope windows --scope sdk --scope toolchain-windows --toolchain-dir "$TCW" \
        >/dev/null 2>"$lic/w.err" \
        || refuse "Windows licence staging refused: $(tail -1 "$lic/w.err")"
fi
if [[ $LIN -eq 1 && -d "$TOOLS/clang" ]]; then
    python3 scripts/dist_compliance.py licences --stage "$lic/l" --scope all \
        --scope sdk --scope toolchain-linux --scope appimage \
        --toolchain-dir "$TOOLS/clang" >/dev/null 2>"$lic/l.err" \
        || refuse "Linux licence staging refused: $(tail -1 "$lic/l.err")" \
                  "(rerun scripts/setup_linux_clang.sh)"
fi
rm -rf "${lic:?}"
if [[ ${#FAIL[@]} -gt 0 ]]; then
    echo "[test-dist] preflight FAILED (t+$((SECONDS - T0)) s):" >&2
    for f in "${FAIL[@]}"; do echo "[test-dist]   - $f" >&2; done
    exit 1
fi
phase "preflight passed"
[[ $PREFLIGHT_ONLY -eq 0 ]] || exit 0

# ---- build and package -------------------------------------------------------
# build_preset <preset>: configures under this version and builds every target.
build_preset() {
    phase "configure $1"
    cmake --preset "$1" -DOPENPETE_DIST_VERSION="$VERSION" >/dev/null \
        || { echo "[test-dist] configure $1 failed" >&2; exit 1; }
    phase "build $1"
    cmake --build --preset "$1" \
        || { echo "[test-dist] build $1 failed" >&2; exit 1; }
}

ARTS=()
mkdir -p "$OUT"
if [[ $WIN -eq 1 ]]; then
    build_preset windows-mingw-launcher
    phase "package windows"
    rm -f "${OUT:?}/openpete-$VERSION-windows-x86_64.zip"
    scripts/package-dist.sh windows --out "$OUT" --toolchain "$TCW" --with-mod-sdk
    ARTS+=("$OUT/openpete-$VERSION-windows-x86_64.zip")
fi
if [[ $LIN -eq 1 && $CONTAINER -eq 0 ]]; then
    build_preset linux-release-launcher
    phase "package linux"
    rm -f "${OUT:?}/openpete-$VERSION-linux-x86_64.tar.xz" \
          "${OUT:?}/OpenPete-$VERSION-x86_64.AppImage"
    scripts/package-dist.sh linux --out "$OUT" --toolchain "$TOOLS/clang" \
        --with-mod-sdk --appimage
    ARTS+=("$OUT/openpete-$VERSION-linux-x86_64.tar.xz"
           "$OUT/OpenPete-$VERSION-x86_64.AppImage")
fi
if [[ $CONTAINER -eq 1 ]]; then
    phase "container build and package linux"
    COUT=build/test-dist-container
    rm -rf "${COUT:?}"
    scripts/release-container.sh --test --appimage --out "$COUT"
    for a in "openpete-$VERSION-linux-x86_64.tar.xz" "OpenPete-$VERSION-x86_64.AppImage"; do
        [[ -f "$COUT/$a" ]] || { echo "[test-dist] container wrote no $COUT/$a" >&2; exit 1; }
        if [[ "$(realpath "$COUT")" != "$OUT" ]]; then
            mv -f "$COUT/$a" "$COUT/$a.sha256" "$OUT/"
        fi
        ARTS+=("$OUT/$a")
    done
fi

# ---- report ------------------------------------------------------------------
phase "done"
for a in "${ARTS[@]}"; do
    [[ -f "$a" ]] || { echo "[test-dist] missing archive: $a" >&2; exit 1; }
    printf '[test-dist] %8s  %s\n' "$(du -h "$a" | cut -f1)" "$a"
done
