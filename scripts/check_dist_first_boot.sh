#!/usr/bin/env bash
# check_dist_first_boot.sh - the distribution gate.
#
# Proves the SHIPPED artifact works: package it, put it somewhere clean,
# --ingest the reference disc, let first boot extract+recompile+link the
# module, then replay the full 20k hardware oracle against the dist exe +
# its self-built module. Finally, drop a source-only mod into the mods
# directory and prove a subsequent boot compiles + loads it.
# This is the closest thing to "a stranger
# downloaded it and it worked" that CI can check.
#
#   scripts/check_dist_first_boot.sh              # the .tar.xz artifact
#   scripts/check_dist_first_boot.sh --appimage   # the AppImage artifact
#   scripts/check_dist_first_boot.sh [--appimage] --game spyro-2
#   scripts/check_dist_first_boot.sh [--appimage] --all-shipped
#
# --game ID (or OPENPETE_E2E_GAME; default spyro-1) names the game the run
# ingests and replays. games/<game>/oracle.conf supplies the disc (DISC),
# the golden (GOLDEN) and the flags the golden's capture ran under
# (RUN_ARGS, e.g. --no-card for a cardless golden), which the replay
# passes. A missing default golden is captured with
# scripts/capture_psx_oracle_ds.sh --game <game>. --all-shipped packages
# once and runs every game of games/shipped.toml against that one
# artifact, then prints one verdict row per game.
#
# OPENPETE_DIST_ARTIFACT=<file> skips the packaging step and gates that
# already-built .tar.xz or .AppImage instead; with it set, no build tree is
# needed, so the gate runs on a second machine from a copy of this script,
# scripts/check_glibc_floor.sh, mods/r10-libc/, games/<game>/oracle.conf
# and tools/{gen_mod_docs,gen_mod_sdk,psyq_types}.py. OPENPETE_DISC and
# OPENPETE_TRACE name the disc and the golden (one game; --all-shipped
# refuses them); the golden's sidecars (<golden>.vsync, .loads, .srand)
# must sit beside it.
#
# Memory cards: every launch runs on a scratch HOME, so the default card
# directory (the per-user state dir openpete/cards) is scratch, and an
# inherited OPENPETE_CARD_DIR is unset; the oracle replay adds the
# recipe's RUN_ARGS. The gate asserts the first boot provisioned its cards
# inside the scratch layout.
#
# OPENPETE_DIST_MASK_HOST_DEV=1 runs every launch of the artifact with
# empty directories mounted over /usr/include and /usr/lib/gcc (bwrap, or
# an unprivileged unshare mount namespace when bwrap is absent): a dev host
# then has no libc headers, crt objects or libgcc, as on SteamOS, and the
# first-boot module build and the mod drop-in must use only what the
# artifact carries.
#
# The AppImage variant is the same chain against a different artifact,
# plus the three things only a read-only, path-varying mount can get
# wrong: nothing may be written inside the payload, the module cache must
# survive a mount path that changes every launch (otherwise every launch
# pays a 12 s+ recompile), and no driver/display library may be bundled.
#
# Both variants run against a scratch HOME/XDG, because a fresh install
# writes to the per-user directories (openpete_paths) - the gate must not
# read or write the developer's real ones.
#
# The glibc floor is asserted here too: a release-base artifact
# must not require symbols newer than the floor, or it fails to
# start on the distributions it claims to support, with a loader error the
# user cannot act on. Building on a dev box legitimately exceeds it - set
# OPENPETE_DIST_ALLOW_GLIBC_FLOOR=1 to downgrade that to a loud warning.
#
# Exit 0 = PASS; 2 = failure; 64 = prerequisites missing.
set -euo pipefail

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows
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
        echo "[dist-gate] no display in the environment - adopting $WAYLAND_DISPLAY"
        break
    done
    if [[ -z "${WAYLAND_DISPLAY:-}" && -S /tmp/.X11-unix/X0 ]]; then
        export DISPLAY=:0
        echo "[dist-gate] no display in the environment - adopting DISPLAY=:0"
    fi
