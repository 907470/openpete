cmake_minimum_required(VERSION 3.22)
project(openpete LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OpenAL has no Config-mode package file in the openal-soft prebuilt, so
# when cross-compiling with MinGW seed the cache vars FindOpenAL.cmake reads.
# Native Linux uses the system OpenAL via FindOpenAL's default search.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    set(_OAL_ROOT "${CMAKE_SOURCE_DIR}/third_party/OpenAL-mingw/x86_64-w64-mingw32")
    # PsyCross uses `#include <AL/al.h>` form, so point at the parent of
    # AL/ (FindOpenAL.cmake's PATH_SUFFIXES allow either, but the one
    # consumers expect is the prefix that makes the angle-bracket include
    # legal). Header lookup: <AL/al.h> → ${OPENAL_INCLUDE_DIR}/AL/al.h.
    set(OPENAL_INCLUDE_DIR "${_OAL_ROOT}/include"
        CACHE PATH "OpenAL include dir (vendored MinGW prebuilt)" FORCE)
    set(OPENAL_LIBRARY "${_OAL_ROOT}/lib/libOpenAL32.dll.a"
        CACHE FILEPATH "OpenAL import library (vendored MinGW prebuilt)" FORCE)
    unset(_OAL_ROOT)
endif()

# Emscripten ships an OpenAL implementation in its sysroot, linked via the
# `-lopenal` flag rather than a discoverable .a archive. PsyCross calls
# find_package(OpenAL REQUIRED), so seed the cache vars FindOpenAL's
# FindPackageHandleStandardArgs checks (it only verifies they're set and
# not -NOTFOUND). Headers live in the toolchain sysroot, found
# automatically by emcc; OPENAL_INCLUDE_DIR just has to point somewhere
# real for FPHSA. The -lopenal flag also gets appended on the final
# browser target's link line (see games/spyro-1/CMakeLists.txt).
if(EMSCRIPTEN)
    set(OPENAL_INCLUDE_DIR "${CMAKE_SYSROOT}/include"
        CACHE PATH "OpenAL include dir (Emscripten sysroot)" FORCE)
    set(OPENAL_LIBRARY "-lopenal"
        CACHE STRING "OpenAL link flag (Emscripten built-in)" FORCE)
endif()

# SDL3 is built from the third_party/SDL submodule, pinned to the
# release-3.4.x branch (currently the release-3.4.16 tag) rather than to
# main: the release branch carries the 2026 Steam Controller support the
# pin was originally taken from main for, and carries it further along —
# input report formats, touchpads and the rumble fix all landed there
# after that commit. Static link, no shared lib, no SDL test apps —
# produces the SDL3::SDL3-static target consumed by openpete_core and
# PsyCross.
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
# XTEST (X11 synthetic-input extension) is not used by openpete — it only
# polls input, never injects it. Disabling drops the libXtst-dev build
# dependency so a from-source SDL build needs no extra system packages.
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL)

# Which games to enable. A game whose generated/ has no recompiled output
# warns and skips itself, so listing one costs nothing until it is regenerated.
set(OPENPETE_GAMES "spyro-1;spyro-2;spyro-3" CACHE STRING "Games to build (semicolon-separated)")

# In-process GLSL→SPIR-V for the mod shader compile service
# (notes/plans/openpete-mod-shaders.md S1). glslang 15.4.0 pinned:
# C++17 under GCC 10 (the MinGW cross compiler's ceiling — same
# constraint that keeps pcsx_redux_gte off for Windows), targets
# vulkan1.0 like the engine's own glslangValidator -V builds. OFF is a
# degraded build: mods' prebuilt .spv still load, GLSL sources refuse
# with a log pointer.
option(OPENPETE_BUILD_GLSLANG
       "Build third_party/glslang for the mod shader compile service" ON)
if(OPENPETE_BUILD_GLSLANG)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)         # no SPIRV-Tools dep
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/glslang)
endif()

