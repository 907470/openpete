#!/usr/bin/env bash
# scripts/setup_linux_clang.sh — fetch and trim the Linux clang that the
# AppImage bundles for the first-boot module compile (distribution D6.3;
# plan openpete-distribution-local-recomp).
#
# Produces .tools/clang/ (gitignored), which package-dist.sh stages as
# recomp/tools/clang — where find_cc (recomp_build.cpp) looks before the
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
# The trim keeps only what the module build actually invokes. The
# generated MIPS translation compiles against the compiler's own
# freestanding headers (see psx_runtime.h) and links -nostdlib, so no
# sysroot, no crt objects and no libc headers are needed:
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
#
# libtinfo: the upstream binaries carry a DT_NEEDED on libtinfo.so.5,
# which no current distribution ships — but they import NOT ONE SYMBOL
# from it (verified below), so it is a stale link-time dependency. An
# empty stub with the expected version node satisfies the loader; both
# binaries already have RUNPATH=$ORIGIN/../lib, so dropping it in lib/
# resolves without any LD_LIBRARY_PATH that could leak into the game
# process and shadow a host library.
#
# Idempotent — re-running with the tree already present verifies and exits.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="18.1.8"
STEM="clang+llvm-${VERSION}-x86_64-linux-gnu-ubuntu-18.04"
SHA256="54ec30358afcc9fb8aa74307db3046f5187f9fb89fb37064cdde906e062ebf36"
URL="https://github.com/llvm/llvm-project/releases/download/llvmorg-${VERSION}/${STEM}.tar.xz"
MAJOR="${VERSION%%.*}"
DEST=".tools/clang"
# The binary tarball carries only a pointer to the license, not its text,
# and Apache-2.0 requires the text to accompany a binary distribution —
# so fetch the canonical file from the same tag.
LIC_URL="https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-${VERSION}/llvm/LICENSE.TXT"
LIC_SHA256="8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee"

if [[ -x "$DEST/bin/clang" ]] && "$DEST/bin/clang" --version 2>/dev/null \
        | grep -q "clang version $VERSION"; then
    echo "[setup-clang] $DEST already at $VERSION"
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
    "$STEM/lib/clang/$MAJOR/include"
mv ".tools/clang-x/$STEM" "$DEST"
rmdir .tools/clang-x
ln -sf lld "$DEST/bin/ld"

echo "[setup-clang] libtinfo stub"
# Prove the premise rather than assuming it: if a future toolchain really
# did call into libtinfo, an empty stub would fail at the first
# diagnostic instead of here.
undef=$(nm -D --undefined-only "$DEST/bin/clang-$MAJOR" "$DEST/bin/lld" \
        | awk '{print $2}' | grep -E '^(setupterm|tigetnum|tigetstr|tigetflag|tputs|tparm|del_curterm|set_curterm|cur_term)$' || true)
[[ -z "$undef" ]] || {
    echo "[setup-clang] the bundled binaries DO use libtinfo ($undef);" \
         "an empty stub is not safe — bundle a real ncurses instead" >&2
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
