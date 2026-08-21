#!/usr/bin/env bash
# check_dist_first_boot.sh — the distribution gate (D4/D6.5; plan
# openpete-distribution-local-recomp).
#
# Proves the SHIPPED artifact works: package it, put it somewhere clean,
# --ingest the reference disc, let first boot extract+recompile+link the
# module, then replay the full 20k hardware oracle against the dist exe +
# its self-built module. Finally, drop a source-only mod into the mods
# directory and prove a subsequent boot compiles + loads it
# (openpete-mod-compile-on-boot). This is the closest thing to "a stranger
# downloaded it and it worked" that CI can check.
#
#   scripts/check_dist_first_boot.sh              # the .tar.xz artifact
#   scripts/check_dist_first_boot.sh --appimage   # the AppImage artifact
#
# The AppImage variant is the same chain against a different artifact,
# plus the three things only a read-only, path-varying mount can get
# wrong: nothing may be written inside the payload, the module cache must
# survive a mount path that changes every launch (otherwise every launch
# pays a 12 s+ recompile), and no driver/display library may be bundled.
#
# Both variants run against a scratch HOME/XDG, because a fresh install
# writes to the per-user directories (openpete_paths) — the gate must not
# read or write the developer's real ones.
#
# The glibc floor is asserted here too (D6.6): a release-base artifact
# must not require symbols newer than the target rung, or it fails to
# start on the distributions it claims to support, with a loader error the
# user cannot act on. Building on a dev box legitimately exceeds it — set
# OPENPETE_DIST_ALLOW_GLIBC_FLOOR=1 to downgrade that to a loud warning.
#
# Exit 0 = PASS; 2 = failure; 64 = prerequisites missing.
set -euo pipefail
cd "$(dirname "$0")/.."

APPIMAGE=0
[[ "${1:-}" == "--appimage" ]] && APPIMAGE=1

# Debian 11 / bullseye — see "Build base and the glibc floor" in the plan.
GLIBC_MAX=2.31
GLIBCXX_MAX=3.4.28

DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
TRACE=tests/golden/spyro-1/psxtrace-demo-20k.bin
[[ -f "$DISC" ]] || { echo "[dist-gate] disc missing: $DISC" >&2; exit 64; }
[[ -f "$TRACE" ]] || { echo "[dist-gate] golden missing: $TRACE" >&2; exit 64; }
[[ -x build/linux-release-shared/games/spyro-1/openpete-spyro1 ]] \
    || { echo "[dist-gate] build linux-release-shared first" >&2; exit 64; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/openpete-dist-gate.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# A scratch per-user layout. Everything the artifact writes lands here, so
# the gate can inspect it and the developer's own library is untouched.
export HOME="$WORK/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_STATE_HOME="$HOME/.local/state"
export XDG_DATA_HOME="$HOME/.local/share"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME" "$XDG_DATA_HOME"
DATA="$XDG_DATA_HOME/openpete"

DISC_ABS=$(realpath "$DISC")
TRACE_ABS=$(realpath "$TRACE")

echo "[dist-gate] packaging..."
# --with-mod-sdk: releases ship without the mod SDK, but the boot-time
# mod compiler is still a supported path, so the gate packages it in.
pkg_args=(linux --with-mod-sdk --out "$WORK/dist")
[[ $APPIMAGE -eq 1 ]] && pkg_args+=(--appimage)
scripts/package-dist.sh "${pkg_args[@]}" >/dev/null

if [[ $APPIMAGE -eq 1 ]]; then
    image=$(ls "$WORK"/dist/*.AppImage)
    echo "[dist-gate] $(basename "$image") ($(du -h "$image" | cut -f1))"
    # Extract rather than mount: a writable copy of the payload is the
    # only way to observe a write the read-only mount would have
    # swallowed, and it is also the FUSE-less path CI needs.
    ( cd "$WORK" && "$image" --appimage-extract >/dev/null )
    appdir="$WORK/squashfs-root"
    APP=("$appdir/AppRun")
    BIN="$appdir/usr/openpete-spyro1"
    # Content manifest of the payload, to be re-checked after the run.
    ( cd "$appdir" && find . -type f -exec sha256sum {} + | sort -k2 ) \
        > "$WORK/payload-before.txt"
else
    archive=$(ls "$WORK"/dist/*.tar.xz)
    echo "[dist-gate] extracting $(basename "$archive") into a clean dir..."
    mkdir -p "$WORK/install"
    tar xJf "$archive" -C "$WORK/install"
    appdir=$(ls -d "$WORK"/install/*/)
    APP=("$appdir/openpete-spyro1")
    BIN="$appdir/openpete-spyro1"
fi

echo "[dist-gate] glibc floor..."
have_glibc=$(objdump -T "$BIN" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
have_glibcxx=$(objdump -T "$BIN" | grep -o 'GLIBCXX_[0-9.]*' | sort -uV | tail -1)
echo "[dist-gate]   requires ${have_glibc:-none} / ${have_glibcxx:-none}" \
     "(target GLIBC_$GLIBC_MAX / GLIBCXX_$GLIBCXX_MAX)"
floor_ok=1
newest() { printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1; }
[[ -z "$have_glibc"   || $(newest "${have_glibc#GLIBC_}" "$GLIBC_MAX") == "$GLIBC_MAX" ]] || floor_ok=0
[[ -z "$have_glibcxx" || $(newest "${have_glibcxx#GLIBCXX_}" "$GLIBCXX_MAX") == "$GLIBCXX_MAX" ]] || floor_ok=0
if [[ $floor_ok -eq 0 ]]; then
    if [[ -n "${OPENPETE_DIST_ALLOW_GLIBC_FLOOR:-}" ]]; then
        echo "[dist-gate]   WARNING: floor exceeded — this artifact will NOT" \
             "start on the oldest supported distributions. Tolerated because" \
             "OPENPETE_DIST_ALLOW_GLIBC_FLOOR is set; a release must be built" \
             "in the Debian 11 container." >&2
    else
        echo "[dist-gate] FAIL: symbol floor exceeded; build in the release" \
             "container (or set OPENPETE_DIST_ALLOW_GLIBC_FLOOR=1 for a" \
             "dev-box run)" >&2
        exit 2
    fi
fi

if [[ $APPIMAGE -eq 1 ]]; then
    echo "[dist-gate] no driver/display libraries bundled..."
    bad=$(find "$appdir" -name 'lib*.so*' -printf '%f\n' | sort -u \
          | grep -E '^lib(GL|GLX|GLdispatch|EGL|GLESv2|vulkan|X11|Xext|Xrandr|Xi|xcb|wayland|drm|gbm|asound|pulse|pulsecommon|sndio|jack)' || true)
    [[ -z "$bad" ]] || { echo "[dist-gate] FAIL: bundled driver libraries: $bad" >&2; exit 2; }
fi

echo "[dist-gate] first boot (--ingest)..."
if [[ $APPIMAGE -eq 1 ]]; then
    # An empty PATH is how the gate proves self-containment: the image
    # must reach its own compiler and linker by absolute path. A host cc
    # or binutils quietly filling in for a missing bundle would otherwise
    # pass here and fail on a user's machine that has neither.
    mkdir -p "$WORK/empty-path"
    PATH="$WORK/empty-path" "${APP[@]}" --headless --frame-limit 300 \
          --ingest "$DISC_ABS" --max-runtime-seconds 900 \
          > "$WORK/first-boot.log" 2>&1 \
        || { echo "[dist-gate] FAIL: first boot did not complete with an" \
                  "empty PATH — the image is not self-contained" >&2;
             tail -20 "$WORK/first-boot.log" >&2; exit 2; }
    grep -q "cc=.*recomp/tools/clang/bin/clang" "$WORK/first-boot.log" \
        || { echo "[dist-gate] FAIL: first boot did not use the bundled" \
                  "compiler:" "$(grep -o 'cc=[^ ]*' "$WORK/first-boot.log" | head -1)" >&2;
             exit 2; }
else
    "${APP[@]}" --headless --frame-limit 300 \
          --ingest "$DISC_ABS" --max-runtime-seconds 900 \
          > "$WORK/first-boot.log" 2>&1 \
        || { echo "[dist-gate] FAIL: first boot did not complete" >&2;
             tail -20 "$WORK/first-boot.log" >&2; exit 2; }
fi

module=$(ls "$DATA"/library/*/modules/*/spyro1-recomp.so 2>/dev/null | head -1)
[[ -n "$module" ]] || { echo "[dist-gate] FAIL: no module built under $DATA" >&2;
                        tail -20 "$WORK/first-boot.log" >&2; exit 2; }
grep -q "first-boot module build" "$WORK/first-boot.log" \
    || { echo "[dist-gate] FAIL: first boot did not build a module" >&2; exit 2; }
# The module must not have acquired a shared-library dependency it cannot
# resolve: it is linked -nostdlib and binds everything else against the
# already-loaded exe at dlopen (D6.0).
needed=$(objdump -p "$module" | awk '/NEEDED/ {print $2}' || true)
[[ -z "$needed" ]] || { echo "[dist-gate] FAIL: module carries DT_NEEDED: $needed" >&2; exit 2; }

echo "[dist-gate] full 20k oracle on the dist exe + self-built module..."
"${APP[@]}" --headless \
      --run-recomp 0x80012204 \
      --psxtrace-verify "$TRACE_ABS" \
      --frame-limit 200000 > "$WORK/oracle.log" 2>&1 \
    || { echo "[dist-gate] FAIL: oracle divergence or crash" >&2;
         tail -20 "$WORK/oracle.log" >&2; exit 2; }

if [[ $APPIMAGE -eq 1 ]]; then
    echo "[dist-gate] warm boot re-attaches (module cache is mount-independent)..."
    module_stamp_before=$(stat -c %Y "$module")
    "${APP[@]}" --headless --frame-limit 60 --max-runtime-seconds 300 \
          > "$WORK/warm-boot.log" 2>&1 \
        || { echo "[dist-gate] FAIL: warm boot did not complete" >&2;
             tail -20 "$WORK/warm-boot.log" >&2; exit 2; }
    ! grep -q "first-boot module build" "$WORK/warm-boot.log" \
        || { echo "[dist-gate] FAIL: warm boot rebuilt the module — the cache" \
                  "key is not independent of the mount path" >&2; exit 2; }
    [[ "$(stat -c %Y "$module")" == "$module_stamp_before" ]] \
        || { echo "[dist-gate] FAIL: warm boot rewrote the cached module" >&2; exit 2; }

    echo "[dist-gate] nothing was written inside the payload..."
    ( cd "$appdir" && find . -type f -exec sha256sum {} + | sort -k2 ) \
        > "$WORK/payload-after.txt"
    diff -u "$WORK/payload-before.txt" "$WORK/payload-after.txt" \
        > "$WORK/payload.diff" \
        || { echo "[dist-gate] FAIL: the run modified the AppImage payload:" >&2;
             head -20 "$WORK/payload.diff" >&2; exit 2; }
fi

echo "[dist-gate] drop-in source mod compiles + loads on boot..."
mkdir -p "$DATA/mods"
cp -r mods/hello-anchor "$DATA/mods/"
rm -rf "$DATA/mods/hello-anchor/.build"
"${APP[@]}" --headless --frame-limit 300 \
      --run-recomp 0x80012204 --max-runtime-seconds 300 \
      > "$WORK/mod-boot.log" 2>&1 \
    || { echo "[dist-gate] FAIL: boot with drop-in mod crashed" >&2;
         tail -20 "$WORK/mod-boot.log" >&2; exit 2; }
[[ -f "$DATA/mods/hello-anchor/.build/hello-anchor.so" ]] \
    || { echo "[dist-gate] FAIL: drop-in mod did not compile" >&2;
         tail -20 "$WORK/mod-boot.log" >&2; exit 2; }
grep -q "loaded hello-anchor" "$WORK/mod-boot.log" \
    || { echo "[dist-gate] FAIL: drop-in mod compiled but did not load" >&2;
         tail -20 "$WORK/mod-boot.log" >&2; exit 2; }

what=$([[ $APPIMAGE -eq 1 ]] && echo AppImage || echo tarball)
echo "[dist-gate] PASS — $what: unpack → ingest → self-build → oracle-clean → mod drop-in"