# PsyCross integration (Phase 1c+). Builds the static lib and links it
# into openpete_core; psyq_thunks.cpp routes recompiled PSYQ calls into
# PsyCross. PsyCross's CMake calls find_package(OpenAL REQUIRED), so a
# Linux build needs `libopenal-dev`. Disable with -DOPENPETE_BUILD_PSYCROSS=OFF
# if you need a non-PsyCross link path (the per-game thunks will then
# fall through to the abort-on-call weak stubs in psyq_stubs.c).
option(OPENPETE_BUILD_PSYCROSS "Build third_party/PsyCross as a static lib" ON)
if(OPENPETE_BUILD_PSYCROSS)
    add_subdirectory(third_party/PsyCross)
endif()

# Vendored pcsx-redux GTE — third reference engine for the GTE trace logger
# (PsyCross is the second). Adds a static lib `pcsx_redux_gte` exposing one
# C-callable: pcsxr_gte_run(cmd, pre_data[32], pre_ctrl[32], post_data[32],
# post_ctrl[32]).
# pcsx_redux_gte is a GTE trace-logger reference (diagnostic only, not on
# the runtime path) and requires C++23. MinGW-GCC 10 — the version Ubuntu
# 22.04 ships in `gcc-mingw-w64-x86-64-posix` — caps at C++20, so default
# the option OFF when cross-compiling. Native Linux/macOS keep it ON.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    set(_OPENPETE_PCSX_REDUX_GTE_DEFAULT OFF)
else()
    set(_OPENPETE_PCSX_REDUX_GTE_DEFAULT ON)
endif()
option(OPENPETE_BUILD_PCSX_REDUX_GTE
       "Build third_party/pcsx_redux_gte as a static lib"
       ${_OPENPETE_PCSX_REDUX_GTE_DEFAULT})
unset(_OPENPETE_PCSX_REDUX_GTE_DEFAULT)
if(OPENPETE_BUILD_PCSX_REDUX_GTE)
    add_subdirectory(third_party/pcsx_redux_gte)
endif()

# Native render foundation: RHI + Vulkan backend (game-agnostic, under
# core/). Added before core/ so openpete_core can link openpete_rhi as the
# renderer milestones land. See notes/plans/openpete-native-renderer.md.
add_subdirectory(core/rhi)

add_subdirectory(core)

# Host-side unit tests (no PSX boot, just pure-function checks). Testing is
# enabled before the game directories so their per-game ctest rows
# (state_regions_derivation_<game>) register.
option(OPENPETE_BUILD_TESTS "Build host-side unit tests" ON)
if(OPENPETE_BUILD_TESTS)
    enable_testing()
endif()

