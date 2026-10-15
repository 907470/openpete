#!/usr/bin/env bash
# package-dist.sh - assemble end-user distribution archives.
#
# Ships ONLY our own code + tools + facts-metadata. No game bytes: no
# recomp module (users first-boot-compile it), no disc data, ROM-free
# icon (the dist presets pin OPENPETE_ROM_FREE_ICON=ON). The knob is a CMake
# option, never a child's environment - wslenv-guard: no-launch.
#
#   scripts/package-dist.sh linux   [--out DIR] [--toolchain DIR] [--appimage]
#   scripts/package-dist.sh windows [--out DIR] [--toolchain DIR]
#   scripts/package-dist.sh {linux|windows} --dry-run
#
# One archive, openpete-<ver>-<platform>-x86_64, carrying the one
# executable (`openpete`, `openpete.exe`) from the *-launcher build and the
# first-boot inputs (manifests) and SDK trees of every game
# games/shipped.toml lists. A discless launch of the one executable shows
# the launcher over every game's library entries and runs the game of the
# entry it boots or the disc dropped on it. --dry-run prints the games, the
# build input, and each archive with the executable's path inside it, and
# exits without packaging anything.
#
# --appimage additionally emits OpenPete-<ver>-x86_64.AppImage from the
# SAME staged tree the tarball is made from, so the two artifacts can
# never drift. The AppImage is the primary Linux download: its squashfs
# keeps the bundled compiler compressed and never unpacks it, and its
# read-only mount is what forces the writable-state split (the library,
# mods, user assets and recordings resolve through openpete_paths).
#
# --with-mod-sdk additionally stages the mod SDK headers the boot-time
# mod compiler needs (plus, where mods live next to the exe, a mods/ drop
# folder). Releases ship without it; the dist gate passes it to keep that
# path exercised.
#
# Inputs: the corresponding *-launcher preset build (linux-release-launcher /
# windows-mingw-launcher) with its staged recomp/ directory: the one
# executable `openpete` (`openpete.exe`) at the tree root, staged into the
# archive under the same name, with the files beside it. --toolchain
# points at an UNPACKED compiler tree to bundle: llvm-mingw (ucrt
# x86_64) for Windows, a trimmed llvm.org clang for Linux
# (scripts/setup_linux_clang.sh produces one in .tools/clang). Without
# it the Windows zip and the Linux tarball fall back to the user's own
# compiler (documented in README-dist); the AppImage always bundles one,
# since needing nothing installed is the point of it.
set -euo pipefail

# Staging runs the built exe (--compile-mods) to fill each mod's .build, and
# vibration ships on by default: no player uses a pad during packaging, and
# the buzzing shows up in no log, so it would never surface on its own.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/.."

PLATFORM="${1:-}"
shift || true
OUT=dist
TOOLCHAIN=""
WITH_MOD_SDK=0
APPIMAGE=0
DRY_RUN=0
# --sign '<command>' - Authenticode hook, Windows platform only. Invoked
# once per first-party PE (openpete.exe, PSXRecomp.exe) with the
# file path appended; a nonzero exit fails the package. Evaluated per
# file so embedded $(...) (e.g. a fresh az access token) re-runs each
# time. The bundled llvm-mingw binaries are NOT signed (SmartScreen
# only screens the shell-executed download; ~400 upstream tools would
# burn signatures attesting binaries we didn't build). jsign example:
#   --sign 'jsign --storetype TRUSTEDSIGNING
#           --keystore eus.codesigning.azure.net
#           --storepass "$(az account get-access-token
#               --resource https://codesigning.azure.net
#               --query accessToken -o tsv)"
#           --alias <account>/<profile>
#           --tsaurl http://timestamp.acs.microsoft.com'
# The RFC3161 timestamp is NOT optional with Artifact Signing: its
# certs live ~3 days; only the countersignature keeps releases valid.
SIGN_CMD=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --toolchain) TOOLCHAIN="$2"; shift 2 ;;
        --sign) SIGN_CMD="$2"; shift 2 ;;
        --with-mod-sdk) WITH_MOD_SDK=1; shift ;;
        --appimage) APPIMAGE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done

