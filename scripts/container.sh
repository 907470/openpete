#!/usr/bin/env bash
# scripts/container.sh - build the decompilations, the games, the engine and
# the Windows exe inside containers, from a clone with no compiler.
#
#   scripts/container.sh engine            core libraries + unit tests
#   scripts/container.sh decomp <game>     the game's decompilation build
#   scripts/container.sh game <game> [--oracle]
#                                          recompile, build, boot check;
#                                          --oracle adds the 20k oracle gate
#   scripts/container.sh windows <game>    the MinGW cross build of <game>
#   scripts/container.sh shell [cmd...]    a shell (or cmd) in the developer
#                                          image, set up for commits
#
# <game> is spyro-1, spyro-2 or spyro-3. The forms run in the developer
# image (packaging/dev-image.Dockerfile; `windows` in its mingw stage) or,
# for `decomp`, in the image of the decompilation's own Dockerfile. Each
# image is built on first use, tagged with a hash of its recipe files.
#
# Inputs the user supplies (none is distributed with the project):
#   OPENPETE_PSYQ_INCLUDE  directory of the PSY-Q headers (4.0 for spyro-1,
#                          4.7 for spyro-2); `decomp` copies its files into
#                          games/<game>/<game>/psyq/include, keeping files
#                          already there (Spyro 3 tracks its one header)
#   OPENPETE_DISC_DIR      directory holding the game's .cue/.bin (default
#                          games/<game>/<game>/build, beside the decomp
#                          outputs); mounted read-only; the cue's name is the
#                          basename of DISC in games/<game>/oracle.conf
#   OPENPETE_BIOS          the SCPH-1001 BIOS for --oracle (default
#                          .tools/pcsx-redux/scph1001.bin); mounted read-only
#                          at that path, the one setup_duckstation.sh reads
# Knobs:
#   OPENPETE_CONTAINER_ENGINE  podman or docker (default: podman, then docker)
#   OPENPETE_CONTAINER_JOBS    build parallelism (default nproc)
#
# Network: the image builds, scripts/setup_duckstation.sh (submodule init,
# dependency download, build) and the `shell` form run with the default
# network; every configure, build, regen, test and run step runs under
# --network=none.
#
# Builds go to build/container/<preset>, configured with
# `cmake --preset <preset> -B build/container/<preset>` and built and tested
# by directory: the presets fix binaryDir to build/<preset>, which stays the
# host's. The basename keeps the linux-release fence checks running; CMake
# hands them this directory's compile database. HOME and the XDG
# directories sit under build/container/home (sandbox-guard: intentional
# the container's HOME and XDG directories under build/container/home are its sandbox). Every form but `shell` sets
# OPENPETE_NO_COMMITS=1 (no hook arming needed) and every run sets
# OPENPETE_VIBRATION=0. Runs are headless.
#
# A commit made inside the container: `scripts/container.sh shell` after a
# configure of every game the commit stages (`game <g>` for each, or
# `engine` for a core-only commit); the shell exports the two compile
# database variables, passes the host's git identity as GIT_AUTHOR_* and
# GIT_COMMITTER_* and runs scripts/setup.sh once (it arms the hooks).
#
# Timing window: every image build and container run first waits out
# another job's timing window (scripts/lib/timing_gate.sh); with
# OPENPETE_TIMING_GATE_TIMEOUT=<seconds> set it gives up at the limit and the
# script exits 75 without launching. The host's timing-window directory is
# mounted into every container (ctr_timing_env), so a gated launch inside it
# (the oracle's sandboxed runs) waits on the same window.
#
# Exit: 0 pass; 1 a step or check failed; 64 usage or a missing input,
# named on stderr; 75 the timing gate's timeout.
#
# The OPENPETE_* variables go to Linux processes in the container with -e;
# the Windows exe is linked and its import table read, never launched -
# wslenv-guard: no-launch.
set -euo pipefail
cd "$(dirname "$0")/.."
TREE="$PWD"
# shellcheck source=scripts/lib/container.sh
source scripts/lib/container.sh
# shellcheck source=scripts/lib/timing_gate.sh
source scripts/lib/timing_gate.sh

usage() {
    sed -n '2,/^set -euo/s/^# \{0,1\}//p' "$0" | sed '$d' >&2
    exit 64
}
refuse() { echo "[container] refused: $*" >&2; exit 64; }
fail() { echo "[container] FAIL: $*" >&2; exit 1; }
say() { echo "[container] $*"; }
# Wait out another job's timing window before a build or launch; exit with
# the gate's status (75) when OPENPETE_TIMING_GATE_TIMEOUT is reached.
timing_gate() { op_timing_gate "container $form${game:+ $game}" >&2 || exit $?; }

