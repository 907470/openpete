#!/usr/bin/env bash
# scripts/setup_linux_clang.sh - fetch and trim the Linux clang that the
# AppImage bundles for the first-boot module compile.
#
# Produces .tools/clang/ (gitignored), which package-dist.sh stages as
# recomp/tools/clang - where find_cc (games/common/recomp_build.cpp) looks before the
# system compiler.
#
# Version choice: 18.1.8 is the NEWEST llvm.org release whose Linux
# tarball is still built on Ubuntu 18.04. That matters, because the
# bundled compiler's own symbol floor is a floor on the whole product:
# a runtime built for glibc 2.31 that ships a compiler needing 2.34
# starts fine and then fails to build the game. Measured:
#   18.1.8 ubuntu-18.04 : GLIBC_2.27  GLIBCXX_3.4.21   <- under target
#   22.1.8 Linux-X64    : GLIBC_2.34  GLIBCXX_3.4.30   <- over
#
# The trim keeps only what the builds invoke. The generated MIPS
# translation compiles against the compiler's own freestanding headers
# (see psx_runtime.h) and links -nostdlib, so it needs no sysroot, no crt
# objects and no libc headers. A source mod may include libc headers and
# may need compiler runtime routines (__divti3 for an __int128 division);
# the mod loader compiles it with -nostdlibinc against sysroot/include and
# links -nostdlib -nostartfiles plus sysroot/lib/libclang_rt.builtins.a,
# so a mod build reads nothing from the host either (core/mods/
# mod_loader.c). libc symbols resolve at dlopen from the process, which
# already holds libc:
#
#   bin/clang, bin/clang-18   the driver + the real binary
#   bin/clang-cpp             driver alias the toolchain expects
#   bin/lld, bin/ld.lld       the linker...
#   bin/ld                    ...found under the name clang's driver
#                             searches for in its own directory, so the
#                             bundle does not need host binutils either
#   lib/clang/18/include      stdint.h / stddef.h and friends
#   lib/libtinfo.so.5         see below
#   LICENSE.TXT               Apache-2.0 with LLVM exception (NOTICE.md)
#   sysroot/include           glibc 2.31 and Linux 5.10 uapi headers, the
#                             release container's (Debian 11) libc6-dev and
#                             linux-libc-dev, with the multiarch directory
#                             x86_64-linux-gnu/ merged into include/ so one
#                             -isystem names them all
#   sysroot/lib/libclang_rt.builtins.a
#                             compiler-rt builtins from the same tarball
#                             (lib/clang/18/lib/x86_64-unknown-linux-gnu/)
#   sysroot/licences/         the two packages' Debian copyright files
#   sysroot/STAMP             the pins below; a mismatch re-runs the setup
#
# The two .debs come from snapshot.debian.org at the instant the release
# Dockerfile pins (packaging/linux-release.Dockerfile SNAPSHOT), so the
# headers are byte-identical to the container's /usr/include; the release
# container refuses a staged tree that differs (scripts/release-container.sh).
#
# libtinfo: the upstream binaries carry a DT_NEEDED on libtinfo.so.5,
# which no current distribution ships - but they import NOT ONE SYMBOL
# from it (verified below), so it is a stale link-time dependency. An
# empty stub with the expected version node satisfies the loader; both
# binaries already have RUNPATH=$ORIGIN/../lib, so dropping it in lib/
# resolves without any LD_LIBRARY_PATH that could leak into the game
# process and shadow a host library.
#
# Idempotent - re-running with the tree already present verifies and exits.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="18.1.8"
STEM="clang+llvm-${VERSION}-x86_64-linux-gnu-ubuntu-18.04"
SHA256="54ec30358afcc9fb8aa74307db3046f5187f9fb89fb37064cdde906e062ebf36"
URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${VERSION}/${STEM}.tar.xz"
MAJOR="${VERSION%%.*}"
DEST=".tools/clang"
# The binary tarball carries only a pointer to the license, not its text,
# and Apache-2.0 requires the text to accompany a binary distribution -
# so fetch the canonical file from the same tag.
LIC_URL="https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-${VERSION}/llvm/LICENSE.TXT"
LIC_SHA256="8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee"
BUILTINS="lib/clang/$MAJOR/lib/x86_64-unknown-linux-gnu/libclang_rt.builtins.a"
BUILTINS_SHA256="4362cf6a98152bf014ef63709291c55603fc8acb047c20c9a7abdde94b3c55e3"
# The release container's libc6-dev and linux-libc-dev (dpkg-query -W in
# the image built from packaging/linux-release.Dockerfile).
SNAPSHOT="20260903T000000Z"
DEB_BASE="https://snapshot.debian.org/archive/debian-security/$SNAPSHOT/pool/updates/main"
LIBC_DEB="libc6-dev_2.31-13+deb11u14_amd64.deb"
LIBC_URL="$DEB_BASE/g/glibc/$LIBC_DEB"
LIBC_SHA256="7d4002192e09c3cc97f31dcc85ecd7741adbec9011abbdd14fb1790cab20bffa"
LINUX_DEB="linux-libc-dev_5.10.262-1_amd64.deb"
LINUX_URL="$DEB_BASE/l/linux/$LINUX_DEB"
LINUX_SHA256="77e61a88d14b5dabfea170587fc2cb1020b950c6326d54915f54c4f16fb819e8"
STAMP="$VERSION $BUILTINS_SHA256 $LIBC_SHA256 $LINUX_SHA256"