foreach(game IN LISTS OPENPETE_GAMES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/CMakeLists.txt")
        add_subdirectory(games/${game})
    else()
        message(WARNING "OPENPETE_GAMES requested '${game}' but games/${game}/CMakeLists.txt is missing — skipping")
    endif()
endforeach()

if(OPENPETE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/CMakeLists.txt")
    add_subdirectory(tests/unit)

    # Integration test: gameplay-state-vs-presentation determinism.
    # Asserts that running the headless demo at multiple aspect ratios
    # produces bit-identical gameplay state. Slow (boots two PSX
    # sessions); labeled `slow` so plain `ctest` skips it. Run with
    # `ctest -L slow` or directly via the script.
    add_test(NAME aspect_determinism
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_aspect_determinism.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=8000)
    set_tests_properties(aspect_determinism PROPERTIES
        LABELS "slow;determinism"
        TIMEOUT 600
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Modding-framework PoC-ladder gate (modtest v0): chain-order matrix,
    # observer-neutrality vs the PSX oracle, raw-tier warning, and the
    # double-gems expected-divergence assertion. Slow (four headless
    # sessions); `ctest -L mods` or scripts/check_mods.sh directly.
    # See notes/plans/openpete-modding-framework.md.
    add_test(NAME mod_poc_ladder
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mods.sh)
    set_tests_properties(mod_poc_ladder PROPERTIES
        LABELS "slow;mods"
        TIMEOUT 1800
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # ImGui containment gate: the mods fixture proves the table binds
    # headless, but every guard it installs runs only inside a UI section
    # body, and a body needs an open overlay — so this leg opens one in a
    # real window and reads engagement, the error bracket and the
    # out-of-body refusal off the log. Skips (64) with no display.
    # See notes/plans/openpete-mod-imgui-escape-hatch.md.
    add_test(NAME mod_imgui
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mod_imgui.sh)
    set_tests_properties(mod_imgui PROPERTIES
        LABELS "mods"
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Per-channel material gate: turns every mods/channel-gallery row into
    # an assertion (a row that should paint must, a KNOWN GAP row must
    # not), plus refine-vs-declarative equivalence — including the
    # many-distinct-outcome case, which is the shape that caught
    # per-instance slots being stamped after their intern table was reset.
    # Neither check_mods nor native_visual_regression runs a mod material
    # at all, so this is the only gate that exercises the selector seam.
    # Slow (a dozen headless sessions); `ctest -L mods` or the script.
    add_test(NAME mod_channel_selectors
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_mod_channels.sh)
    set_tests_properties(mod_channel_selectors PROPERTIES
        LABELS "slow;mods"
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Freecam-corpus gate for the terrain near-LP gapfill classifier —
    # replays recorded problematic freecam spots (holes that must stay
    # filled, walls/arches the fill must not touch) and asserts metric
    # expectations with the feature on vs off. Slow (boots ~7 headless
    # sessions); `ctest -L gapfill` or the script directly.
    add_test(NAME gapfill_corpus
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_gapfill_corpus.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(gapfill_corpus PROPERTIES
        LABELS "slow;gapfill"
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Hor+ presentation symmetry — boots the headless demo at 4:3, 16:9
    # and 21:9, takes a screenshot at the same frame, and pixel-diffs the
    # central 4:3 region. Asserts that widening the GTE projection does
    # not perturb the canonical 4:3 view. Currently RED: cyclorama
    # stretching + sub-pixel projection drift leak into the central
    # region. Closing the leak makes this gate green.
    add_test(NAME aspect_screenshot_symmetry
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_aspect_screenshot.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frame=850)
    set_tests_properties(aspect_screenshot_symmetry PROPERTIES
        LABELS "slow;determinism"
        TIMEOUT 600
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Native renderer visual-regression gate (W1.2). Renders the native `-vk`
    # frame HEADLESS through a software rasterizer (surfaceless Vulkan, plan
    # W1.1 — no GPU / no display) at a curated set of attract-demo scenes and
    # compares each against a stored golden with a tolerance metric. The
    # automated safety net for the derive-from-gamestate renderer: catches
    # native regressions and the next undiscovered transient-state seam. Skips
    # cleanly (exit 64) if the binary / disc / goldens / Vulkan loader / python
    # +PIL are absent. See notes/plans/openpete-native-renderer.md (Wave 1)
    # and tests/golden/spyro-1/native-vk/README.md.
    add_test(NAME native_visual_regression
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_native_visual.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(native_visual_regression PROPERTIES
        LABELS "slow;visual"
        # Headroom over the game's own --max-runtime-seconds 600 so the
        # in-loop self-limit always fires first (equal timeouts raced, and a
        # hung game loop can't fire its own limit — the 2026-07-01 flake).
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Presentation-golden image gate (the image-side counterpart to the psx
    # oracle). Captures the native -vk corpus (canonical tick-keyed +
    # deterministic alpha=0.5 intermediate) headless under llvmpipe and diffs it
    # against the committed golden (tests/golden/spyro-1/presentation). A clean
    # tree is pixel-identical, so any divergence is a presentation regression the
    # gameplay oracle can't see. Covers the fanned-out corpus: the 53 skip-to-level
    # bk2s (capped to a few evenly-spaced samples each, 16:9, both LOD modes) plus
    # the debug-freecam poses. Re-bless an intended change with
    # scripts/check_presentation_golden.sh --bless; scope a fast subset with
    # --filter=<bk2-substr>. Skips (exit 64) if the binary / disc / Vulkan loader /
    # PIL are absent. Also runs the credits-roll interpolation leg
    # (scripts/check_credits_interp.sh) so it cannot be skipped separately.
    # See notes/plans/openpete-presentation-golden.md.
    add_test(NAME presentation_golden
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_presentation_golden.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(presentation_golden PROPERTIES
        LABELS "slow;presentation"
        # The whole corpus replays ~53 bk2 movies + the freecam poses (~35 min);
        # generous headroom so a loaded host doesn't spuriously time out.
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The pacing analysis suite's own maths, against synthetic inputs with
    # known answers. Fast, no PSX boot, no display: a wrong Allan deviation or
    # a mis-set gate tolerance is worse than none, because it looks
    # authoritative and nobody re-derives it. See
    # notes/plans/openpete-sim-tick-pacing-perfection.md (M2).
    add_test(NAME pacing_stats
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pacing/test_pacestats.py)
    set_tests_properties(pacing_stats PROPERTIES
        LABELS "pacing"
        TIMEOUT 300
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The standing pacing gate (M9): does the sim clock hold its grid, does it
    # hold BECAUSE presents shed (proved by fault injection, not by a machine
    # fast enough never to exercise the mechanism), and does it hold in a level
    # rather than only on the attract demo. Exit 3 = vacuous (nothing engaged)
    # and fails the test deliberately: a gate that cannot reach its own subject
    # must never read as a pass. 64 = SKIP (no binary / disc / display).
    #
    # NOT in the default set: it runs four 120 s legs and needs a display for
    # three of them. Run with `ctest -L pacing`.
    add_test(NAME pacing_strict
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_pacing.sh)
    set_tests_properties(pacing_strict PROPERTIES
        LABELS "slow;pacing"
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # --record video-capture smoke test. Boots the headless demo with
    # --record and uses ffprobe to assert a playable VFR H.264 stream with
    # even dimensions, the reported frame count, monotonic non-uniform PTS,
    # and window-independent (bars-free) capture at 21:9. Skips cleanly
    # (exit 64) if the disc/binary/ffprobe are absent. See
    # notes/plans/openpete-record-video.md.
    add_test(NAME record_smoke
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_record.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=300)
    set_tests_properties(record_smoke PROPERTIES
        LABELS "slow;tooling"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Performance benchmark pipeline smoke — replays a few bk2 movies through
    # the profiler (--profile-out) and asserts the trace/summary/rollup pipeline
    # runs and parses. NOT a perf-threshold gate (CI hardware varies); the
    # baseline regression check (scripts/bench-corpus.py --baseline) is a
    # manually-run tool. See notes/plans/openpete-perf-profiling.md.
    add_test(NAME bench_corpus_smoke
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bench-corpus.py
                     --exe $<TARGET_FILE:openpete-spyro1>
                     --movies artisans-bridge,stone-hill-entry
                     --frame-limit 4000 --max-runtime-seconds 90
                     --skip-ticks 30
                     --out-dir ${CMAKE_BINARY_DIR}/bench-smoke --run-id ctest)
    set_tests_properties(bench_corpus_smoke PROPERTIES
        LABELS "slow;bench"
        TIMEOUT 400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Channel registry audit — fast static check (no PSX boot). Cross-
    # references manifest/render_channels.toml against the recompiler's
    # address-keyed substitution table; fails on missing or orphaned
    # substitutions. See notes/plans/openpete-determinism-enforcement.md
    # Phase 4.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME channel_registry_audit
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_channel_registry.py)
        set_tests_properties(channel_registry_audit PROPERTIES
            LABELS "determinism"
            TIMEOUT 30
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        # Channel signature anchors (hook-anchor plan step 2): calibrate +
        # verify that every determinism-critical channel site is uniquely
        # anchorable by an instruction-pattern signature (local or
        # function-scoped) in the reference binary, and that the matcher
        # relocates a spliced window (self-test). Skips cleanly if the regen
        # artifacts (PSX.EXE / discovered_functions.log) aren't present.
        add_test(NAME channel_signatures
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_channel_signatures.py
                         --out ${CMAKE_CURRENT_BINARY_DIR}/channel_signatures.txt)
        set_tests_properties(channel_signatures PROPERTIES
            LABELS "determinism"
            TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # No PSX RAM dump in the tree (scripts/fences/no-ram-dump.py). The
    # hook half refuses `git add`/`git commit` of a dump; this half scans
    # `git ls-files` by content (has_ram trace, 2 MB image, PS-X EXE) so a
    # commit made outside the hook is caught before it is pushed. The scan
    # runs its own negative controls first and refuses PASS unless they
    # were refused.
    add_test(NAME no_ram_dumps
             COMMAND python3
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/no-ram-dump.py tree
                     ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(no_ram_dumps PROPERTIES
        LABELS "fence;oracle"
        TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Recompiler GTE data-register side effects: mtc2/lwc2 to $30 must
    # recompute LZCR, to $28 must unpack IR1-3. Recompiles a probe binary
    # with PSXRecomp and runs the emitted C against a harness. Skips when
    # PSXRecomp is not built.
    add_test(NAME recomp_gte_sideeffects
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_recomp_gte.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --cc=${CMAKE_C_COMPILER})
    set_tests_properties(recomp_gte_sideeffects PROPERTIES
        LABELS "recompiler;determinism"
        TIMEOUT 120
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # PSX oracle gate — verify openpete gameplay-state RAM matches a
    # DuckStation (real SCPH1001) baseline tick-for-tick over the 20k-tick
    # attract-demo soak (4+ level loads + gameplay). The 3 CD-load pool
    # regions are tolerated only while a load transient heals; every other
    # region must match bit-exact (see check_psx_oracle.sh / the manifest's
    # cd_load_transient flag). The golden is generated, not committed: the
    # script captures it from the pinned DuckStation fork on the first run
    # (about fifteen minutes, hence the timeout) and skips when the fork is
    # not built (see docs/duckstation-oracle.md).
    add_test(NAME psx_oracle_match
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_psx_oracle.sh)
    set_tests_properties(psx_oracle_match PROPERTIES
        LABELS "slow;oracle"
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1")

    # Matrix determinism gate — broaden coverage from one (aspect,
    # scenario) cell to a sweep across aspects × fovs × scenarios.
    # Same expected-red-vs-regression policy as aspect_determinism.
    # Slow (boots N PSX sessions); only runs under `ctest -L slow`.
    # Phase 6 of openpete-determinism-enforcement.md.
    add_test(NAME determinism_matrix
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=2000)
    set_tests_properties(determinism_matrix PROPERTIES
        LABELS "slow;determinism;matrix"
        TIMEOUT 1800
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Soak determinism gate — long-replay mode for hunting slow-burn
    # / late-frame channel leaks the 2000-frame matrix cells miss.
    # Iterates the full BK2 corpus (minus tas/frame-time-test) at
    # 4:3 vs 16:9, default fov, with --bk2-stop-on-end so each cell
    # exits on movie end. Not part of the default ctest run; opt in
    # via `ctest -L soak`. Substantially slower than matrix.
    add_test(NAME determinism_soak
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --soak)
    set_tests_properties(determinism_soak PROPERTIES
        LABELS "slow;determinism;soak"
        TIMEOUT 7200
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Savestate restore-at-yield gate. Per corpus movie, three headless legs
    # (clean, --state-stress 1, --state-stress-rewind 25) whose
    # --gameplay-hash-log outputs must agree byte-for-byte, plus the runahead
    # stanzas: --runahead 2 must leave the COMMITTED timeline identical to
    # clean, the read-only prediction oracle must report >= 50 checks with
    # zero mismatches (a driver that never speculates would pass the compare
    # trivially), and a portal entry must still complete its level load under
    # speculation. Slow (~20 headless sessions); `ctest -L savestate` or the
    # script directly. Skips (exit 64) if the binary / disc / movies are
    # absent. See notes/plans/openpete-savestate-frame-step-rewind.md.
    add_test(NAME state_roundtrip
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_state_roundtrip.sh)
    set_tests_properties(state_roundtrip PROPERTIES
        LABELS "slow;determinism;savestate"
        # Each leg self-limits at `timeout 600`; headroom for ~20 of them.
        TIMEOUT 7200
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Sub-tick present gameplay-state gate. The psx oracle replays canonical
    # ticks only, so an intermediate present that writes gameplay-readable RAM
    # and mis-restores it is invisible to it. Per movie, two headless legs of
    # the same binary — intermediate re-extracts armed vs not — whose
    # gameplay-hash logs must be identical, with a per-present bracket check
    # that names the region a leak came from, and the canonical leg compared
    # against the blessed baseline under tests/state-baselines/. Refuses to
    # pass at zero sub-tick presents (exit 3, VACUOUS). Runs the `quick`
    # corpus here; `--corpus full` is the arc-end sweep. Skips (exit 64) if
    # the binary or disc are absent.
    add_test(NAME subtick_state
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_subtick_state.py)
    set_tests_properties(subtick_state PROPERTIES
        LABELS "slow;determinism"
        # Four headless legs, each self-limited at --max-runtime-seconds 280.
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/games/spyro-1/openpete-spyro1"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