form="${1:-}"; shift || true
game="" oracle=0
case "$form" in
    engine) [[ $# -eq 0 ]] || usage ;;
    decomp|windows)
        game="${1:-}"; shift || true
        [[ $# -eq 0 ]] || usage ;;
    game)
        game="${1:-}"; shift || true
        if [[ "${1:-}" == --oracle ]]; then oracle=1; shift; fi
        [[ $# -eq 0 ]] || usage ;;
    shell) ;;
    -h|--help|"") usage ;;
    *) usage ;;
esac
if [[ -n "$form" && "$form" != engine && "$form" != shell ]]; then
    case "$game" in
        spyro-1|spyro-2|spyro-3) ;;
        *) refuse "unknown game '${game}' (spyro-1, spyro-2 or spyro-3)" ;;
    esac
fi

ctr_engine container
ctr_uidmap
JOBS="${OPENPETE_CONTAINER_JOBS:-$(nproc)}"
say "engine: $(ctr_engine_mode)"

BUILD="$TREE/build/container"
DEV_RECIPE=(packaging/dev-image.Dockerfile packaging/dev-image.sh packaging/dev-image-packages.txt)
DEV_HASH="$(cat "${DEV_RECIPE[@]}" | sha256sum | cut -c1-12)"
DEV_IMAGE="openpete-dev:$DEV_HASH"
MINGW_IMAGE="openpete-dev-mingw:$DEV_HASH"

# The developer image and, for `windows`, its mingw stage. The context is a
# directory holding the stage script and the package list alone.
dev_images() {
    local want_mingw="$1" ctx
    if ctr_image_exists "$DEV_IMAGE" && { (( ! want_mingw )) || ctr_image_exists "$MINGW_IMAGE"; }; then
        return
    fi
    timing_gate
    ctx="$(mktemp -d)"
    cp packaging/dev-image.sh packaging/dev-image-packages.txt "$ctx/"
    local t0=$SECONDS
    if ! ctr_image_exists "$DEV_IMAGE"; then
        say "building $DEV_IMAGE"
        ctr_build_image packaging/dev-image.Dockerfile "$ctx" "$DEV_IMAGE" developer
    fi
    if (( want_mingw )) && ! ctr_image_exists "$MINGW_IMAGE"; then
        say "building $MINGW_IMAGE"
        ctr_build_image packaging/dev-image.Dockerfile "$ctx" "$MINGW_IMAGE" mingw "developer=$DEV_IMAGE"
    fi
    rm -rf "$ctx"
    say "image build: $((SECONDS - t0)) s"
}

# Mounts and environment of every developer-image run.
ctr_tree_mounts "$TREE"
mkdir -p "$BUILD"
ctr_home_env "$BUILD/home" "$BUILD/home"
ctr_timing_env
CTR_ENV+=(-e OPENPETE_VIBRATION=0
          -e "OPENPETE_COMPLEXITY_DB=$BUILD/linux-release/compile_commands.json"
          -e "OPENPETE_DOCS_DB=$BUILD/linux-release/compile_commands.json"
          -e "OPENPETE_PSXRECOMP=$TREE/third_party/psxrecomp/build/PSXRecomp")
[[ "$form" != shell ]] && CTR_ENV+=(-e OPENPETE_NO_COMMITS=1)

# in_image IMAGE NETWORK CMD... - run a bash script in IMAGE at the tree.
in_image() {
    local image="$1" net="$2"; shift 2
    local netopt=()
    [[ "$net" == none ]] && netopt=(--network=none)
    timing_gate
    "$CTR_ENGINE" "${CTR_ENGINE_ARGS[@]}" run --rm "${netopt[@]}" "${CTR_MOUNTS[@]}" "${CTR_ENV[@]}" \
        "${CTR_UIDMAP[@]}" -w "$TREE" "$image" bash -euo pipefail -c "$@"
}

# Refuse when a configure-input submodule is not checked out.
check_submodules() {
    local p
    for p in third_party/SDL third_party/glslang third_party/imgui third_party/PsyCross; do
        [[ -n "$(ls -A "$p" 2>/dev/null)" ]] \
            || refuse "submodule $p is empty; run: git submodule update --init --recursive -- $p"
    done
}