if [[ -x "$DEST/bin/clang" ]] && "$DEST/bin/clang" --version 2>/dev/null \
        | grep -q "clang version $VERSION" \
   && [[ "$(cat "$DEST/sysroot/STAMP" 2>/dev/null)" == "$STAMP" ]]; then
    echo "[setup-clang] $DEST already at $VERSION with its sysroot"
    exit 0
fi

mkdir -p .tools
tarball=".tools/${STEM}.tar.xz"
if [[ ! -f "$tarball" ]] || ! echo "$SHA256  $tarball" | sha256sum -c --status -; then
    echo "[setup-clang] downloading $STEM (~1 GB)"
    curl --fail --retry 5 -L -o "$tarball.tmp" "$URL"
    echo "$SHA256  $tarball.tmp" | sha256sum -c -
    mv "$tarball.tmp" "$tarball"
fi

echo "[setup-clang] extracting the parts the module build uses"
rm -rf "$DEST" .tools/clang-x
mkdir -p .tools/clang-x
tar xf "$tarball" -C .tools/clang-x \
    "$STEM/bin/clang" "$STEM/bin/clang-$MAJOR" "$STEM/bin/clang-cpp" \
    "$STEM/bin/lld" "$STEM/bin/ld.lld" \
    "$STEM/lib/clang/$MAJOR/include" "$STEM/$BUILTINS"
mv ".tools/clang-x/$STEM" "$DEST"
rmdir .tools/clang-x
ln -sf lld "$DEST/bin/ld"

echo "[setup-clang] sysroot: compiler-rt builtins"
echo "$BUILTINS_SHA256  $DEST/$BUILTINS" | sha256sum -c -
mkdir -p "$DEST/sysroot/lib" "$DEST/sysroot/include" "$DEST/sysroot/licences"
mv "$DEST/$BUILTINS" "$DEST/sysroot/lib/"
rmdir "$DEST/lib/clang/$MAJOR/lib/x86_64-unknown-linux-gnu" "$DEST/lib/clang/$MAJOR/lib"

echo "[setup-clang] sysroot: glibc and Linux uapi headers"
debs=$(mktemp -d)
for spec in "$LIBC_DEB $LIBC_URL $LIBC_SHA256 glibc" \
            "$LINUX_DEB $LINUX_URL $LINUX_SHA256 linux"; do
    read -r deb url sha lic <<<"$spec"
    if [[ ! -f ".tools/$deb" ]] || ! echo "$sha  .tools/$deb" | sha256sum -c --status -; then
        curl --fail --retry 5 -L -o ".tools/$deb.tmp" "$url"
        echo "$sha  .tools/$deb.tmp" | sha256sum -c -
        mv ".tools/$deb.tmp" ".tools/$deb"
    fi
    mkdir -p "$debs/$lic"
    dpkg-deb -x ".tools/$deb" "$debs/$lic"
    cp "$debs/$lic"/usr/share/doc/*/copyright "$DEST/sysroot/licences/$lic.copyright"
    # Merge the multiarch directory into include/; a name present in both
    # would make one -isystem ambiguous, so it is refused.
    inc="$debs/$lic/usr/include"
    clash=$( (cd "$inc/x86_64-linux-gnu" && find . -type f) | while read -r f; do
                 [[ -e "$inc/$f" || -e "$DEST/sysroot/include/$f" ]] && echo "$f"; done || true)
    [[ -z "$clash" ]] || { echo "[setup-clang] multiarch header clash: $clash" >&2; exit 1; }
    cp -a "$inc/x86_64-linux-gnu"/. "$DEST/sysroot/include/"
    rm -rf "${inc:?}/x86_64-linux-gnu"
    cp -a "$inc"/. "$DEST/sysroot/include/"
done
rm -rf "${debs:?}"
echo "$STAMP" > "$DEST/sysroot/STAMP"

echo "[setup-clang] libtinfo stub"
# Prove the premise rather than assuming it: if a future toolchain really
# did call into libtinfo, an empty stub would fail at the first
# diagnostic instead of here.
undef=$(nm -D --undefined-only "$DEST/bin/clang-$MAJOR" "$DEST/bin/lld" \
        | awk '{print $2}' | grep -E '^(setupterm|tigetnum|tigetstr|tigetflag|tputs|tparm|del_curterm|set_curterm|cur_term)$' || true)
[[ -z "$undef" ]] || {
    echo "[setup-clang] the bundled binaries DO use libtinfo ($undef);" \
         "an empty stub is not safe - bundle a real ncurses instead" >&2
    exit 1; }
stub=$(mktemp -d); trap 'rm -rf "$stub"' EXIT
echo 'static int openpete_libtinfo_stub;' > "$stub/stub.c"
# The version node the upstream binaries name in their version reference.
echo 'NCURSES_TINFO_5.0.19991023 { local: *; };' > "$stub/stub.map"
"${CC:-cc}" -shared -fPIC -nostdlib -Wl,-soname,libtinfo.so.5 \
    -Wl,--version-script="$stub/stub.map" \
    -o "$DEST/lib/libtinfo.so.5" "$stub/stub.c"

echo "[setup-clang] license text"
curl --fail --retry 5 -L -o "$DEST/LICENSE.TXT.tmp" "$LIC_URL"
echo "$LIC_SHA256  $DEST/LICENSE.TXT.tmp" | sha256sum -c -
mv "$DEST/LICENSE.TXT.tmp" "$DEST/LICENSE.TXT"

"$DEST/bin/clang" --version | head -1
echo "[setup-clang] $DEST ready ($(du -sh "$DEST" | cut -f1))"
