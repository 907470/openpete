# tests/unit/CMakeLists.txt — host-side unit tests.
#
# These don't boot the PSX runtime; they exercise pure functions
# (parsers, decoders) that we'd rather catch regressions in without a full
# game build. Hooked into ctest via enable_testing() in the root list.

# bk2_movie depends on miniz (zip read) + the log macros. Compile those
# sources directly into the test target instead of linking openpete_core
# — pulling the full static lib transitively drags SDL2 / SDL2main into
# the link line, which under MinGW redefines main → WinMain and the
# console-style test fails to link with "undefined SDL_main".
add_executable(bk2_movie_test
    bk2_movie_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/bk2_movie.c
    ${CMAKE_SOURCE_DIR}/core/runtime/log.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(bk2_movie_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/third_party/miniz)
set_target_properties(bk2_movie_test PROPERTIES C_STANDARD 99)
add_test(NAME bk2_movie_test COMMAND bk2_movie_test)

# fnv1a64_test — header-only, no openpete deps. Verifies the FNV1a-64
# implementation underpinning the determinism harness's per-frame
# gameplay-state hash.
add_executable(fnv1a64_test fnv1a64_test.c)
target_include_directories(fnv1a64_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(fnv1a64_test PROPERTIES C_STANDARD 99)
add_test(NAME fnv1a64_test COMMAND fnv1a64_test)

# openpete_time_test — verifies the high-resolution monotonic clock +
# hybrid sleep+spin waiter (Phase 0 of openpete-frame-pacing.md).
# Compiles openpete_time.c directly to stay SDL-free. The Win32 path
# needs winmm; Linux is link-clean.
add_executable(openpete_time_test
    openpete_time_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c)
target_include_directories(openpete_time_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(openpete_time_test PROPERTIES C_STANDARD 99)
if(WIN32)
    target_link_libraries(openpete_time_test PRIVATE winmm)
endif()
add_test(NAME openpete_time_test COMMAND openpete_time_test)

# openpete_hook_test — verifies the OPENPETE_HOOK decomp-seam macro
# (notes/plans/openpete-generic-hook-anchor-refactor.md step 4). Built
# twice from one source: the "matching" variant (no OPENPETE_BUILD) asserts
# the macro is a no-op (byte-match-preserving); the "openpete" variant
# (-DOPENPETE_BUILD) asserts it calls the named host hook. Header-only.
add_executable(openpete_hook_test_matching openpete_hook_test.c)
target_include_directories(openpete_hook_test_matching PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(openpete_hook_test_matching PROPERTIES C_STANDARD 99)
add_test(NAME openpete_hook_test_matching COMMAND openpete_hook_test_matching)

add_executable(openpete_hook_test_openpete openpete_hook_test.c)
target_include_directories(openpete_hook_test_openpete PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
target_compile_definitions(openpete_hook_test_openpete PRIVATE OPENPETE_BUILD)
set_target_properties(openpete_hook_test_openpete PROPERTIES C_STANDARD 99)
add_test(NAME openpete_hook_test_openpete COMMAND openpete_hook_test_openpete)

# keymap_test — the [keys] action registry, token parser, layered store
# and conflict scan. keymap.c reaches SDL only through the lookup/input
# seams, so this installs fakes for both: the link line stays SDL-free
# like every other console test here, and the fake input state is what
# lets the edge latch and the keyboard-suspend rule be exercised at all.
# log.c comes along for OPL_WARN_F.
add_executable(keymap_test
    keymap_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/keymap.c
    ${CMAKE_SOURCE_DIR}/core/runtime/log.c)
target_include_directories(keymap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(keymap_test PROPERTIES C_STANDARD 11)
add_test(NAME keymap_test COMMAND keymap_test)

# psx_region_test — verifies the PSX RAM virtualization region table
# (registration / lookup / overlap-rejection). Compiles psx_region.c
# directly so we don't drag openpete_core's SDL2 dependency into a
# console test binary. See notes/plans/openpete-ram-virtualization.md.
add_executable(psx_region_test
    psx_region_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/psx_region.c)
target_include_directories(psx_region_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner/include)
set_target_properties(psx_region_test PROPERTIES C_STANDARD 99)
add_test(NAME psx_region_test COMMAND psx_region_test)

# knob_registry_test — the M-overlay knob table: module-contributed rows
# (openpete_knob_add) landing inside their section, capacity refusal, and
# KNOB_ACTION button rows. Compiles knob_registry.c directly and stubs the
# renderer/host setters it writes through, so no SDL in a console test.
# config.c comes along because the registry now asks the resolver where each
# row's value came from instead of re-deriving it — and having both tables in
# one binary is what lets this test assert they agree (see the schema
# cross-check in main).
add_executable(knob_registry_test
    knob_registry_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/knob_registry.c
    ${CMAKE_SOURCE_DIR}/core/runtime/config.c
    ${CMAKE_SOURCE_DIR}/core/runtime/config_schema.c
    # keymap.c rides along because config.c now seeds and resolves the
    # binding store, paths.c because it decides where the [saved] zone is
    # written. No log.c: this test stubs the log entry points itself.
    ${CMAKE_SOURCE_DIR}/core/runtime/keymap.c
    ${CMAKE_SOURCE_DIR}/core/runtime/paths.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(knob_registry_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner/include)
set_target_properties(knob_registry_test PROPERTIES C_STANDARD 11)
add_test(NAME knob_registry_test COMMAND knob_registry_test)

# config_resolve_test — openpete.toml precedence. The resolver decides which
# of ~10 layers wins each knob and then materializes exactly one spelling of
# it; this pins that ordering down so an edit can't quietly re-order the
# tiers. config.c is deliberately SDL-free so it compiles straight in
# alongside tomlc99. See notes/plans/openpete-config-consolidation.md.
add_executable(config_resolve_test
    config_resolve_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/config.c
    ${CMAKE_SOURCE_DIR}/core/runtime/config_schema.c
    ${CMAKE_SOURCE_DIR}/core/runtime/keymap.c
    ${CMAKE_SOURCE_DIR}/core/runtime/log.c
    ${CMAKE_SOURCE_DIR}/core/runtime/paths.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(config_resolve_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(config_resolve_test PROPERTIES C_STANDARD 11)
add_test(NAME config_resolve_test COMMAND config_resolve_test)

# record_encode_bench — encode-throughput validation for --record (plan
# openpete-record-video.md Phase 7). Drives the swscale RGBA->yuv420p ->
# libx264 path with synthetic frames as fast as the encoder allows and
# reports the sustained fps (480 is the plan target). Self-contained: finds
# libav* locally so the unit-test dir needn't depend on link order with
# core/. The ctest floor is loose (a gross-regression catch); the real
# number is printed in the RESULT line.
# Native only: host pkg-config resolves host libav, which would poison a
# cross compile (and there's no target libav to link). Mirrors the guard in
# core/CMakeLists.txt.
# Gated on OPENPETE_ENABLE_RECORD too: distribution presets set it OFF, and a
# dist build must not link the GPL libav* anywhere in the tree — not even in a
# (never-shipped) test binary.
find_package(PkgConfig QUIET)
if(OPENPETE_ENABLE_RECORD AND PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
    pkg_check_modules(BENCH_LIBAV QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale)
    if(BENCH_LIBAV_FOUND)
        add_executable(record_encode_bench
            ${CMAKE_SOURCE_DIR}/tests/bench/record_encode_bench.c)
        target_link_libraries(record_encode_bench PRIVATE PkgConfig::BENCH_LIBAV)
        set_target_properties(record_encode_bench PROPERTIES C_STANDARD 11)
        add_test(NAME record_encode_bench
                 COMMAND record_encode_bench --width 854 --height 480
                         --frames 1000 --fps 480 --min-fps 60
                         --out ${CMAKE_CURRENT_BINARY_DIR}/encode-bench.mkv)
        set_tests_properties(record_encode_bench PROPERTIES
            LABELS "slow;tooling"
            TIMEOUT 120)
    endif()
endif()
