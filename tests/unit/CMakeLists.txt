# tests/unit/CMakeLists.txt - host-side unit tests.
#
# These don't boot the PSX runtime; they exercise pure functions
# (parsers, decoders) that we'd rather catch regressions in without a full
# game build. Hooked into ctest via enable_testing() in the root list.

# bk2_movie depends on miniz (zip read) + the log macros. Compile those
# sources directly into the test target instead of linking openpete_core,
# whose link interface carries SDL3, OpenAL and every other runtime
# dependency into a console test that uses none of them.
add_executable(bk2_movie_test
    bk2_movie_test.c
    ${CMAKE_SOURCE_DIR}/core/record/bk2_movie.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(bk2_movie_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/third_party/miniz)
set_target_properties(bk2_movie_test PROPERTIES C_STANDARD 99)
add_test(NAME bk2_movie_test COMMAND bk2_movie_test)

# fnv1a64_test - header-only, no openpete deps. Verifies the FNV1a-64
# implementation underpinning the determinism harness's per-frame
# gameplay-state hash.
add_executable(fnv1a64_test fnv1a64_test.c)
target_include_directories(fnv1a64_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(fnv1a64_test PROPERTIES C_STANDARD 99)
add_test(NAME fnv1a64_test COMMAND fnv1a64_test)

# openpete_time_test - verifies the high-resolution monotonic clock +
# hybrid sleep+spin waiter.
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

# openpete_hook_test - verifies the OPENPETE_HOOK decomp-seam macro. Built
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

# keymap_test - the [keys] action registry, token parser, layered store
# and conflict scan. keymap.c reaches SDL only through the lookup/input
# seams, so this installs fakes for both: the link line stays SDL-free
# like every other console test here, and the fake input state is what
# lets the edge latch and the keyboard-suspend rule be exercised at all.
# log.c comes along for OPL_WARN_F.
add_executable(keymap_test
    keymap_test.c
    ${CMAKE_SOURCE_DIR}/core/input/keymap.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(keymap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(keymap_test PROPERTIES C_STANDARD 11)
add_test(NAME keymap_test COMMAND keymap_test)

# psx_region_test - verifies the PSX RAM virtualization region table
# (registration / lookup / overlap-rejection). Compiles psx_region.c
# directly so openpete_core's SDL3 and OpenAL dependencies stay out of a
# console test binary.
add_executable(psx_region_test
    psx_region_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(psx_region_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psx_region_test PROPERTIES C_STANDARD 99)
add_test(NAME psx_region_test COMMAND psx_region_test)

# psx_runtime_layout_test - static assertions that CPUState in
# core/include/psxrt/psx_runtime.h has the offset, size and type of every
# field of the recompiler runner's CPUState, which savestates and mods built
# against an earlier SDK depend on. The only target that reads the runner
# header.
add_executable(psx_runtime_layout_test psx_runtime_layout_test.c)
target_include_directories(psx_runtime_layout_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psx_runtime_layout_test PROPERTIES C_STANDARD 11)
add_test(NAME psx_runtime_layout_test COMMAND psx_runtime_layout_test)

# psx_span_test - the span form of the guest-pointer resolver and the
# width-checked bus accessors at the ends of main RAM, the scratchpad and a
# registered region. Compiles cpu.c and psx_region.c with local stubs for
# the CD-ROM register model, the BIOS ROM window and the logger.
add_executable(psx_span_test
    psx_span_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(psx_span_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psx_span_test PROPERTIES C_STANDARD 11)
add_test(NAME psx_span_test COMMAND psx_span_test)

# knob_registry_test - the M-overlay knob table: module-contributed rows
# (openpete_knob_add) landing inside their section, capacity refusal, and
# KNOB_ACTION button rows. Compiles knob_registry.c directly and stubs the
# renderer/host setters it writes through, so no SDL in a console test.
# config.c comes along because the registry now asks the resolver where each
# row's value came from instead of re-deriving it - and having both tables in
# one binary is what lets this test assert they agree (see the schema
# cross-check in main).
add_executable(knob_registry_test
    knob_registry_test.c
    ${CMAKE_SOURCE_DIR}/core/config/knob_registry.c
    ${CMAKE_SOURCE_DIR}/core/config/config.c
    ${CMAKE_SOURCE_DIR}/core/config/config_schema.c
    # keymap.c rides along because config.c now seeds and resolves the
    # binding store, paths.c because it decides where the [saved] zone is
    # written. No log.c: this test stubs the log entry points itself.
    ${CMAKE_SOURCE_DIR}/core/input/keymap.c
    ${CMAKE_SOURCE_DIR}/core/config/paths.c
    ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(knob_registry_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(knob_registry_test PROPERTIES C_STANDARD 11)
add_test(NAME knob_registry_test COMMAND knob_registry_test)

# audio_volume_test - the --sfx / --music slider presets: stamping, the
# re-stamp over the game's own reseeds (title screen, SpuInitialize, save
# load), and the release when the player moves that slider in the options
# menu. The module is a state machine over guest RAM, so a fake CPUState over
# a small array covers it without a controller replay.
add_executable(audio_volume_test
    audio_volume_test.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/features/audio_volume.c)
target_include_directories(audio_volume_test PRIVATE
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(audio_volume_test PROPERTIES C_STANDARD 99)
add_test(NAME audio_volume_test COMMAND audio_volume_test)

# vibration_test - the measured motor response and the --vibration
# preset's state machine. Two TUs, both deliberately SDL-free: rumble_curve.c
# is the curve alone (the reference constants a later "it feels too strong"
# tweak would otherwise drift silently), vibration.c is the guest-side
# preset over a fake CPUState. The rest of the vibration path needs a real pad
# and is covered by the virtual-pad rumble oracle instead - see
# scripts/check_rumble.sh.
add_executable(vibration_test
    vibration_test.c
    ${CMAKE_SOURCE_DIR}/core/input/rumble_curve.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/features/vibration.c)
target_include_directories(vibration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(vibration_test PROPERTIES C_STANDARD 99)
if(NOT WIN32)
    target_link_libraries(vibration_test PRIVATE m)
endif()
add_test(NAME vibration_test COMMAND vibration_test)

# memcard_test - the host memory card against killed and failed writes and
# against card files it did not create (atomic replacement, the libmcrd and
# BIOS error paths, set-aside of unknown files, read-only headered images,
# in-place frame writes). POSIX only: the cases fork.
if(NOT WIN32)
    add_executable(memcard_test
        memcard_test.c
        ${CMAKE_SOURCE_DIR}/core/host/memcard.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(memcard_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(memcard_test PROPERTIES C_STANDARD 11)
    add_test(NAME memcard_test
             COMMAND memcard_test ${CMAKE_CURRENT_BINARY_DIR}/memcard_test.d)
endif()

# config_resolve_test - openpete.toml precedence. The resolver decides which
# of ~10 layers wins each knob and then materializes exactly one spelling of
# it; this pins that ordering down so an edit can't quietly re-order the
# tiers. config.c is deliberately SDL-free so it compiles straight in
# alongside tomlc99.
add_executable(config_resolve_test
    config_resolve_test.c
    ${CMAKE_SOURCE_DIR}/core/config/config.c
    ${CMAKE_SOURCE_DIR}/core/config/config_schema.c
    ${CMAKE_SOURCE_DIR}/core/input/keymap.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c
    ${CMAKE_SOURCE_DIR}/core/config/paths.c
    ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(config_resolve_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(config_resolve_test PROPERTIES C_STANDARD 11)
add_test(NAME config_resolve_test COMMAND config_resolve_test)

# record_encode_bench - encode-throughput validation for --record. Drives the swscale RGBA->yuv420p ->
# libx264 path with synthetic frames as fast as the encoder allows and
# reports the sustained fps (480 is the target). Self-contained: finds
# libav* locally so the unit-test dir needn't depend on link order with
# core/. The ctest floor is loose (a gross-regression catch); the real
# number is printed in the RESULT line.
# Native only: host pkg-config resolves host libav, which would poison a
# cross compile (and there's no target libav to link). Mirrors the guard in
# core/CMakeLists.txt.
# Gated on OPENPETE_ENABLE_RECORD too: distribution presets set it OFF, and a
# dist build must not link the GPL libav* anywhere in the tree - not even in a
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
            # An encoder throughput floor measured beside a game process
            # measures the machine; share the game tests' lock.
            RESOURCE_LOCK openpete_game
            TIMEOUT 120)
    endif()
endif()

# openpete_osd_test - the OSD toast ring's state machine on a synthetic
# clock (coalescing, the ttl's first-geometry start, id skew between the two
# readers, eviction, the rate limit, the stack glide). openpete_osd.c is
# compiled directly and SDL's mutex API is faked in the test, so the link
# line stays SDL-free like the rest of the console tests here.
add_executable(openpete_osd_test
    openpete_osd_test.c
    ${CMAKE_SOURCE_DIR}/core/ui/openpete_osd.c)
target_include_directories(openpete_osd_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/third_party/SDL/include)
set_target_properties(openpete_osd_test PROPERTIES C_STANDARD 11)
add_test(NAME openpete_osd_test COMMAND openpete_osd_test)

# vstream_test - the moby vertex-stream resolver + handle table
# (core/psx/vstream.c): stock pass-through, handle indexing in both
# encodings, bind-once entries, and the bounded host alias through a
# registered region reached via a top-bit-masked (KUSEG) pointer.
add_executable(vstream_test
    vstream_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/vstream.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(vstream_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(vstream_test PROPERTIES C_STANDARD 99)
add_test(NAME vstream_test COMMAND vstream_test)

# text_moby_id_test - the HUD text-glyph provenance table
# (games/spyro-1/renderer/text_moby_id.c): keys are the emitter call, not
# the pool address; a generation survives its own draw's drain and is
# cleared by a drain with no emit between it (a draw that allocates pool
# mobys without the text emitters, e.g. the credits roll); the class guard.
add_executable(text_moby_id_test
    text_moby_id_test.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/renderer/text_moby_id.c)
target_include_directories(text_moby_id_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(text_moby_id_test PROPERTIES C_STANDARD 99)
add_test(NAME text_moby_id_test COMMAND text_moby_id_test)

# mod_shader_compile_test - compiles every shipped mod shader (mods/*/**.vert
# and .frag) against the SDK staged next to openpete-spyro1, through the
# compile-on-boot front end (core/mods/mod_glsl.c) - same glslang settings,
# include path and profiles - and fails on any error or when no material
# fragment was compiled. The shaders are otherwise compiled only when a mod
# loads at boot, so an SDK header change can break every material mod with
# no build or test noticing.
if(OPENPETE_BUILD_GLSLANG AND TARGET openpete-spyro1)
    add_executable(mod_shader_compile_test
        mod_shader_compile_test.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_glsl.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(mod_shader_compile_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/mods
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/third_party/glslang/include)
    target_link_libraries(mod_shader_compile_test PRIVATE
        glslang glslang-default-resource-limits)
    set_target_properties(mod_shader_compile_test PROPERTIES C_STANDARD 99)
    # The staged SDK is the directory compile-on-boot resolves includes in.
    add_dependencies(mod_shader_compile_test spyro1_sdk_stage)
    add_test(NAME mod_shader_compile
             COMMAND mod_shader_compile_test ${CMAKE_SOURCE_DIR}/mods
                     $<TARGET_FILE_DIR:openpete-spyro1>/sdk)
    set_tests_properties(mod_shader_compile PROPERTIES LABELS "mods")
endif()

# rhi_memtype_candidates_test - the memory-type preference order behind the
# terrain arenas' allocations (core/rhi/rhi_memtype.c). Compiles that one
# translation unit against the vendored Vulkan headers: the function reads a
# reported memory table and needs no instance, device or loader, so the test
# links nothing.
add_executable(rhi_memtype_candidates_test
    rhi_memtype_candidates_test.c
    ${CMAKE_SOURCE_DIR}/core/rhi/rhi_memtype.c)
target_include_directories(rhi_memtype_candidates_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/rhi
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
set_target_properties(rhi_memtype_candidates_test PROPERTIES C_STANDARD 99)
add_test(NAME rhi_memtype_candidates_test COMMAND rhi_memtype_candidates_test)

# mod_vec_test - the mod surface's two growable tables
# (core/mods/mod_vec.c): the chunked table's growth past one block and the
# element addresses that survive it, the flat table's contiguity after a
# realloc, the order-preserving remove, clear keeping storage, the no-ops
# on a zeroed table, and the exit census line. mod_vec.c reaches nothing
# but malloc and the logger, so log.c is the whole link line.
add_executable(mod_vec_test
    mod_vec_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(mod_vec_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_vec_test PROPERTIES C_STANDARD 99)
add_test(NAME mod_vec_test COMMAND mod_vec_test)
set_tests_properties(mod_vec_test PROPERTIES LABELS "mods")

# tracer_snapshot_test - the Spyro 1 tracer set copy clamps the guest set
# count to the game's four-entry tables (fake bus, no game link).
add_executable(tracer_snapshot_test
    tracer_snapshot_test.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/renderer/tracer_snapshot.c)
target_include_directories(tracer_snapshot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(tracer_snapshot_test PROPERTIES C_STANDARD 11)
add_test(NAME tracer_snapshot_test COMMAND tracer_snapshot_test)

# boot_memfill_test - a crafted EXE header whose memfill range wraps in
# 32 bits is skipped (boot.c + cpu.c with local stubs).
add_executable(boot_memfill_test
    boot_memfill_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/boot.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(boot_memfill_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(boot_memfill_test PROPERTIES C_STANDARD 11)
add_test(NAME boot_memfill_test COMMAND boot_memfill_test)

# exe_loader_cap_test - the disc EXE loader refuses a directory size over
# 2 MiB + 2048 before allocating (crafted ISO9660 image, disc_image reader).
add_executable(exe_loader_cap_test
    exe_loader_cap_test.cpp
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c)
target_include_directories(exe_loader_cap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_link_libraries(exe_loader_cap_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(exe_loader_cap_test PRIVATE winmm)
endif()
set_target_properties(exe_loader_cap_test PROPERTIES CXX_STANDARD 20)
add_test(NAME exe_loader_cap_test COMMAND exe_loader_cap_test)

# disc_cache_short_read_test - a chunk the disc cache loader reads short is
# retried, then served from the file, and never served as stale heap bytes.
# The test TU includes disc_cache.c with fread and malloc replaced.
add_executable(disc_cache_short_read_test
    disc_cache_short_read_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c)
target_include_directories(disc_cache_short_read_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include)
target_link_libraries(disc_cache_short_read_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(disc_cache_short_read_test PRIVATE winmm)
endif()
set_target_properties(disc_cache_short_read_test PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
add_test(NAME disc_cache_short_read_test COMMAND disc_cache_short_read_test)

# disc_image_test - the disc reader on synthetic raw images written by the
# test: PVD, root and one-subdirectory lookup, ';1' and case folding, the
# 2048-byte refusal, sector and user-data reads, cache hits and misses
# against direct reads, and the booted-disc read_sector the mod API forwards
# to (with the api table's appended slot).
add_executable(disc_image_test
    disc_image_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_cache.c
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c)
target_include_directories(disc_image_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include)
target_link_libraries(disc_image_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(disc_image_test PRIVATE winmm)
endif()
set_target_properties(disc_image_test PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
add_test(NAME disc_image_test COMMAND disc_image_test)

# sfx_smpl_walk_test - the WAV smpl chunk walk ends on a chunk that runs past
# the file and refuses loop points outside int range. The test TU includes
# sfx_pack.c with stubs for PsyCross, the mod registry and paths.
add_executable(sfx_smpl_walk_test
    sfx_smpl_walk_test.c
    ${CMAKE_SOURCE_DIR}/core/host/sfx_ident.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(sfx_smpl_walk_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/mods
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/xxhash)
target_link_libraries(sfx_smpl_walk_test PRIVATE SDL3::SDL3-static)
set_target_properties(sfx_smpl_walk_test PROPERTIES C_STANDARD 11)
add_test(NAME sfx_smpl_walk_test COMMAND sfx_smpl_walk_test)