# Licence fences over the staged tree (scripts/dist_compliance.py):
# assert_no_copyleft refuses a shipped ELF/PE carrying pcsx-redux's GTE or
# a static FFmpeg/x264; stage_licences copies the upstream licence text of
# every NOTICE.md component the archive carries into <stage>/licences/ and
# refuses a component with no text. Both run before the archive is written.
assert_no_copyleft() {  # assert_no_copyleft <path>...
    python3 scripts/dist_compliance.py scan "$@" \
        || { echo "[package] licence fence refused the staged binaries" >&2; exit 1; }
}
stage_licences() {  # stage_licences <stage> <dist_compliance.py licences args>...
    local S="$1"; shift
    python3 scripts/dist_compliance.py licences --stage "$S" "$@" \
        || { echo "[package] licence texts incomplete" >&2; exit 1; }
}

sign_pe() {  # sign_pe <file> - no-op unless --sign was given
    [[ -n "$SIGN_CMD" ]] || return 0
    echo "[package] signing $(basename "$1")"
    eval "$SIGN_CMD \"\$1\"" || { echo "[package] signing FAILED: $1" >&2; exit 1; }
}

# OPENPETE_DIST_VERSION overrides the derived name, and the CMake configure
# reads the same variable for the version the build SHOWS on screen - export
# it before configuring as well as before packaging, or the artifact's name
# and its version line disagree.
# OPENPETE_DIST_VERSION overrides the derived name. Test builds handed to
# testers want the commit alone (`git rev-parse --short HEAD`) so a report
# names the build without implying a release; leave it unset for releases,
# where the tag is the point.
VERSION="${OPENPETE_DIST_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
# The version is the artifact's name, which is the one thing about a build a
# stranger reads before running it. A describe that fell through to a commit
# suffix means this checkout cannot see the tag - the tag lives on another
# machine, or was never made - and the artifact would carry the PREVIOUS
# release's number. Name that here rather than let it surface as a filename
# nobody notices until it is published.
case "$VERSION" in
    *-g[0-9a-f]*|dev|*-dirty)
        echo "[package] note: version '$VERSION' is not a clean tag." >&2
        echo "[package]   a release artifact wants OPENPETE_DIST_VERSION=vX.Y.Z," \
             "or a checkout that can see the tag (git fetch --tags)." >&2
        ;;
esac
case "$PLATFORM" in
linux|windows) ;;
*)
    echo "usage: scripts/package-dist.sh {linux|windows} [--out DIR]" \
         "[--toolchain DIR] [--with-mod-sdk] [--appimage (linux)] [--dry-run]" >&2
    exit 64
    ;;
esac