# The game's disc: sets DISC_DIR and CUE, mounts the directory read-only.
resolve_disc() {
    local disc
    disc="$(sed -n 's/^DISC="\(.*\)"$/\1/p' "games/$game/oracle.conf")"
    [[ -n "$disc" ]] || fail "games/$game/oracle.conf names no DISC"
    DISC_DIR="${OPENPETE_DISC_DIR:-games/$game/$game/build}"
    [[ "$DISC_DIR" == /* ]] || DISC_DIR="$TREE/$DISC_DIR"
    [[ -d "$DISC_DIR" ]] \
        || refuse "disc directory $DISC_DIR is absent; place the $(basename "$disc") image of your own disc there or set OPENPETE_DISC_DIR"
    CUE="$DISC_DIR/$(basename "$disc")"
    [[ -f "$CUE" ]] \
        || refuse "disc directory $DISC_DIR holds no $(basename "$CUE"); place your own disc image there or set OPENPETE_DISC_DIR"
    ctr_mount "$DISC_DIR" "$DISC_DIR" ro
}

# boot_check EXE TAG [ARGS...]: --frame-limit 600 (1173 on Spyro 2 and 3, whose
# feed key counts the boot's 573 fast-boot VBLs) exits 0 and prints the
# frame-limit exit line; the hang watchdog's 2 and every other code fail.
# The log is build/container/boot-<game>-<TAG>.log.
boot_check() {
    local exe="$1" tag="$2"
    shift 2
    local log="$BUILD/boot-$game-$tag.log"
    local run_args=() code=0 limit=600
    case "$game" in spyro-2|spyro-3) limit=1173;; esac
    # The game's own boot flags (RUN_ARGS of its oracle recipe).
    read -ra run_args <<<"$(bash -c 'source "$1"; echo "${RUN_ARGS[@]}"' _ "games/$game/oracle.conf")"
    rm -f "$log"
    in_image "$DEV_IMAGE" none "\"\$@\" >$(printf %q "$log") 2>&1" _ \
        "$exe" --headless --no-config --disc "$CUE" "${run_args[@]}" "$@" --frame-limit "$limit" \
        || code=$?
    [[ -f "$log" ]] || fail "boot check wrote no log $log"
    local line
    line="$(grep -m1 '^openpete-exit:' "$log" || true)"
    say "boot check $exe: exit $code, ${line:-no openpete-exit line}"
    (( code == 0 )) || fail "boot check exited $code (log $log)"
    [[ "$line" == "openpete-exit: reason=frame-limit"* ]] \
        || fail "boot check did not reach the frame limit (log $log)"
}

form_engine() {
    check_submodules
    dev_images 0
    local t0=$SECONDS
    in_image "$DEV_IMAGE" none "
        cmake --preset linux-release -B '$BUILD/linux-release' -DOPENPETE_GAMES=
        cmake --build '$BUILD/linux-release' -j $JOBS
        ctest --test-dir '$BUILD/linux-release' -R _test --no-tests=error --output-on-failure -j $JOBS"
    say "engine: PASS ($((SECONDS - t0)) s)"
}

form_decomp() {
    local dir="games/$game/$game"
    [[ -f "$dir/Dockerfile" ]] || refuse "$dir is not checked out; run: git submodule update --init --recursive -- $dir"
    [[ -n "$(ls -A "$dir/tools/maspsx" 2>/dev/null)" ]] \
        || refuse "$dir/tools/maspsx is empty (the build would report success with empty objects); run: git submodule update --init --recursive -- $dir"
    mkdir -p "$dir/psyq/include"
    if [[ -n "${OPENPETE_PSYQ_INCLUDE:-}" ]]; then
        [[ -d "$OPENPETE_PSYQ_INCLUDE" ]] || refuse "OPENPETE_PSYQ_INCLUDE=$OPENPETE_PSYQ_INCLUDE is not a directory"
        cp -rn "$OPENPETE_PSYQ_INCLUDE/." "$dir/psyq/include/"
        say "PSY-Q headers copied from $OPENPETE_PSYQ_INCLUDE into $dir/psyq/include"
    fi
    if [[ "$game" != spyro-3 ]] && ! find "$dir/psyq/include" -name '*.h' -print -quit | grep -q .; then
        local v=4.0; [[ "$game" == spyro-2 ]] && v=4.7
        refuse "$dir/psyq/include holds no PSY-Q header; set OPENPETE_PSYQ_INCLUDE to your own PSY-Q $v include directory"
    fi
    local hash image ctx t0=$SECONDS
    hash="$(sha256sum "$dir/Dockerfile" | cut -c1-12)"
    image="openpete-decomp-$game:$hash"
    if ! ctr_image_exists "$image"; then
        say "building $image"
        timing_gate
        ctx="$(mktemp -d)"
        ctr_build_image "$dir/Dockerfile" "$ctx" "$image" ""
        rm -rf "$ctx"
        say "image build: $((SECONDS - t0)) s"
    fi
    local build_cmd="make all -j $JOBS"
    # Spyro 3's build only prints a warning on a hash mismatch, so the hashes
    # are checked after it. Its sha256sum.txt spells 19 overlay paths
    # `build.wad/`; the check reads a copy with them as `build/wad/`, where
    # the build writes every overlay.
    [[ "$game" == spyro-3 ]] && build_cmd="python3 ./build_multiproc.py
        sed 's#^\([0-9A-Fa-f]* \)build\.wad/#\1build/wad/#' sha256sum.txt >/tmp/sha256sum.txt
        sha256sum -c /tmp/sha256sum.txt"
    t0=$SECONDS
    timing_gate
    "$CTR_ENGINE" "${CTR_ENGINE_ARGS[@]}" run --rm --network=none "${CTR_MOUNTS[@]}" "${CTR_ENV[@]}" "${CTR_UIDMAP[@]}" \
        -w "$TREE/$dir" "$image" bash -euo pipefail -c "$build_cmd" \
        || fail "decomp $game build or hash check failed"
    local f
    for f in build/PSX.EXE build/psx.map; do
        [[ -s "$dir/$f" ]] || fail "decomp $game wrote no $dir/$f"
    done
    say "decomp $game: PASS ($((SECONDS - t0)) s)"
}

form_game() {
    check_submodules
    [[ -f "games/$game/$game/build/psx.map" ]] \
        || refuse "games/$game/$game/build/psx.map is absent; run: scripts/container.sh decomp $game"
    resolve_disc
    if (( oracle )); then
        local bios="${OPENPETE_BIOS:-$TREE/.tools/pcsx-redux/scph1001.bin}"
        [[ -f "$bios" ]] || refuse "no SCPH-1001 BIOS at $bios; set OPENPETE_BIOS to your own copy"
        local bios_target="$TREE/.tools/pcsx-redux/scph1001.bin"
        if ! [[ "$bios" -ef "$bios_target" ]]; then
            # The mount point is created here as the host user and removed on
            # exit: an empty file left at the path would be copied as the
            # BIOS by a later setup_duckstation.sh run.
            if [[ ! -e "$bios_target" ]]; then
                mkdir -p "$(dirname "$bios_target")"
                : >"$bios_target"
                trap 'rm -f "$TREE/.tools/pcsx-redux/scph1001.bin"' EXIT
            fi
            ctr_mount "$bios" "$bios_target" ro
        fi
    fi
    dev_images 0
    local t0=$SECONDS exe
    in_image "$DEV_IMAGE" none "
        if [[ ! -x third_party/psxrecomp/build/PSXRecomp ]]; then
            cmake -S third_party/psxrecomp/recompiler -B third_party/psxrecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
            cmake --build third_party/psxrecomp/build -j $JOBS
        fi
        GAME=$game bash scripts/regen-manifest.sh
        GAME=$game bash scripts/regen-recomp.sh
        cmake --preset linux-release -B '$BUILD/linux-release' -DOPENPETE_GAMES=$game
        cmake --build '$BUILD/linux-release' -j $JOBS"
    say "game $game build: $((SECONDS - t0)) s"
    exe="$BUILD/linux-release/openpete"
    boot_check "$exe" linux-release
    if [[ "$game" == spyro-1 ]]; then
        t0=$SECONDS
        in_image "$DEV_IMAGE" none "
            cmake --preset linux-release-launcher -B '$BUILD/linux-release-launcher' -DOPENPETE_GAMES=$game
            cmake --build '$BUILD/linux-release-launcher' -j $JOBS"
        say "game $game shared build: $((SECONDS - t0)) s"
        # A copy of the exe in a directory with no module beside it builds
        # the module from the disc (extract, PSXRecomp, the image's cc, link)
        # before it boots, as on a first boot; beside the CMake-built module
        # the exe would load that one instead.
        local fb="$BUILD/linux-release-launcher/first-boot"
        local module="$fb/spyro1-recomp.so"
        rm -rf "$fb"
        mkdir -p "$fb"
        # recomp/ holds the inputs the build stages beside the exe (manifest,
        # module sources; tools/ empty, so find_cc takes the image's cc and
        # OPENPETE_PSXRECOMP names the recompiler).
        cp -a "$BUILD/linux-release-launcher/openpete" \
              "$BUILD/linux-release-launcher/recomp" "$fb/"
        boot_check "$fb/openpete" linux-release-launcher
        [[ -f "$module" ]] || fail "the shared exe built no module at $module"
        say "first-boot module: $module ($(stat -c %s "$module") bytes)"
    fi
    if (( oracle )); then
        t0=$SECONDS
        in_image "$DEV_IMAGE" default "scripts/setup_duckstation.sh"
        say "setup_duckstation: $((SECONDS - t0)) s"
        t0=$SECONDS
        CTR_ENV+=(-e "OPENPETE_BIN=$exe" -e "OPENPETE_DISC=$CUE")
        [[ -n "${OPENPETE_FRAME_LIMIT:-}" ]] && CTR_ENV+=(-e "OPENPETE_FRAME_LIMIT=$OPENPETE_FRAME_LIMIT")
        in_image "$DEV_IMAGE" none "tests/harness/check_psx_oracle.sh --game $game" \
            || fail "oracle $game"
        say "oracle $game: PASS ($((SECONDS - t0)) s)"
    fi
    say "game $game: PASS"
}

# The import table of a Windows exe names only system DLLs and OpenAL32.dll.
WIN_SYSTEM_DLLS="advapi32 bcrypt cfgmgr32 comdlg32 crypt32 d3d11 d3d12 dbghelp dinput8 dwmapi dxgi gdi32 hid imm32 kernel32 msvcrt ntdll ole32 oleaut32 opengl32 psapi setupapi shell32 shlwapi user32 userenv uxtheme version winmm ws2_32 xinput1_4 api-ms-win-crt-*"

form_windows() {
    check_submodules
    [[ -f "games/$game/generated/PSX.EXE_full.c" ]] \
        || refuse "games/$game/generated holds no recompiled C; run: scripts/container.sh game $game"
    dev_images 1
    local t0=$SECONDS
    in_image "$MINGW_IMAGE" none "
        cmake --preset windows-mingw -B '$BUILD/windows-mingw' -DOPENPETE_GAMES=$game
        cmake --build '$BUILD/windows-mingw' -j $JOBS"
    local exe="$BUILD/windows-mingw/openpete.exe"
    [[ -f "$exe" ]] || fail "no $exe"
    local dlls bad=() d ok pat
    dlls="$(in_image "$MINGW_IMAGE" none "x86_64-w64-mingw32-objdump -p '$exe' | sed -n 's/^[[:space:]]*DLL Name: //p'")"
    for d in $dlls; do
        ok=0
        [[ "${d,,}" == openal32.dll ]] && ok=1
        for pat in $WIN_SYSTEM_DLLS; do
            # shellcheck disable=SC2053
            [[ "${d,,}" == $pat.dll ]] && ok=1
        done
        (( ok )) || bad+=("$d")
    done
    say "windows $game imports: ${dlls//$'\n'/ }"
    (( ${#bad[@]} == 0 )) || fail "imports outside the system set and OpenAL32.dll: ${bad[*]}"
    say "windows $game: PASS ($((SECONDS - t0)) s)"
}

form_shell() {
    dev_images 0
    local name email tty=()
    name="$(git config user.name || true)"
    email="$(git config user.email || true)"
    [[ -n "$name" ]] && CTR_ENV+=(-e "GIT_AUTHOR_NAME=$name" -e "GIT_COMMITTER_NAME=$name")
    [[ -n "$email" ]] && CTR_ENV+=(-e "GIT_AUTHOR_EMAIL=$email" -e "GIT_COMMITTER_EMAIL=$email")
    [[ -t 0 ]] && tty=(-it)
    local cmd='scripts/setup.sh || echo "[container] scripts/setup.sh exited $?"; exec bash'
    (( $# )) && cmd='scripts/setup.sh >/dev/null || true; exec "$@"'
    timing_gate
    "$CTR_ENGINE" "${CTR_ENGINE_ARGS[@]}" run --rm "${tty[@]}" "${CTR_MOUNTS[@]}" "${CTR_ENV[@]}" "${CTR_UIDMAP[@]}" \
        -w "$TREE" "$DEV_IMAGE" bash -c "$cmd" _ "$@"
}

case "$form" in
    engine)  form_engine ;;
    decomp)  form_decomp ;;
    game)    form_game ;;
    windows) form_windows ;;
    shell)   form_shell "$@" ;;
esac
