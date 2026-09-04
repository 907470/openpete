#!/usr/bin/env bash
# package-dist.sh — assemble end-user distribution archives
# (distribution D3; plan openpete-distribution-local-recomp).
#
# Ships ONLY our own code + tools + facts-metadata. No game bytes: no
# recomp module (users first-boot-compile it), no disc data, ROM-free
# icon (the dist presets pin OPENPETE_ROM_FREE_ICON=ON). The knob is a CMake
# option, never a child's environment — wslenv-guard: no-launch.
#
#   scripts/package-dist.sh linux   [--out DIR] [--toolchain DIR] [--appimage]
#   scripts/package-dist.sh windows [--out DIR] [--toolchain DIR]
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
# Inputs: the corresponding *-shared preset build (linux-release-shared /
# windows-mingw-shared) with its staged recomp/ directory. --toolchain
# points at an UNPACKED compiler tree to bundle: llvm-mingw (ucrt
# x86_64) for Windows, a trimmed llvm.org clang for Linux
# (scripts/setup_linux_clang.sh produces one in .tools/clang). Without
# it the Windows zip and the Linux tarball fall back to the user's own
# compiler (documented in README-dist); the AppImage always bundles one,
# since needing nothing installed is the point of it.
set -euo pipefail
cd "$(dirname "$0")/.."

PLATFORM="${1:-}"
shift || true
OUT=dist
TOOLCHAIN=""
WITH_MOD_SDK=0
APPIMAGE=0
# --sign '<command>' — Authenticode hook, Windows platform only. Invoked
# once per first-party PE (openpete-spyro1.exe, PSXRecomp.exe) with the
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
        *) echo "unknown arg: $1" >&2; exit 64 ;;
    esac
done

sign_pe() {  # sign_pe <file> — no-op unless --sign was given
    [[ -n "$SIGN_CMD" ]] || return 0
    echo "[package] signing $(basename "$1")"
    eval "$SIGN_CMD \"\$1\"" || { echo "[package] signing FAILED: $1" >&2; exit 1; }
}

# OPENPETE_DIST_VERSION overrides the derived name. Test builds handed to
# testers want the commit alone (`git rev-parse --short HEAD`) so a report
# names the build without implying a release; leave it unset for releases,
# where the tag is the point.
VERSION="${OPENPETE_DIST_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
NAME="openpete-spyro1-${VERSION}-${PLATFORM}-x86_64"
# Absolute: the example-staging loop below runs inside `cd mods/<id>`
# subshells, where a relative --out no longer resolves.
OUT=$(realpath -m "$OUT")
STAGE="$OUT/$NAME"