# The shipped games: games/shipped.toml, the list the User docs and the
# surface audience are computed for.
mapfile -t GAMES < <(python3 -c '
import sys
try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib
for g in tomllib.load(open(sys.argv[1], "rb"))["games"]:
    print(g)
' games/shipped.toml)
[[ ${#GAMES[@]} -gt 0 ]] || { echo "[package] games/shipped.toml lists no game" >&2; exit 1; }

# shipped <game>: whether games/shipped.toml lists the game.
shipped() {
    local g
    for g in "${GAMES[@]}"; do [[ "$g" == "$1" ]] && return 0; done
    return 1
}

# Absolute: the example-staging loop below runs inside `cd mods/<id>`
# subshells, where a relative --out no longer resolves.
OUT=$(realpath -m "$OUT")

stage_common() {  # stage_common <exe-dir>
    local B="$1"
    mkdir -p "$STAGE/recomp/manifest"
    local g
    for g in "${GAMES[@]}"; do
        [[ -d "$B/recomp/manifest/$g" ]] \
            || { echo "[package-dist] $B/recomp/manifest/$g missing: shipped" \
                      "game $g has no first-boot inputs in this build (it is" \
                      "not linked, or its openpete_add_game names no" \
                      "FIRST_BOOT_SOURCES), so the archive could not build its" \
                      "module from the player's disc" >&2; exit 1; }
        cp -a "$B/recomp/manifest/$g" "$STAGE/recomp/manifest/"
    done
    cp -a "$B/recomp/src"      "$STAGE/recomp/"
    # The kit is compiled on the user's machine, so it ships as readable
    # C - but its comments are engineering notes about the host tree.
    scripts/strip-comments.py "$STAGE"/recomp/src/*
    if [[ $WITH_MOD_SDK -eq 1 ]]; then
        # Mod SDK: mods ship mod.toml +
        # src/ and compile at boot against <exe>/sdk with the bundled
        # toolchain. The SDK is a handful of headers, and read-only -
        # unlike mods/ it belongs in the install. The game-neutral headers
        # sit at sdk/ root and each packaged game's generated headers
        # (symbols, wrappers, structs, openpete_game.h) under
        # sdk/games/<game>/; the compile line puts -I sdk/games/<game>
        # before -I sdk, so an archive whose SDK lacks a shipped game's
        # tree compiles no mod for it and is refused here. The root's
        # dispatchers of the per-game headers stay: on a line with -I sdk
        # alone they include a packaged game's tree, or stop with #error
        # naming a game whose tree this archive does not carry.
        cp -a "$B/sdk" "$STAGE/sdk"
        # The one executable's SDK holds every built game's tree; the
        # archive carries the shipped games'.
        for g in "$STAGE"/sdk/games/*/; do
            shipped "$(basename "$g")" || rm -rf "${g:?}"
        done
        for g in "${GAMES[@]}"; do
            [[ -f "$STAGE/sdk/games/$g/openpete_game.h" ]] \
                || { echo "[package-dist] $B/sdk has no games/$g/openpete_game.h:" \
                          "build the $g sdk stage target" >&2; exit 1; }
        done
        # The example mods named in mods/examples.txt ride with the SDK,
        # so a zip holder has working mods without the repository; the
        # Modding section of docs/ (below) is their reference. Examples
        # live under sdk/, not mods/: a mod dropped into mods/ is live on
        # the next boot, and an example is copied there on purpose.
        # An example whose manifest selects no packaged game (the loader's
        # rule, tools/gen_mod_docs.py mod_games) would be dropped at
        # discovery on every shipped game, so it stays out of the SDK.
        mkdir -p "$STAGE/sdk/examples"
        while read -r ex; do
            [[ -z "$ex" || "$ex" == \#* ]] && continue
            [[ -f "mods/$ex/mod.toml" ]] \
                || { echo "[package-dist] mods/examples.txt names $ex but" \
                          "mods/$ex/mod.toml does not exist" >&2; exit 1; }
            # Exit 3 = not selected; any other failure fails the package.
            local sel=0
            python3 -c '
import sys
sys.path.insert(0, "tools")
from gen_mod_docs import mod_games
games = mod_games(open(sys.argv[1], encoding="utf-8").read())
sys.exit(0 if any(g in games for g in sys.argv[2:]) else 3)
' "mods/$ex/mod.toml" "${GAMES[@]}" || sel=$?
            if [[ $sel -eq 3 ]]; then
                echo "[package-dist] example $ex selects no shipped game; not staged"
                continue
            fi
            [[ $sel -eq 0 ]] \
                || { echo "[package-dist] cannot read the games of mods/$ex/mod.toml" >&2; exit 1; }
            # Leaves build products (.build/) and the CI harness file
            # (modtest.toml) behind; config.toml is the mod's shipped
            # knob defaults and stays.
            mkdir -p "$STAGE/sdk/examples/$ex"
            # xargs, not find -exec: a failed cp then fails the package
            # instead of leaving an empty example directory behind.
            (cd "mods/$ex" && find . -type f \
                ! -path './.build/*' ! -name modtest.toml -print0 \
                | xargs -0 cp --parents -t "$STAGE/sdk/examples/$ex/")
            [[ -f "$STAGE/sdk/examples/$ex/mod.toml" ]] \
                || { echo "[package-dist] $ex staged without mod.toml" >&2; exit 1; }
        done < mods/examples.txt
        # The drop-in mods/ folder only belongs here where mods actually
        # resolve next to the exe. Under the per-user layout (POSIX
        # default, and the only option for a read-only AppImage) the
        # runtime looks in the data directory instead, and a placeholder
        # shipped here would be a directory nobody's mods are read from -
        # worse, it would be mistaken for the real one. README-dist names
        # the per-user path for those platforms.
        if [[ "$PLATFORM" == windows ]]; then
            mkdir -p "$STAGE/mods"
            printf '%s\n' \
                "Drop mods here: mods/<id>/ (or mods/<id>.zip) containing" \
                "mod.toml, plus src/*.c (compiled automatically at boot) and/or" \
                "assets/. mods/enabled.txt (one id per line) fixes the load" \
                "order; without it every mod loads, alphabetically." \
                > "$STAGE/mods/README.txt"
        fi
    fi
    # The documentation site (the `docs` CMake target) beside the exe:
    # User always, Modding with the SDK. The target builds both sets, so a
    # missing one means mkdocs was absent at build time.
    local site="$DOCS/docs-site-user"
    [[ $WITH_MOD_SDK -eq 1 ]] && site="$DOCS/docs-site"
    [[ -f "$site/index.html" ]] \
        || { echo "[package-dist] $site/index.html missing: install" \
                  "mkdocs-material and build the docs target" >&2; exit 1; }
    cp -a "$site" "$STAGE/docs"
    cp gamecontrollerdb.txt "$STAGE/" 2>/dev/null || true
    cp LICENSE NOTICE.md "$STAGE/"
    local scopes=(--scope all)
    [[ "$PLATFORM" == windows ]] && scopes+=(--scope windows)
    [[ $WITH_MOD_SDK -eq 1 ]] && scopes+=(--scope sdk)
    stage_licences "$STAGE" "${scopes[@]}"
    # Preconfigured settings, so a first run looks the way we intend
    # rather than falling back to bare defaults. Both in-game menus
    # rewrite this file, so anything here is only a starting point.
    cp packaging/openpete-dist.toml "$STAGE/openpete.toml"
    # The README template carries both platforms' wording; keep only the
    # blocks for the one being packaged.
    awk -v want="@$PLATFORM" '
        /^<!-- @(windows|linux) -->$/ { skip = ($2 != want); next }
        /^<!-- @end -->$/             { skip = 0; next }
        !skip
    ' packaging/README-dist.md.in \
        | sed -e "s/@VERSION@/$VERSION/g" -e "s/@PLATFORM@/$PLATFORM/g" \
        > "$STAGE/README.md"
}

# Libraries an AppImage must NEVER carry: graphics, display-server and
# audio-device clients whose protocol is versioned against a host driver
# or daemon. A bundled copy shadowing the host's breaks on exactly the
# machines the image is meant to run on. Asserted after staging rather
# than trusted.
#
# sndio is deliberately absent from this list. It is a socket client to
# sndiod, not a device driver, and libopenal.so.1 carries a DT_NEEDED on
# it: a package manager installs the two together, but an AppImage
# carries OpenAL past the package manager, so an image without libsndio
# aborts in ld.so on any host that lacks it -- whatever its audio stack.
APPIMAGE_FORBIDDEN='^lib(GL|GLX|GLdispatch|EGL|GLESv2|vulkan|X11|Xext|Xrandr|Xi|xcb|wayland|drm|gbm|asound|pulse|pulsecommon|jack)'

# The C++ and C runtimes always come from the host: bundling them is what
# the glibc floor exists to prevent, and both are backward compatible, so
# a host copy at or above the floor satisfies a binary built against it.
# The floor assertion in check_dist_first_boot.sh is what guarantees that,
# not the closure check below.
APPIMAGE_HOST_CORE='^(ld-linux[^/]*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libstdc\+\+|libgcc_s)\.so'

# Every bundled library ships its licence. The map is the authority: a
# library the closure pulls in with no entry here fails packaging, so a
# notice can never be silently omitted for a dependency nobody chose.
#
# Keyed on the library name with every version suffix stripped, because
# the SONAME is distro-specific -- Debian 11 ships sndio as
# libsndio.so.7.0 where later releases ship libsndio.so.7, and the licence
# does not change with the soname.
declare -A APPIMAGE_LIB_LICENCE=(
    [libopenal]=third_party/OpenAL-mingw/COPYING
    [libsndio]=packaging/appimage/licences/libsndio.txt
    [libbsd]=packaging/appimage/licences/libbsd.txt
    [libmd]=packaging/appimage/licences/libmd.txt
)

# DT_NEEDED name and resolved path of every direct dependency of a file.
elf_needed() { readelf -d "$1" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p'; }
elf_soname() { readelf -d "$1" | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p'; }

# Stage a library under its SONAME (the name ld.so will search for, not
# the versioned filename) and recurse into its own DT_NEEDED, so a
# dependency-of-a-dependency cannot be forgotten. Host-core and forbidden
# names are left for the host to resolve.
bundle_lib() {  # bundle_lib <appdir> <library-path>
    local APPDIR="$1" src="$2" so n path
    so=$(elf_soname "$src"); [[ -n "$so" ]] || so=$(basename "$src")
    if [[ -e "$APPDIR/usr/lib/$so" ]]; then return 0; fi
    cp -L "$src" "$APPDIR/usr/lib/$so"
    while read -r n; do
        [[ -n "$n" ]] || continue
        if [[ "$n" =~ $APPIMAGE_HOST_CORE || "$n" =~ $APPIMAGE_FORBIDDEN ]]; then
            continue
        fi
        if [[ -e "$APPDIR/usr/lib/$n" ]]; then continue; fi
        path=$(ldd "$src" | awk -v n="$n" '$1==n {print $3}')
        [[ -n "$path" && -f "$path" ]] || {
            echo "[package] cannot resolve $n, needed by $so" >&2; exit 1; }
        bundle_lib "$APPDIR" "$path"
    done < <(elf_needed "$src")
}

# Every DT_NEEDED of the payload resolves inside the AppDir or is a name
# the host is required to provide. This is the assertion that catches an
# omitted dependency; the forbidden-list check below only catches an
# included one, which is why it could never have caught libsndio.
assert_appdir_closure() {  # assert_appdir_closure <appdir> <file>...
    local APPDIR="$1"; shift
    local f n miss=""
    for f in "$@"; do
        while read -r n; do
            [[ -n "$n" ]] || continue
            if [[ "$n" =~ $APPIMAGE_HOST_CORE || "$n" =~ $APPIMAGE_FORBIDDEN ]]; then
                continue
            fi
            if [[ -e "$APPDIR/usr/lib/$n" ]]; then continue; fi
            miss+=" $n(<-$(basename "$f"))"
        done < <(elf_needed "$f")
    done
    [[ -z "$miss" ]] || {
        echo "[package] unbundled dependencies:$miss" >&2; exit 1; }
}

build_appimage() {  # build_appimage <stage-dir>
    local S="$1"
    local TOOL="${OPENPETE_APPIMAGETOOL:-.tools/appimagetool-x86_64.AppImage}"
    [[ -x "$TOOL" ]] || {
        echo "[package] --appimage needs appimagetool: run" \
             "scripts/setup_appimagetool.sh" >&2; exit 64; }

    local APPDIR="$OUT/OpenPete.AppDir"
    rm -rf "$APPDIR"
    # The payload keeps the tarball's exact shape one level down, because
    # the runtime resolves recomp/, sdk/ and the shipped assets/ relative
    # to its own executable. usr/ is only where AppImage convention puts
    # a payload; nothing inside it is rearranged.
    mkdir -p "$APPDIR/usr" "$APPDIR/usr/lib"
    cp -a "$S"/. "$APPDIR/usr/"

    # The image must be able to recompile the game on a machine with no
    # development packages at all - that is the reason it exists. Ship a
    # compiler whether or not the tarball stage got one.
    if [[ ! -x "$APPDIR/usr/recomp/tools/clang/bin/clang" ]]; then
        local CC_TREE="${TOOLCHAIN:-.tools/clang}"
        [[ -x "$CC_TREE/bin/clang" ]] || {
            echo "[package] --appimage needs a compiler to bundle: run" \
                 "scripts/setup_linux_clang.sh (or pass --toolchain DIR)" >&2
            exit 64; }
        echo "[package] bundling clang from $CC_TREE"
        mkdir -p "$APPDIR/usr/recomp/tools/clang"
        cp -a "$CC_TREE"/. "$APPDIR/usr/recomp/tools/clang/"
    fi

    # OpenAL Soft is the one runtime dependency that is neither a driver
    # nor universally installed. LGPL: dynamic linking preserved, license
    # text rides along (NOTICE.md), same arrangement as the Windows zip.
    local al
    al=$(ldd "$S/$EXE" | awk '/libopenal\.so/ {print $3}')
    [[ -n "$al" && -f "$al" ]] || { echo "[package] libopenal.so.1 not found" >&2; exit 1; }
    bundle_lib "$APPDIR" "$al"

    mkdir -p "$APPDIR/usr/licences"
    local so lic
    for so in $(ls "$APPDIR/usr/lib"); do
        lic="${APPIMAGE_LIB_LICENCE[${so%%.so*}]:-}"
        [[ -n "$lic" && -f "$lic" ]] || {
            echo "[package] $so is bundled with no licence: add it to" \
                 "APPIMAGE_LIB_LICENCE" >&2; exit 1; }
        # Named for what actually shipped, soname and all.
        cp "$lic" "$APPDIR/usr/licences/$so.txt"
    done
    echo "[package] bundled libs:" $(ls "$APPDIR/usr/lib")
    # The tarball stage's licences/ became usr/licences/ above; add the
    # texts only the image carries (its runtime, its bundled compiler).
    stage_licences "$APPDIR/usr" --scope appimage --scope toolchain-linux \
        --toolchain-dir "$APPDIR/usr/recomp/tools/clang"
    assert_no_copyleft "$APPDIR"

    assert_appdir_closure "$APPDIR" "$APPDIR/usr/$EXE" \
        "$APPDIR"/usr/lib/*.so*

    local bad
    bad=$(ls "$APPDIR/usr/lib" | grep -E "$APPIMAGE_FORBIDDEN" || true)
    [[ -z "$bad" ]] || {
        echo "[package] driver/display libraries must not be bundled: $bad" >&2
        exit 1; }

    install -m755 packaging/appimage/AppRun "$APPDIR/AppRun"
    install -m644 packaging/appimage/com.openpete.OpenPete.desktop \
        "$APPDIR/com.openpete.OpenPete.desktop"
    mkdir -p "$APPDIR/usr/share/applications" \
             "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
             "$APPDIR/usr/share/metainfo"
    cp "$APPDIR/com.openpete.OpenPete.desktop" "$APPDIR/usr/share/applications/"
    # The ROM-free mark (tools/gen_dist_icon.py) - the authentic memcard
    # icon must never ship. Generated here so the image can never pick up
    # a ROM-derived one by accident.
    python3 tools/gen_dist_icon.py --png "$APPDIR/com.openpete.OpenPete.png"
    cp "$APPDIR/com.openpete.OpenPete.png" \
       "$APPDIR/usr/share/icons/hicolor/256x256/apps/"
    sed -e "s/@VERSION@/${VERSION#v}/g" -e "s/@DATE@/$(date -u +%Y-%m-%d)/g" \
        packaging/appimage/com.openpete.OpenPete.appdata.xml \
        > "$APPDIR/usr/share/metainfo/com.openpete.OpenPete.appdata.xml"

    local IMG="$OUT/OpenPete-${VERSION}-x86_64.AppImage"
    rm -f "$IMG"
    # --runtime-file: without it appimagetool downloads the launcher stub
    # from a rolling tag at package time, so the artifact would depend on
    # the network and on whatever that tag points at today.
    local RT="${OPENPETE_APPIMAGE_RUNTIME:-.tools/appimage-runtime-x86_64}"
    local rt_arg=()
    if [[ -f "$RT" ]]; then rt_arg=(--runtime-file "$RT")
    else echo "[package] NOTE: no pinned AppImage runtime at $RT -" \
              "appimagetool will fetch one (run scripts/setup_appimagetool.sh)" >&2
    fi
    # --appimage-extract-and-run: the tool is itself an AppImage, and
    # build containers routinely have no FUSE.
    ARCH=x86_64 "$TOOL" --appimage-extract-and-run "${rt_arg[@]}" "$APPDIR" "$IMG"
    ( cd "$OUT" && sha256sum "$(basename "$IMG")" > "$(basename "$IMG").sha256" )
    rm -rf "$APPDIR"
    echo "[package] $IMG ready"
}

package_linux() {  # package_linux <exe-dir>
    local B="$1"
    [[ -x "$BIN" ]] || { echo "build linux-release-launcher first ($BIN)" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$BIN" "$STAGE/$EXE"
    # Strip debug info from the SHIPPED copy only (RelWithDebInfo embeds
    # thousands of absolute build paths - a username/layout leak). The
    # build-tree exe keeps its symbols for dev debugging. Exports live in
    # .dynsym, which strip preserves, so the first-boot module still binds.
    strip "$STAGE/$EXE"
    stage_common "$B"
    mkdir -p "$STAGE/recomp/tools"
    cp third_party/psxrecomp/build/PSXRecomp "$STAGE/recomp/tools/"
    # The tarball bundles a compiler only when asked: it is the
    # portable/dev artifact, and a ~320 MB toolchain would quadruple a
    # 3 MB download for users who already have cc. The AppImage always
    # carries one (build_appimage stages it) - the squashfs keeps it
    # compressed and its whole point is that first boot needs nothing
    # installed. find_cc (games/common/recomp_build.cpp) prefers
    # recomp/tools/clang/bin/clang over the system cc either way.
    if [[ -n "$TOOLCHAIN" ]]; then
        echo "[package] bundling clang from $TOOLCHAIN"
        mkdir -p "$STAGE/recomp/tools/clang"
        cp -a "$TOOLCHAIN"/. "$STAGE/recomp/tools/clang/"
    else
        echo "[package] NOTE: no --toolchain - the archive uses the" \
             "system cc (README-dist documents this)." >&2
    fi
    [[ -n "$SIGN_CMD" ]] && echo "[package] NOTE: --sign is Windows-only" \
        "(Authenticode); ignored for linux" >&2
    if [[ -n "$TOOLCHAIN" ]]; then
        stage_licences "$STAGE" --scope toolchain-linux \
            --toolchain-dir "$STAGE/recomp/tools/clang"
    fi
    assert_no_copyleft "$STAGE"
    tar cJf "$OUT/$NAME.tar.xz" -C "$OUT" "$NAME"
    ( cd "$OUT" && sha256sum "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
    if [[ $APPIMAGE -eq 1 ]]; then build_appimage "$STAGE"; fi
}

package_windows() {  # package_windows <exe-dir>
    local B="$1"
    [[ -f "$BIN" ]] || { echo "build windows-mingw-launcher first ($BIN)" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$BIN" "$STAGE/$EXE.exe"
    # Strip debug info from the SHIPPED copy only (same rationale as the
    # Linux branch). PE exports live in the export table data directory,
    # not the COFF symbol table, and the first-boot module links against
    # the staged implib - stripping cannot break either.
    if [[ $APPIMAGE -eq 1 ]]; then
        echo "[package] --appimage is a Linux format; ignored for windows" >&2
    fi
    x86_64-w64-mingw32-strip "$STAGE/$EXE.exe"
    cp "$B/OpenAL32.dll" "$STAGE/" 2>/dev/null \
        || cp third_party/OpenAL-mingw/x86_64-w64-mingw32/bin/OpenAL32.dll "$STAGE/"
    # OpenAL Soft is LGPL: its license text must ride with the DLL (NOTICE.md).
    cp third_party/OpenAL-mingw/COPYING "$STAGE/OpenAL-COPYING.txt"
    stage_common "$B"
    mkdir -p "$STAGE/recomp/tools"
    # The tree builds PSXRecomp.exe from third_party/psxrecomp/recompiler on
    # every build (target openpete_psxrecomp), so this copy is as current as
    # the executable beside it.
    [[ -f "$B/psxrecomp/PSXRecomp.exe" ]] || { echo "no $B/psxrecomp/PSXRecomp.exe:" \
        "cmake --build --preset windows-mingw-launcher" >&2; exit 64; }
    cp "$B/psxrecomp/PSXRecomp.exe" "$STAGE/recomp/tools/"
    # The implib the first-boot module links against. A PE module names the
    # image it imports from and the loader binds that name only to a loaded
    # module of the same file name, so -D is the shipped file name. dlltool
    # derives its internal symbol names from the -l path, so it runs inside
    # the output directory: the build tree's copy names the build path.
    local def
    def=$(realpath "$TREE/gen/openpete_module_exports.def")
    ( cd "$STAGE/recomp/tools" && x86_64-w64-mingw32-dlltool -d "$def" \
        -l libopenpete-exe-exports.a -D "$EXE.exe" )
    if [[ -n "$TOOLCHAIN" ]]; then
        echo "[package] trimming toolchain from $TOOLCHAIN"
        T="$STAGE/recomp/tools/llvm-mingw"
        mkdir -p "$T"
        cp -a "$TOOLCHAIN"/bin "$TOOLCHAIN"/lib "$TOOLCHAIN"/include \
              "$TOOLCHAIN"/x86_64-w64-mingw32 "$T/" 2>/dev/null || true
        # LLVM's licence rides with the trimmed tree; stage_licences below
        # refuses the archive when the source tree has none.
        cp "$TOOLCHAIN"/LICENSE.TXT "$T/" 2>/dev/null || true
        stage_licences "$STAGE" --scope toolchain-windows --toolchain-dir "$T"
        # Upstream llvm-mingw's bin/ is FLAT - every entry is a driver binary,
        # wrapper script or config. Anything that is a directory got there by
        # accident (a build run with its cwd inside the toolchain leaves a whole
        # tree behind, which then rides into the archive: 372MB of stale objects
        # and a recursive copy of the toolchain itself shipped this way once).
        # Prune them so a polluted source tree can't quietly bloat a release.
        find "$T/bin" -mindepth 1 -maxdepth 1 -type d -print -exec rm -rf {} + \
            | sed 's|^|[package] dropped stray dir from toolchain bin: |'
        # x86_64 + C-focused trim: the module compile needs only x86_64 C
        ( cd "$T/bin" && ls | grep -E "^(aarch64|armv7|i686)" | xargs -r rm -f \
          && rm -f lldb* clang-tidy* clang-format* clang-scan-deps* \
                   llvm-pdbutil* llvm-cov* llvm-profdata* liblldb* 2>/dev/null || true )
        rm -rf "$T"/lib/liblldb* "$T"/lib/libscanbuild "$T"/lib/libear
    else
        echo "[package] NOTE: no --toolchain given - archive will need" \
             "\$OPENPETE_CC or a later toolchain drop-in for first boot" >&2
    fi
    # Sign after strip and staging, before archiving. First-party PEs
    # only (see --sign notes above).
    sign_pe "$STAGE/$EXE.exe"
    sign_pe "$STAGE/recomp/tools/PSXRecomp.exe"
    assert_no_copyleft "$STAGE"
    # Zipped from INSIDE the stage dir, so the archive has no wrapping
    # folder: Windows' "Extract All" already creates one named after the
    # zip, and a second level leaves users an install nested one deep.
    ( cd "$STAGE" && python3 -m zipfile -c "../$NAME.zip" . )
    ( cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256" )
}

EXE=openpete
NAME="${EXE}-${VERSION}-${PLATFORM}-x86_64"
STAGE="$OUT/$NAME"
# B: the build tree root, where the executable, recomp/ and sdk/ stage;
# BIN: the executable; DOCS: the directory the docs target writes the
# sites into.
case "$PLATFORM" in
linux)   TREE=build/linux-release-launcher; B="$TREE"; BIN="$B/$EXE"
         ART="$NAME.tar.xz" ;;
windows) TREE=build/windows-mingw-launcher; B="$TREE"; BIN="$B/$EXE.exe"
         ART="$NAME.zip" ;;
esac
DOCS="$TREE/games/spyro-1"
if [[ $DRY_RUN -eq 1 ]]; then
    # The tarball wraps its payload in $NAME/; the zip has no wrapping folder.
    inner="$NAME/"; [[ $PLATFORM == windows ]] && inner=""
    echo "[package] dry run: ${GAMES[*]}: $BIN -> $OUT/$ART:$inner$(basename "$BIN")"
    [[ $PLATFORM == linux && $APPIMAGE -eq 1 ]] && echo "[package] dry run:" \
        "${GAMES[*]}: $BIN -> $OUT/OpenPete-${VERSION}-x86_64.AppImage:usr/$EXE"
    exit 0
fi
"package_$PLATFORM" "$B"
echo "[package] $OUT/$NAME ready"
ls -la "$OUT" | grep "$NAME"

