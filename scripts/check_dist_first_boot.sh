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
# OPENPETE_DIST_ARTIFACT=<file> skips the packaging step and gates that
# already-built .tar.xz or .AppImage instead; with it set, no build tree is
# needed, so the gate runs on a second machine from a copy of this script
# alone. OPENPETE_DISC and OPENPETE_TRACE name the disc and the golden.
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

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a machine-driven run — nobody is holding the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

# PsyCross rasterizes the OT for the oracle, so every run here needs a
# reachable display even under --headless. An ssh session carries none of the
# login session's, and the resulting failure is a boot-time SDL error a long
# way from its cause. Adopt the login session's socket when the environment
# names no display and one is there to adopt; say so, because a run against a
# borrowed display should not look like a run that had one.
if [[ -z "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]]; then
    : "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
    for sock in "$XDG_RUNTIME_DIR"/wayland-*; do
        [[ -S "$sock" ]] || continue
        export XDG_RUNTIME_DIR WAYLAND_DISPLAY="${sock##*/}"
        echo "[dist-gate] no display in the environment — adopting $WAYLAND_DISPLAY"
        break
    done
    if [[ -z "${WAYLAND_DISPLAY:-}" && -S /tmp/.X11-unix/X0 ]]; then
        export DISPLAY=:0
        echo "[dist-gate] no display in the environment — adopting DISPLAY=:0"
    fi
fi

cd "$(dirname "$0")/.."

APPIMAGE=0
[[ "${1:-}" == "--appimage" ]] && APPIMAGE=1

# Debian 11 / bullseye — see "Build base and the glibc floor" in the plan.
GLIBC_MAX=2.31
GLIBCXX_MAX=3.4.28

DISC="${OPENPETE_DISC:-games/spyro-1/spyro-1/build/SCUS-94228.cue}"
TRACE="${OPENPETE_TRACE:-tests/golden/spyro-1/psxtrace-demo-20k.bin}"
ARTIFACT="${OPENPETE_DIST_ARTIFACT:-}"
[[ -f "$DISC" ]] || { echo "[dist-gate] disc missing: $DISC" >&2; exit 64; }
[[ -f "$TRACE" ]] || { echo "[dist-gate] golden missing: $TRACE" >&2; exit 64; }
if [[ -n "$ARTIFACT" ]]; then
    [[ -f "$ARTIFACT" ]] || { echo "[dist-gate] artifact missing: $ARTIFACT" >&2; exit 64; }
    case "$ARTIFACT" in *.AppImage) APPIMAGE=1 ;; esac
else
    [[ -x build/linux-release-shared/games/spyro-1/openpete-spyro1 ]] \
        || { echo "[dist-gate] build linux-release-shared first" >&2; exit 64; }
fi

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

if [[ -n "$ARTIFACT" ]]; then
    echo "[dist-gate] using $(basename "$ARTIFACT")..."
    mkdir -p "$WORK/dist"
    cp "$ARTIFACT" "$WORK/dist/"
else
    echo "[dist-gate] packaging..."
    # Releases ship the mod SDK; the gate packages the same way.
    pkg_args=(linux --with-mod-sdk --out "$WORK/dist")
    [[ $APPIMAGE -eq 1 ]] && pkg_args+=(--appimage)
    scripts/package-dist.sh "${pkg_args[@]}" >/dev/null
fi

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
          | grep -E '^lib(GL|GLX|GLdispatch|EGL|GLESv2|vulkan|X11|Xext|Xrandr|Xi|xcb|wayland|drm|gbm|asound|pulse|pulsecommon|jack)' || true)
    [[ -z "$bad" ]] || { echo "[dist-gate] FAIL: bundled driver libraries: $bad" >&2; exit 2; }

    # The check above only catches a library we wrongly included. This one
    # catches one we wrongly omitted, which is the failure a user sees: a
    # DT_NEEDED that resolves on the packaging machine and nowhere else
    # aborts in ld.so before main. Everything the payload needs must be in
    # usr/lib, be part of the C runtime, or be a host driver the image is
    # required not to carry.
    echo "[dist-gate] dependency closure..."
    host_core='^(ld-linux[^/]*|libc|libm|libdl|libpthread|librt|libresolv|libutil|libstdc\+\+|libgcc_s)\.so'
    drivers='^lib(GL|GLX|GLdispatch|EGL|GLESv2|vulkan|X11|Xext|Xrandr|Xi|xcb|wayland|drm|gbm|asound|pulse|pulsecommon|jack)'
    miss=""
    while read -r f; do
        while read -r n; do
            [[ -n "$n" ]] || continue
            [[ "$n" =~ $host_core || "$n" =~ $drivers ]] && continue
            [[ -e "$appdir/usr/lib/$n" ]] && continue
            miss+=" $n(<-$(basename "$f"))"
        done < <(readelf -d "$f" | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p')
    done < <(printf '%s\n' "$BIN" "$appdir"/usr/lib/*.so*)
    [[ -z "$miss" ]] || {
        echo "[dist-gate] FAIL: unbundled dependencies:$miss" >&2; exit 2; }
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

# A GUI launch has no terminal, so the log file is the only thing a user
# can attach to a report. Assert the first boot — the run most likely to
# go wrong on a stranger's machine — actually left one, and that it
# carries the build it did rather than an empty shell.
LOGF="$XDG_STATE_HOME/openpete/logs/openpete.log"
[[ -s "$LOGF" ]] \
    || { echo "[dist-gate] FAIL: first boot wrote no log file at $LOGF" >&2;
         exit 2; }
grep -q "first-boot module build" "$LOGF" \
    || { echo "[dist-gate] FAIL: the log file did not capture the module" \
              "build it was open for" >&2; exit 2; }
echo "[dist-gate] first boot left a log file ($(wc -c < "$LOGF") bytes)..."

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

# An update that changes what the recompiler emits must re-key the module
# cache. Keying on the PSXRecomp binary alone did NOT: a shipped manifest
# change (a new funcs list, a channel-site update — the most common kind
# of change between releases) silently handed the user the previous
# version's module, with no crash and nothing that looked stale.
echo "[dist-gate] an emission-input change re-keys the module cache..."
before_dir=$(dirname "$module")
for input in "$appdir"/usr/recomp/manifest/channel_sites.txt \
             "$appdir"/recomp/manifest/channel_sites.txt; do
    [[ -f "$input" ]] && { echo "# dist-gate upgrade probe" >> "$input"; break; }
done
"${APP[@]}" --headless --frame-limit 60 --run-recomp 0x80012204 \
      --max-runtime-seconds 900 > "$WORK/upgrade.log" 2>&1 \
    || { echo "[dist-gate] FAIL: boot after an emission-input change crashed" >&2;
         tail -20 "$WORK/upgrade.log" >&2; exit 2; }
after=$(ls "$DATA"/library/*/modules/*/spyro1-recomp.so 2>/dev/null | wc -l)
(( after >= 2 )) \
    || { echo "[dist-gate] FAIL: a manifest change did not re-key the module" \
              "cache — an upgrading user would keep running the previous" \
              "version's generated code" >&2; exit 2; }
grep -q "building the recomp module from your disc" "$WORK/upgrade.log" \
    || { echo "[dist-gate] FAIL: the module was re-keyed but no rebuild ran" >&2;
         exit 2; }

# Whatever the stamp cannot see must still not be a dead end: an
# unusable module at the ACTIVE path is rebuilt in place, not fatal.
echo "[dist-gate] an unusable module at the active path self-repairs..."
active=$(grep -o "shared recomp module attached: .*" "$WORK/upgrade.log" \
         | tail -1 | sed "s/.*attached: //")
[[ -f "$active" ]] || { echo "[dist-gate] FAIL: no active module path in the log" >&2; exit 2; }
printf '\177ELF not a recomp module' > "$active"
"${APP[@]}" --headless --frame-limit 60 --run-recomp 0x80012204 \
      --max-runtime-seconds 900 > "$WORK/repair.log" 2>&1 \
    || { echo "[dist-gate] FAIL: an unusable module was fatal instead of" \
              "being rebuilt" >&2; tail -20 "$WORK/repair.log" >&2; exit 2; }
grep -q "rebuilding it from your disc" "$WORK/repair.log" \
    || { echo "[dist-gate] FAIL: unusable module did not trigger a rebuild" >&2;
         exit 2; }

# The staged example is the drop-in: sdk/examples/ sits next to the
# binary (AppImage: usr/). Its presence with the docs site proves the
# --with-mod-sdk staging, and the compile proves the example.
sdkdir="$(dirname "$BIN")/sdk"
[[ -f "$sdkdir/examples/hello-anchor/mod.toml" ]] \
    || { echo "[dist-gate] FAIL: $sdkdir/examples/hello-anchor not staged (no mod.toml)" >&2; exit 2; }
[[ -f "$sdkdir/docs/index.html" ]] \
    || { echo "[dist-gate] FAIL: $sdkdir/docs/index.html not staged (mkdocs missing at package time?)" >&2; exit 2; }
echo "[dist-gate] drop-in source mod (from sdk/examples) compiles + loads on boot..."
mkdir -p "$DATA/mods"
cp -r "$sdkdir/examples/hello-anchor" "$DATA/mods/"
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