fi

cd "$(dirname "$0")/.."

APPIMAGE=0
GAME="${OPENPETE_E2E_GAME:-spyro-1}"
ALL_SHIPPED=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --appimage) APPIMAGE=1; shift ;;
        --game) GAME="${2:?--game needs an id}"; shift 2 ;;
        --game=*) GAME="${1#--game=}"; shift ;;
        --all-shipped) ALL_SHIPPED=1; shift ;;
        *) echo "usage: $0 [--appimage] [--game ID | --all-shipped]" >&2; exit 64 ;;
    esac
done

# Debian 11 / bullseye, the release build base (packaging/linux-release.Dockerfile).
GLIBC_MAX=2.31
GLIBCXX_MAX=3.4.28

ARTIFACT="${OPENPETE_DIST_ARTIFACT:-}"

if [[ $ALL_SHIPPED -eq 1 ]]; then
    [[ -z "${OPENPETE_DISC:-}${OPENPETE_TRACE:-}" ]] \
        || { echo "[dist-gate] --all-shipped derives each game's disc and golden;" \
                  "unset OPENPETE_DISC and OPENPETE_TRACE" >&2; exit 64; }
    mapfile -t games < <(python3 -c '
import re
m = re.search(r"^games\s*=\s*\[([^\]]*)\]", open("games/shipped.toml").read(), re.M)
print("\n".join(re.findall(r"\"([^\"]+)\"", m.group(1)) if m else []))')
    [[ ${#games[@]} -gt 0 ]] || { echo "[dist-gate] games/shipped.toml lists no game" >&2; exit 64; }
    ALLWORK=$(mktemp -d "${TMPDIR:-/tmp}/openpete-dist-gate-all.XXXXXX")
    trap 'rm -rf "${ALLWORK:?}"' EXIT
    child=("$0")
    [[ $APPIMAGE -eq 1 ]] && child+=(--appimage)
    if [[ -z "$ARTIFACT" ]]; then
        # One package for every game: the children gate the same bytes.
        [[ -x build/linux-release-launcher/openpete ]] \
            || { echo "[dist-gate] build linux-release-launcher first" >&2; exit 64; }
        source scripts/lib/tools_dir.sh
        tc_src="$(openpete_tools_dir .)/clang"
        pkg_args=(linux --with-mod-sdk --toolchain "$tc_src" --out "$ALLWORK/dist")
        [[ $APPIMAGE -eq 1 ]] && pkg_args+=(--appimage)
        echo "[dist-gate] packaging once for ${games[*]}..."
        scripts/package-dist.sh "${pkg_args[@]}" >/dev/null
        ext=tar.xz; [[ $APPIMAGE -eq 1 ]] && ext=AppImage
        ARTIFACT=$(ls "$ALLWORK"/dist/*."$ext")
    fi
    rows=(); worst=0
    for g in "${games[@]}"; do
        echo "[dist-gate] ===== $g ====="
        set +e
        OPENPETE_DIST_ARTIFACT="$ARTIFACT" "${child[@]}" --game "$g" 2>&1 \
            | tee "$ALLWORK/$g.log"
        rc=${PIPESTATUS[0]}
        set -e
        verdict=$(grep -m1 -o "psxtrace: verdict.*" "$ALLWORK/$g.log" || echo "no verdict line")
        rows+=("$(printf '%-8s rc=%-3s %s' "$g" "$rc" "$verdict")")
        (( rc > worst )) && worst=$rc
    done
    echo "[dist-gate] ===== summary: $(basename "$ARTIFACT") ====="
    printf '[dist-gate]   %s\n' "${rows[@]}"
    exit "$worst"
fi

CONF="games/$GAME/oracle.conf"
[[ -f "$CONF" ]] || { echo "[dist-gate] no oracle recipe for $GAME: $CONF" >&2; exit 64; }
GOLDEN=""; DISC=""; RUN_ARGS=()
# shellcheck disable=SC1090
source "$CONF"
DISC="${OPENPETE_DISC:-$DISC}"
TRACE="${OPENPETE_TRACE:-$GOLDEN}"
# The module's file name: spyro-2 -> spyro2-recomp.so.
MODULE_SO="${GAME//-/}-recomp.so"
echo "[dist-gate] game $GAME: disc $DISC, golden $TRACE, replay flags: ${RUN_ARGS[*]:-none}"
[[ -f "$DISC" ]] || { echo "[dist-gate] disc missing: $DISC" >&2; exit 64; }
# The golden is generated, not committed: a missing default golden is
# captured as the oracle gate does; an explicit OPENPETE_TRACE never is.
if [[ ! -f "$TRACE" && "$TRACE" == "$GOLDEN" && -x scripts/capture_psx_oracle_ds.sh ]]; then
    [[ -x third_party/duckstation/build-regtest/bin/duckstation-regtest ]] \
        || { echo "[dist-gate] golden missing: $TRACE and DuckStation is not built" \
                  "(scripts/setup_duckstation.sh)" >&2; exit 64; }
    echo "[dist-gate] golden $TRACE missing: capturing it (scripts/capture_psx_oracle_ds.sh --game $GAME)"
    scripts/capture_psx_oracle_ds.sh --game "$GAME" \
        || { echo "[dist-gate] golden capture failed" >&2; exit 64; }
fi
[[ -f "$TRACE" ]] || { echo "[dist-gate] golden missing: $TRACE (tests/harness/check_psx_oracle.sh --game $GAME captures it)" >&2; exit 64; }
# The replay runs under the reference's schedule, read from the sidecars
# beside the golden; every recipe records these three. A golden copied
# without them replays unscheduled and diverges at the first load.
for _sc in vsync loads srand; do
    [[ -f "$TRACE.$_sc" ]] || { echo "[dist-gate] golden sidecar missing: $TRACE.$_sc (copy every $TRACE.* file with the golden)" >&2; exit 64; }
done
if [[ -n "$ARTIFACT" ]]; then
    [[ -f "$ARTIFACT" ]] || { echo "[dist-gate] artifact missing: $ARTIFACT" >&2; exit 64; }
    case "$ARTIFACT" in *.AppImage) APPIMAGE=1 ;; esac
else
    [[ -x build/linux-release-launcher/openpete ]] \
        || { echo "[dist-gate] build linux-release-launcher first" >&2; exit 64; }
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/openpete-dist-gate.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# A scratch per-user layout. Everything the artifact writes lands here, so
# the gate can inspect it and the developer's own library is untouched.
# The packaging step's Python keeps the developer's user site (tomli on a
# Python without tomllib); only the artifact sees the scratch home.
PYTHONUSERBASE="$(python3 -m site --user-base)"
export PYTHONUSERBASE
export HOME="$WORK/home"
export XDG_CONFIG_HOME="$HOME/.config"
export XDG_STATE_HOME="$HOME/.local/state"
export XDG_DATA_HOME="$HOME/.local/share"
# The cards must resolve inside the scratch layout, never a caller's.
unset OPENPETE_CARD_DIR
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
    # Releases ship the mod SDK and the fetched clang with its mod sysroot
    # (release-container.sh); the gate packages the same way.
    source scripts/lib/tools_dir.sh
    tc_src="$(openpete_tools_dir .)/clang"
    [[ -x "$tc_src/bin/clang" && -f "$tc_src/sysroot/include/stdio.h" ]] \
        || { echo "[dist-gate] no bundled toolchain at $tc_src (scripts/setup_linux_clang.sh)" >&2; exit 64; }
    pkg_args=(linux --with-mod-sdk --toolchain "$tc_src" --out "$WORK/dist")
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
    BIN="$appdir/usr/openpete"
    # Content manifest of the payload, to be re-checked after the run.
    ( cd "$appdir" && find . -type f -exec sha256sum {} + | sort -k2 ) \
        > "$WORK/payload-before.txt"
else
    archive=$(ls "$WORK"/dist/*.tar.xz)
    echo "[dist-gate] extracting $(basename "$archive") into a clean dir..."
    mkdir -p "$WORK/install"
    tar xJf "$archive" -C "$WORK/install"
    appdir=$(ls -d "$WORK"/install/*/)
    APP=("$appdir/openpete")
    BIN="$appdir/openpete"
fi

if [[ -n "${OPENPETE_DIST_MASK_HOST_DEV:-}" ]]; then
    if command -v bwrap >/dev/null; then
        MASK=(bwrap --dev-bind / / --tmpfs /usr/include --tmpfs /usr/lib/gcc)
    else
        MASK=(unshare -rm sh -c 'mount -t tmpfs none /usr/include &&
              mount -t tmpfs none /usr/lib/gcc && exec "$@"' sh)
    fi
    # The mask must engage: both directories read empty through it.
    "${MASK[@]}" sh -c '[ -z "$(ls -A /usr/include)$(ls -A /usr/lib/gcc)" ]' \
        || { echo "[dist-gate] FAIL: ${MASK[0]} did not mask /usr/include" \
                  "and /usr/lib/gcc" >&2; exit 2; }
    APP=("${MASK[@]}" "${APP[@]}")
    echo "[dist-gate] host development files masked by ${MASK[0]}:" \
         "/usr/include and /usr/lib/gcc read empty"
fi

echo "[dist-gate] glibc floor (every ELF in the payload)..."
floor_ok=1
GLIBC_MAX=$GLIBC_MAX GLIBCXX_MAX=$GLIBCXX_MAX scripts/check_glibc_floor.sh "$appdir" \
    | sed "s|$appdir|<payload>/|; s|^|[dist-gate]   |" || floor_ok=0
if [[ $floor_ok -eq 0 ]]; then
    if [[ -n "${OPENPETE_DIST_ALLOW_GLIBC_FLOOR:-}" ]]; then
        echo "[dist-gate]   WARNING: floor exceeded - this artifact will NOT" \
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

# The account's own card directory, stamped before the first boot.
real_card_stamp() {
    local d; d="$(getent passwd "$(id -un)" | cut -d: -f6)/.local/state/openpete/cards"
    stat -c '%n %Y %s' "$d"/* 2>/dev/null | sort
}
REAL_CARD_STAMP="$(real_card_stamp)"
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
                  "empty PATH - the image is not self-contained" >&2;
             tail -20 "$WORK/first-boot.log" >&2; exit 2; }
else
    "${APP[@]}" --headless --frame-limit 300 \
          --ingest "$DISC_ABS" --max-runtime-seconds 900 \
          > "$WORK/first-boot.log" 2>&1 \
        || { echo "[dist-gate] FAIL: first boot did not complete" >&2;
             tail -20 "$WORK/first-boot.log" >&2; exit 2; }
fi
# Every shipped artifact bundles the compiler; a mod built by the host cc
# links the host's libc and would pass here while a player's host has none.
grep -q "cc=.*recomp/tools/clang/bin/clang" "$WORK/first-boot.log" \
    || { echo "[dist-gate] FAIL: first boot did not use the bundled" \
              "compiler:" "$(grep -o 'cc=[^ ]*' "$WORK/first-boot.log" | head -1)" >&2;
         exit 2; }

# A GUI launch has no terminal, so the log file is the only thing a user
# can attach to a report. Assert the first boot - the run most likely to
# go wrong on a stranger's machine - actually left one, and that it
# carries the build it did rather than an empty shell.
LOGF="$XDG_STATE_HOME/openpete/logs/openpete.log"
[[ -s "$LOGF" ]] \
    || { echo "[dist-gate] FAIL: first boot wrote no log file at $LOGF" >&2;
         exit 2; }
grep -q "first-boot module build" "$LOGF" \
    || { echo "[dist-gate] FAIL: the log file did not capture the module" \
              "build it was open for" >&2; exit 2; }
echo "[dist-gate] first boot left a log file ($(wc -c < "$LOGF") bytes)..."
# The default card directory of the user layout is the per-user state dir,
# inside the scratch home. A card is created at the game's first card access,
# which Spyro 1's first boot does not reach and Spyro 2 and 3 do at the title,
# so the check is where cards went, not whether they exist: any card the first
# boot made is in the scratch layout, and the account's own card directory is
# unchanged (REAL_CARD_STAMP, taken before the first boot).
CARDS="$XDG_STATE_HOME/openpete/cards"
if [[ -s "$CARDS/card1.mcr" || -s "$CARDS/card2.mcr" ]]; then
    echo "[dist-gate] first boot provisioned its cards in the scratch layout"
else
    echo "[dist-gate] first boot opened no card (none in $CARDS)"
fi
[[ "$(real_card_stamp)" == "$REAL_CARD_STAMP" ]] \
    || { echo "[dist-gate] FAIL: the account's card directory changed during the first boot" >&2;
         exit 2; }

module=$(ls "$DATA"/library/*/modules/*/"$MODULE_SO" 2>/dev/null | head -1)
[[ -n "$module" ]] || { echo "[dist-gate] FAIL: no module built under $DATA" >&2;
                        tail -20 "$WORK/first-boot.log" >&2; exit 2; }
grep -q "first-boot module build" "$WORK/first-boot.log" \
    || { echo "[dist-gate] FAIL: first boot did not build a module" >&2; exit 2; }
# The ingest writes the game's launcher asset cache (domes, glyph models,
# logo and its manifest) beside the build; the warm boot's no-rerun check
# below is vacuous without it.
grep -q "asset cache of $GAME written" "$WORK/first-boot.log" \
    || { echo "[dist-gate] FAIL: first boot wrote no launcher asset cache" >&2; exit 2; }
# The module must not have acquired a shared-library dependency it cannot
# resolve: it is linked -nostdlib and binds everything else against the
# already-loaded exe at dlopen.
needed=$(objdump -p "$module" | awk '/NEEDED/ {print $2}' || true)
[[ -z "$needed" ]] || { echo "[dist-gate] FAIL: module carries DT_NEEDED: $needed" >&2; exit 2; }

echo "[dist-gate] full 20k oracle on the dist exe + self-built module..."
"${APP[@]}" --headless \
      --psxtrace-verify "$TRACE_ABS" \
      --frame-limit 200000 "${RUN_ARGS[@]}" > "$WORK/oracle.log" 2>&1 \
    || { echo "[dist-gate] FAIL: oracle divergence or crash" >&2;
         tail -20 "$WORK/oracle.log" >&2; exit 2; }
# The verdict line is printed here because $WORK is removed on exit, and a
# zero exit alone does not show that the replay reached its verdict.
grep -E "psxtrace: (summary|verdict)" "$WORK/oracle.log" | tail -2 | sed 's/^/[dist-gate]   /'
grep -q "verdict: PASS" "$WORK/oracle.log" \
    || { echo "[dist-gate] FAIL: the oracle printed no 'verdict: PASS'" >&2;
         tail -20 "$WORK/oracle.log" >&2; exit 2; }

if [[ $APPIMAGE -eq 1 ]]; then
    echo "[dist-gate] warm boot re-attaches (module cache is mount-independent)..."
    module_stamp_before=$(stat -c %Y "$module")
    "${APP[@]}" --headless --frame-limit 60 --max-runtime-seconds 300 \
          > "$WORK/warm-boot.log" 2>&1 \
        || { echo "[dist-gate] FAIL: warm boot did not complete" >&2;
             tail -20 "$WORK/warm-boot.log" >&2; exit 2; }
    ! grep -q "first-boot module build" "$WORK/warm-boot.log" \
        || { echo "[dist-gate] FAIL: warm boot rebuilt the module - the cache" \
                  "key is not independent of the mount path" >&2; exit 2; }
    [[ "$(stat -c %Y "$module")" == "$module_stamp_before" ]] \
        || { echo "[dist-gate] FAIL: warm boot rewrote the cached module" >&2; exit 2; }
    # The asset cache records the build id (version, executable size and
    # modification time); a key that took the mount path would rerun the
    # extraction at every launch.
    ! grep -q "launch-time rerun" "$WORK/warm-boot.log" \
        || { echo "[dist-gate] FAIL: warm boot reran the asset extraction:" \
                  "$(grep -m1 "launch-time rerun" "$WORK/warm-boot.log")" >&2; exit 2; }
    grep -q "asset cache of $GAME is current" "$WORK/warm-boot.log" \
        || { echo "[dist-gate] FAIL: warm boot did not check the asset cache" >&2; exit 2; }

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
# change (a new funcs list, a channel-site update - the most common kind
# of change between releases) silently handed the user the previous
# version's module, with no crash and nothing that looked stale.
echo "[dist-gate] an emission-input change re-keys the module cache..."
before_dir=$(dirname "$module")
probed=""
for input in "$appdir"/usr/recomp/manifest/"$GAME"/manifest.toml \
             "$appdir"/recomp/manifest/"$GAME"/manifest.toml; do
    [[ -f "$input" ]] && { echo "# dist-gate upgrade probe" >> "$input"; probed=$input; break; }
done
[[ -n "$probed" ]] || { echo "[dist-gate] FAIL: no recomp/manifest/$GAME/manifest.toml in the payload" >&2; exit 2; }
"${APP[@]}" --headless --frame-limit 60 \
      --max-runtime-seconds 900 > "$WORK/upgrade.log" 2>&1 \
    || { echo "[dist-gate] FAIL: boot after an emission-input change crashed" >&2;
         tail -20 "$WORK/upgrade.log" >&2; exit 2; }
after=$(ls "$DATA"/library/*/modules/*/"$MODULE_SO" 2>/dev/null | wc -l)
(( after >= 2 )) \
    || { echo "[dist-gate] FAIL: a manifest change did not re-key the module" \
              "cache - an upgrading user would keep running the previous" \
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
"${APP[@]}" --headless --frame-limit 60 \
      --max-runtime-seconds 900 > "$WORK/repair.log" 2>&1 \
    || { echo "[dist-gate] FAIL: an unusable module was fatal instead of" \
              "being rebuilt" >&2; tail -20 "$WORK/repair.log" >&2; exit 2; }
grep -q "rebuilding it from your disc" "$WORK/repair.log" \
    || { echo "[dist-gate] FAIL: unusable module did not trigger a rebuild" >&2;
         exit 2; }

# The staged example is the drop-in: sdk/examples/ sits next to the
# binary (AppImage: usr/), as does the documentation site docs/. The
# compile writes the object for the running game, .build/<game>/. The
# example proves the --with-mod-sdk staging, the site's Modding section
# the docs staging every package carries, and the compile the example.
sdkdir="$(dirname "$BIN")/sdk"
docsdir="$(dirname "$BIN")/docs"
[[ -f "$sdkdir/examples/hello-anchor/mod.toml" ]] \
    || { echo "[dist-gate] FAIL: $sdkdir/examples/hello-anchor not staged (no mod.toml)" >&2; exit 2; }
[[ -f "$docsdir/index.html" ]] \
    || { echo "[dist-gate] FAIL: $docsdir/index.html not staged" >&2; exit 2; }
[[ -f "$docsdir/modding/index.html" ]] \
    || { echo "[dist-gate] FAIL: $docsdir/modding/index.html not staged (Modding section missing from the package)" >&2; exit 2; }
# Every staged example and the libc fixture mods/r10-libc (string.h,
# stdio.h, math.h, stdlib.h, stdatomic.h, an __int128 division) are dropped
# in together and built by the next boot. A mod whose manifest selects the
# running game (the loader's rule, tools/gen_mod_docs.py mod_games): the
# object exists (a mod with no src/ is declarative and builds none), its
# compile.log is empty, the boot logs `loaded <id>`, and the object names
# no DT_NEEDED (libc binds at dlopen from the process). A mod for another
# game: the loader logs it skipped, and it builds and loads nothing.
fixture=mods/r10-libc
[[ -f "$fixture/mod.toml" ]] \
    || { echo "[dist-gate] FAIL: $fixture missing beside this script" >&2; exit 2; }
echo "[dist-gate] drop-in: every sdk example + r10-libc compile + load on boot..."
mkdir -p "$DATA/mods"
dropins=(); others=()
for d in "$sdkdir"/examples/*/ "$fixture"/; do
    [[ -f "$d/mod.toml" ]] || continue
    id=$(basename "$d")
    cp -r "$d" "$DATA/mods/"
    rm -rf "${DATA:?}/mods/$id/.build"
    sel=0
    python3 -c '
import sys
sys.path.insert(0, "tools")
from gen_mod_docs import mod_games
sys.exit(0 if sys.argv[2] in mod_games(open(sys.argv[1], encoding="utf-8").read()) else 3)
' "$d/mod.toml" "$GAME" || sel=$?
    case $sel in
        0) dropins+=("$id") ;;
        3) others+=("$id") ;;
        *) echo "[dist-gate] FAIL: cannot read the games of $d/mod.toml" >&2; exit 2 ;;
    esac
done
"${APP[@]}" --headless --frame-limit 300 \
 --max-runtime-seconds 600 \
      > "$WORK/mod-boot.log" 2>&1 \
    || { echo "[dist-gate] FAIL: boot with drop-in mods crashed" >&2;
         tail -20 "$WORK/mod-boot.log" >&2; exit 2; }
drop_fail=0
printf '[dist-gate]   %-20s %-4s %-12s %-7s %s\n' mod .so compile.log loaded NEEDED
for id in "${dropins[@]}"; do
    so="$DATA/mods/$id/.build/$GAME/$id.so"
    clog="$DATA/mods/$id/.build/$GAME/compile.log"
    if [[ -d "$DATA/mods/$id/src" ]]; then
        so_ok=no; [[ -f "$so" ]] && so_ok=yes
        log_ok=empty; [[ -s "$clog" ]] && log_ok="$(wc -c < "$clog")B"
        needed=""
        if [[ -f "$so" ]]; then
            needed=$(readelf -d "$so" 2>/dev/null \
                     | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' | tr '\n' ' ')
        fi
        [[ -f "$so" && "$log_ok" == empty && -z "$needed" ]] || drop_fail=1
    else
        so_ok=n/a; log_ok=n/a; needed=""
    fi
    loaded=no; grep -q "loaded $id v" "$WORK/mod-boot.log" && loaded=yes
    [[ $loaded == yes ]] || drop_fail=1
    printf '[dist-gate]   %-20s %-4s %-12s %-7s %s\n' "$id" "$so_ok" "$log_ok" "$loaded" "${needed:--}"
done
for id in "${others[@]}"; do
    skipped=no
    grep -q "$id targets .* - skipped (this is $GAME)" "$WORK/mod-boot.log" && skipped=yes
    built=no; [[ -e "$DATA/mods/$id/.build/$GAME" ]] && built=yes
    loaded=no; grep -q "loaded $id v" "$WORK/mod-boot.log" && loaded=yes
    [[ $skipped == yes && $built == no && $loaded == no ]] || drop_fail=1
    printf '[dist-gate]   %-20s other game: skipped=%s built=%s loaded=%s\n' "$id" "$skipped" "$built" "$loaded"
done
if [[ $drop_fail -ne 0 ]]; then
    echo "[dist-gate] FAIL: a drop-in mod did not compile clean, load, or" \
         "link without DT_NEEDED" >&2
    for id in "${dropins[@]}"; do
        clog="$DATA/mods/$id/.build/$GAME/compile.log"
        [[ -s "$clog" ]] && { echo "[dist-gate]   $id compile.log:" >&2; head -3 "$clog" >&2; }
    done
    exit 2
fi

# The packaged SDK carries sdk/games/<g>/ for every game the package
# ships (package-dist.sh stages the shipped games' trees, the games whose
# recomp/manifest/<g>/ it carries) and none other. The root dispatchers
# select a tree by the OPENPETE_GAME_* defines: per game, a line with
# -I sdk alone compiles when the tree is packaged (no OPENPETE_GAME_ID
# selects Spyro 1) and stops at the dispatcher's #error naming the game
# when it is not.
echo "[dist-gate] packaged SDK: per-game trees match the shipped games, root dispatchers select each..."
# A host with no compiler (SteamOS) checks with the artifact's own clang
# and its sysroot headers instead.
tc="$(dirname "$BIN")/recomp/tools/clang"
if command -v cc >/dev/null; then DCC=(cc)
else DCC=("$tc/bin/clang" -nostdlibinc -isystem "$tc/sysroot/include"); fi
printf '#include <openpete_mod_api.h>\n#include <openpete_sdk_symbols.h>\n#include <openpete_sdk_wrappers.h>\n#include <openpete_game_structs.h>\n' \
    > "$WORK/dispatch.c"
shipped_games=$(ls "$(dirname "$BIN")/recomp/manifest" | sort | tr '\n' ' ')
sdk_games=$(ls "$sdkdir/games" | sort | tr '\n' ' ')
[[ "$sdk_games" == "$shipped_games" ]] \
    || { echo "[dist-gate] FAIL: sdk/games/ carries [$sdk_games], the package ships [$shipped_games]" >&2; exit 2; }
[[ " $shipped_games" == *" $GAME "* ]] \
    || { echo "[dist-gate] FAIL: the package does not ship $GAME (ships $shipped_games)" >&2; exit 2; }
for n in 1 2 3; do
    g="spyro-$n"
    defs=()
    [[ $n -ne 1 ]] && defs=(-DOPENPETE_GAME_ID="\"$g\"" -DOPENPETE_GAME_SPYRO1=0
                            -DOPENPETE_GAME_SPYRO2=$((n == 2)) -DOPENPETE_GAME_SPYRO3=$((n == 3)))
    if "${DCC[@]}" -fsyntax-only -Wall -Wundef "${defs[@]}" -I "$sdkdir" "$WORK/dispatch.c" \
          > "$WORK/dispatch-$g.log" 2>&1; then
        got=compiles
    elif grep -qF "has no Spyro $n headers (sdk/games/$g/ is absent)" "$WORK/dispatch-$g.log"; then
        got=refused
    else
        echo "[dist-gate] FAIL: a $g line against the packaged SDK failed without the dispatcher #error" >&2
        head -5 "$WORK/dispatch-$g.log" >&2; exit 2
    fi
    want=refused; [[ -d "$sdkdir/games/$g" ]] && want=compiles
    echo "[dist-gate]   $g: $got"
    [[ $got == "$want" ]] \
        || { echo "[dist-gate] FAIL: a $g line against the packaged SDK $got; expected $want" >&2;
             head -5 "$WORK/dispatch-$g.log" >&2; exit 2; }
done

what=$([[ $APPIMAGE -eq 1 ]] && echo AppImage || echo tarball)
echo "[dist-gate] PASS - $what, $GAME: unpack -> ingest -> self-build -> oracle-clean -> mod drop-in (${#dropins[@]} mods, ${#others[@]} for other games skipped)"