stage_common() {  # stage_common <build-game-dir>
    local B="$1"
    mkdir -p "$STAGE/recomp"
    cp -a "$B/recomp/manifest" "$STAGE/recomp/"
    cp -a "$B/recomp/src"      "$STAGE/recomp/"
    # The kit is compiled on the user's machine, so it ships as readable
    # C — but its comments are engineering notes about the host tree.
    scripts/strip-comments.py "$STAGE"/recomp/src/*
    if [[ $WITH_MOD_SDK -eq 1 ]]; then
        # Mod SDK (openpete-mod-compile-on-boot): mods ship mod.toml +
        # src/ and compile at boot against <exe>/sdk with the bundled
        # toolchain. The SDK is a handful of headers, and read-only —
        # unlike mods/ it belongs in the install.
        cp -a "$B/sdk" "$STAGE/sdk"
        # The mod-author docs site (mod_docs target; built only when
        # mkdocs is installed) and the example mods named in
        # mods/examples.txt ride with the SDK, so a zip holder has the
        # reference and working mods without the repository. Examples
        # live under sdk/, not mods/: a mod dropped into mods/ is live on
        # the next boot, and an example is copied there on purpose.
        if [[ -f "$B/moddocs/site/index.html" ]]; then
            cp -a "$B/moddocs/site" "$STAGE/sdk/docs"
        else
            echo "[package-dist] WARNING: $B/moddocs/site missing (mkdocs" \
                 "not installed?) — sdk/docs/ not staged" >&2
        fi
        mkdir -p "$STAGE/sdk/examples"
        while read -r ex; do
            [[ -z "$ex" || "$ex" == \#* ]] && continue
            [[ -f "mods/$ex/mod.toml" ]] \
                || { echo "[package-dist] mods/examples.txt names $ex but" \
                          "mods/$ex/mod.toml does not exist" >&2; exit 1; }
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
        # shipped here would be a directory nobody's mods are read from —
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
    cp gamecontrollerdb.txt "$STAGE/" 2>/dev/null || true
    cp LICENSE NOTICE.md "$STAGE/"
    # Preconfigured settings, so a first run looks the way we intend
    # rather than falling back to bare defaults. Both in-game menus
    # rewrite this file, so anything here is only a starting point.
    cp docs/openpete-dist.toml "$STAGE/openpete.toml"
    # The README template carries both platforms' wording; keep only the
    # blocks for the one being packaged.
    awk -v want="@$PLATFORM" '
        /^<!-- @(windows|linux) -->$/ { skip = ($2 != want); next }
        /^<!-- @end -->$/             { skip = 0; next }
        !skip
    ' docs/README-dist.md.in \
        | sed -e "s/@VERSION@/$VERSION/g" -e "s/@PLATFORM@/$PLATFORM/g" \
        > "$STAGE/README.md"
}

# Libraries an AppImage must NEVER carry: graphics, display-server and
# audio-device clients bind to the host's drivers, and a bundled copy
# shadowing the host's breaks on exactly the machines the image is meant
# to run on. Asserted after staging rather than trusted.
APPIMAGE_FORBIDDEN='^lib(GL|GLX|GLdispatch|EGL|GLESv2|vulkan|X11|Xext|Xrandr|Xi|xcb|wayland|drm|gbm|asound|pulse|pulsecommon|sndio|jack)'

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
    # development packages at all — that is the reason it exists. Ship a
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
    al=$(ldd "$S/openpete-spyro1" | awk '/libopenal\.so/ {print $3}')
    [[ -n "$al" && -f "$al" ]] || { echo "[package] libopenal.so.1 not found" >&2; exit 1; }
    cp -L "$al" "$APPDIR/usr/lib/libopenal.so.1"
    cp third_party/OpenAL-mingw/COPYING "$APPDIR/usr/OpenAL-COPYING.txt"

    local bad
    bad=$(ls "$APPDIR/usr/lib" | grep -E "$APPIMAGE_FORBIDDEN" || true)
    [[ -z "$bad" ]] || {
        echo "[package] driver/display libraries must not be bundled: $bad" >&2
        exit 1; }

    install -m755 docs/appimage/AppRun "$APPDIR/AppRun"
    install -m644 docs/appimage/com.openpete.OpenPete.desktop \
        "$APPDIR/com.openpete.OpenPete.desktop"
    mkdir -p "$APPDIR/usr/share/applications" \
             "$APPDIR/usr/share/icons/hicolor/256x256/apps" \
             "$APPDIR/usr/share/metainfo"
    cp "$APPDIR/com.openpete.OpenPete.desktop" "$APPDIR/usr/share/applications/"
    # The ROM-free mark (tools/gen_dist_icon.py) — the authentic memcard
    # icon must never ship. Generated here so the image can never pick up
    # a ROM-derived one by accident.
    python3 tools/gen_dist_icon.py --png "$APPDIR/com.openpete.OpenPete.png"
    cp "$APPDIR/com.openpete.OpenPete.png" \
       "$APPDIR/usr/share/icons/hicolor/256x256/apps/"
    sed -e "s/@VERSION@/${VERSION#v}/g" -e "s/@DATE@/$(date -u +%Y-%m-%d)/g" \
        docs/appimage/com.openpete.OpenPete.appdata.xml \
        > "$APPDIR/usr/share/metainfo/com.openpete.OpenPete.appdata.xml"

    local IMG="$OUT/OpenPete-${VERSION}-x86_64.AppImage"
    rm -f "$IMG"
    # --runtime-file: without it appimagetool downloads the launcher stub
    # from a rolling tag at package time, so the artifact would depend on
    # the network and on whatever that tag points at today.
    local RT="${OPENPETE_APPIMAGE_RUNTIME:-.tools/appimage-runtime-x86_64}"
    local rt_arg=()
    if [[ -f "$RT" ]]; then rt_arg=(--runtime-file "$RT")
    else echo "[package] NOTE: no pinned AppImage runtime at $RT —" \
              "appimagetool will fetch one (run scripts/setup_appimagetool.sh)" >&2
    fi
    # --appimage-extract-and-run: the tool is itself an AppImage, and
    # build containers routinely have no FUSE.
    ARCH=x86_64 "$TOOL" --appimage-extract-and-run "${rt_arg[@]}" "$APPDIR" "$IMG"
    ( cd "$OUT" && sha256sum "$(basename "$IMG")" > "$(basename "$IMG").sha256" )
    rm -rf "$APPDIR"
    echo "[package] $IMG ready"
}

case "$PLATFORM" in
linux)
    B=build/linux-release-shared/games/spyro-1
    [[ -x "$B/openpete-spyro1" ]] || { echo "build linux-release-shared first" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$B/openpete-spyro1" "$STAGE/"
    # Strip debug info from the SHIPPED copy only (RelWithDebInfo embeds
    # thousands of absolute build paths — a username/layout leak). The
    # build-tree exe keeps its symbols for dev debugging. Exports live in
    # .dynsym, which strip preserves, so the first-boot module still binds.
    strip "$STAGE/openpete-spyro1"
    stage_common "$B"
    mkdir -p "$STAGE/recomp/tools"
    cp third_party/psxrecomp/build/PSXRecomp "$STAGE/recomp/tools/"
    # The tarball bundles a compiler only when asked: it is the
    # portable/dev artifact, and a ~320 MB toolchain would quadruple a
    # 3 MB download for users who already have cc. The AppImage always
    # carries one (build_appimage stages it) — the squashfs keeps it
    # compressed and its whole point is that first boot needs nothing
    # installed. find_cc (recomp_build.cpp) prefers
    # recomp/tools/clang/bin/clang over the system cc either way.
    if [[ -n "$TOOLCHAIN" ]]; then
        echo "[package] bundling clang from $TOOLCHAIN"
        mkdir -p "$STAGE/recomp/tools/clang"
        cp -a "$TOOLCHAIN"/. "$STAGE/recomp/tools/clang/"
    else
        echo "[package] NOTE: no --toolchain — the archive uses the" \
             "system cc (README-dist documents this)." >&2
    fi
    [[ -n "$SIGN_CMD" ]] && echo "[package] NOTE: --sign is Windows-only" \
        "(Authenticode); ignored for linux" >&2
    tar cJf "$OUT/$NAME.tar.xz" -C "$OUT" "$NAME"
    ( cd "$OUT" && sha256sum "$NAME.tar.xz" > "$NAME.tar.xz.sha256" )
    if [[ $APPIMAGE -eq 1 ]]; then build_appimage "$STAGE"; fi
    ;;
windows)
    B=build/windows-mingw-shared/games/spyro-1
    [[ -f "$B/openpete-spyro1.exe" ]] || { echo "build windows-mingw-shared first" >&2; exit 64; }
    rm -rf "$STAGE"; mkdir -p "$STAGE"
    cp "$B/openpete-spyro1.exe" "$STAGE/"
    # Strip debug info from the SHIPPED copy only (same rationale as the
    # Linux branch). PE exports live in the export table data directory,
    # not the COFF symbol table, and the first-boot module links against
    # the staged implib — stripping cannot break either.
    if [[ $APPIMAGE -eq 1 ]]; then
        echo "[package] --appimage is a Linux format; ignored for windows" >&2
    fi
    x86_64-w64-mingw32-strip "$STAGE/openpete-spyro1.exe"
    cp "$B/OpenAL32.dll" "$STAGE/" 2>/dev/null \
        || cp third_party/OpenAL-mingw/x86_64-w64-mingw32/bin/OpenAL32.dll "$STAGE/"
    # OpenAL Soft is LGPL: its license text must ride with the DLL (NOTICE.md).
    cp third_party/OpenAL-mingw/COPYING "$STAGE/OpenAL-COPYING.txt"
    stage_common "$B"
    mkdir -p "$STAGE/recomp/tools"
    cp third_party/psxrecomp/build-mingw/PSXRecomp.exe "$STAGE/recomp/tools/"
    cp "$B/recomp/tools/libopenpete-exe-exports.a" "$STAGE/recomp/tools/"
    if [[ -n "$TOOLCHAIN" ]]; then
        echo "[package] trimming toolchain from $TOOLCHAIN"
        T="$STAGE/recomp/tools/llvm-mingw"
        mkdir -p "$T"
        cp -a "$TOOLCHAIN"/bin "$TOOLCHAIN"/lib "$TOOLCHAIN"/include \
              "$TOOLCHAIN"/x86_64-w64-mingw32 "$T/" 2>/dev/null || true
        # Upstream llvm-mingw's bin/ is FLAT — every entry is a driver binary,
        # wrapper script or config. Anything that is a directory got there by
        # accident (a build run with its cwd inside the toolchain leaves a whole
        # tree behind, which then rides into the archive: 372MB of stale objects
        # and a recursive copy of the toolchain itself shipped this way once).
        # Prune them so a polluted source tree can't quietly bloat a release.
        find "$T/bin" -mindepth 1 -maxdepth 1 -type d -print -exec rm -rf {} + \
            | sed 's|^|[package] dropped stray dir from toolchain bin: |'
        # x86_64 + C-focused trim (see plan D3.1 measurements)
        ( cd "$T/bin" && ls | grep -E "^(aarch64|armv7|i686)" | xargs -r rm -f \
          && rm -f lldb* clang-tidy* clang-format* clang-scan-deps* \
                   llvm-pdbutil* llvm-cov* llvm-profdata* liblldb* 2>/dev/null || true )
        rm -rf "$T"/lib/liblldb* "$T"/lib/libscanbuild "$T"/lib/libear
    else
        echo "[package] NOTE: no --toolchain given — archive will need" \
             "\$OPENPETE_CC or a later toolchain drop-in for first boot" >&2
    fi
    # Sign after strip and staging, before archiving. First-party PEs
    # only (see --sign notes above).
    sign_pe "$STAGE/openpete-spyro1.exe"
    sign_pe "$STAGE/recomp/tools/PSXRecomp.exe"
    # Zipped from INSIDE the stage dir, so the archive has no wrapping
    # folder: Windows' "Extract All" already creates one named after the
    # zip, and a second level leaves users an install nested one deep.
    ( cd "$STAGE" && python3 -m zipfile -c "../$NAME.zip" . )
    ( cd "$OUT" && sha256sum "$NAME.zip" > "$NAME.zip.sha256" )
    ;;
*)
    echo "usage: scripts/package-dist.sh {linux|windows} [--out DIR]" \
         "[--toolchain DIR] [--with-mod-sdk] [--appimage (linux)]" >&2
    exit 64
    ;;
esac

echo "[package] $OUT/$NAME ready"
ls -la "$OUT" | grep "$NAME"
