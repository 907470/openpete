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

# op_appendf_test - header-only. op_appendf keeps its length at the bytes
# the buffer holds when the text overflows it.
add_executable(op_appendf_test op_appendf_test.c)
target_include_directories(op_appendf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(op_appendf_test PROPERTIES C_STANDARD 99)
add_test(NAME op_appendf_test COMMAND op_appendf_test)

# portal_cover_slot_test - header-only. The feed-plane to published
# cover-plane slot map an inner-rim face names its accepting plane by.
add_executable(portal_cover_slot_test portal_cover_slot_test.c)
target_include_directories(portal_cover_slot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(portal_cover_slot_test PROPERTIES C_STANDARD 99)
add_test(NAME portal_cover_slot_test COMMAND portal_cover_slot_test)

# eager_ladder_test - header-only. The eager producer's sum pricing and
# decimation ladder on synthetic spans and on recorded plan lines, with the
# previous pricing and ladder as the negative control.
add_executable(eager_ladder_test eager_ladder_test.c)
target_include_directories(eager_ladder_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(eager_ladder_test PRIVATE m)
endif()
set_target_properties(eager_ladder_test PROPERTIES C_STANDARD 99)
add_test(NAME eager_ladder_test COMMAND eager_ladder_test)

# moby_snap_rule_test - header-only. The sunk-pickup snap's side term and
# depth rule on synthetic floors and walls: a double-sided floor keeps the
# upper side from beneath, so the tip hides and the body is not pulled.
add_executable(moby_snap_rule_test moby_snap_rule_test.c)
target_include_directories(moby_snap_rule_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(moby_snap_rule_test PRIVATE m)
endif()
set_target_properties(moby_snap_rule_test PROPERTIES C_STANDARD 99)
add_test(NAME moby_snap_rule_test COMMAND moby_snap_rule_test)

# view_rect_test - header-only. The native view scissor equals the pixel set
# the view's quads cover over a window-size grid; the separately rounded
# origin-and-size arithmetic is the negative control.
add_executable(view_rect_test view_rect_test.c)
target_include_directories(view_rect_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(view_rect_test PRIVATE m)
endif()
set_target_properties(view_rect_test PROPERTIES C_STANDARD 99)
add_test(NAME view_rect_test COMMAND view_rect_test)

# line_quad_test - ngte_line_quad's quad, rasterized at 1x by pixel centres,
# draws the pixels the PSX GPU draws for the line, ties excepted, over
# every line within 64 px of a start pixel; the perpendicular 1 px quad is the
# negative control. Compiles native_gte.c with link stubs for the renderer
# entry points it references.
add_executable(line_quad_test
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    line_quad_test.c
    ${CMAKE_SOURCE_DIR}/games/common/renderer/native_gte.c)
target_include_directories(line_quad_test PRIVATE
    $<TARGET_PROPERTY:openpete_game_common,INCLUDE_DIRECTORIES>)
target_compile_definitions(line_quad_test PRIVATE
    $<TARGET_PROPERTY:openpete_game_common,COMPILE_DEFINITIONS>)
if(NOT WIN32)
    target_link_libraries(line_quad_test PRIVATE m)
endif()
set_target_properties(line_quad_test PROPERTIES C_STANDARD 99)
add_test(NAME line_quad_test COMMAND line_quad_test)

# live_latch_test - header-only. A setting latched once per present reads one
# value through the present while another thread keeps setting it; the
# pre-latch read pattern is the negative control. POSIX threads, so not
# built for Windows.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(live_latch_test live_latch_test.c)
    target_include_directories(live_latch_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include)
    set_target_properties(live_latch_test PROPERTIES C_STANDARD 11)
    target_link_libraries(live_latch_test PRIVATE Threads::Threads)
    add_test(NAME live_latch_test COMMAND live_latch_test)
endif()

# openpete_time_test - verifies the high-resolution monotonic clock +
# hybrid sleep+spin waiter.
# Compiles openpete_time.c directly to stay SDL-free. The Win32 path
# needs winmm; Linux is link-clean.
add_executable(openpete_time_test
    openpete_time_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
target_include_directories(openpete_time_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(openpete_time_test PROPERTIES C_STANDARD 99)
if(WIN32)
    target_link_libraries(openpete_time_test PRIVATE winmm)
endif()
add_test(NAME openpete_time_test COMMAND openpete_time_test)

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

# log_snapshot_test - the logger's level filter, spec read-back and format
# (the `semantics` mode), and the same state reconfigured on one thread
# while others log and read the spec back (the `race` mode). The race runs
# twice, under AddressSanitizer (a rule list freed under a reader) and
# ThreadSanitizer (any unsynchronized access to the logger's state).
# sanitizer_run.sh exits 64 (skipped) where the sanitizer runtime does not
# start; a compiler that cannot build the sanitizer registers the same skip.
# The harness uses POSIX threads, pread and setenv, so it is not built for
# Windows.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(log_snapshot_test
        log_snapshot_test.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(log_snapshot_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include)
    set_target_properties(log_snapshot_test PROPERTIES C_STANDARD 11)
    target_link_libraries(log_snapshot_test PRIVATE Threads::Threads)
    add_test(NAME log_snapshot_semantics COMMAND log_snapshot_test semantics)
    set_tests_properties(log_snapshot_semantics PROPERTIES LABELS "log")
    include(CheckCSourceCompiles)
    foreach(san address thread)
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=${san}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=${san}")
        check_c_source_compiles("int main(void) { return 0; }"
            OPENPETE_HAVE_FSANITIZE_${san})
        unset(CMAKE_REQUIRED_FLAGS)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
        set(test_name log_snapshot_race_${san})
        if(OPENPETE_HAVE_FSANITIZE_${san})
            set(target log_snapshot_test_${san})
            add_executable(${target}
                log_snapshot_test.c
                ${CMAKE_SOURCE_DIR}/core/diag/log.c)
            target_include_directories(${target} PRIVATE
                ${CMAKE_SOURCE_DIR}/core/include)
            set_target_properties(${target} PROPERTIES C_STANDARD 11)
            target_compile_options(${target} PRIVATE
                -fsanitize=${san} -fno-omit-frame-pointer -g -O1)
            target_link_options(${target} PRIVATE -fsanitize=${san})
            target_link_libraries(${target} PRIVATE Threads::Threads)
            add_test(NAME ${test_name}
                COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sanitizer_run.sh
                        $<TARGET_FILE:${target}> race 2 3 200000)
        else()
            add_test(NAME ${test_name} COMMAND sh -c
                "echo '-fsanitize=${san} does not build here; skipped'; exit 64")
        endif()
        set_tests_properties(${test_name} PROPERTIES
            LABELS "log;sanitizer" SKIP_RETURN_CODE 64 TIMEOUT 120)
    endforeach()
endif()

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

# psxtrace_schedule_test - the oracle schedule's account of a CdRead or
# waited VSync its sidecars have no row left for: counted past the trace's
# last record, a failure inside the records. One process per case, since
# the schedule keeps its state in file statics. Compiles
# psxtrace_schedule.c and log.c directly; the test defines the anchor
# counters and the root counter 2 hook the schedule links against.
add_executable(psxtrace_schedule_test
    psxtrace_schedule_test.c
    ${CMAKE_SOURCE_DIR}/core/diag/psxtrace_schedule.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(psxtrace_schedule_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psxtrace_schedule_test PROPERTIES C_STANDARD 11)
add_test(NAME psxtrace_schedule_past_end COMMAND psxtrace_schedule_test past-end)
add_test(NAME psxtrace_schedule_beyond_inside COMMAND psxtrace_schedule_test inside)

# psxtrace_ratchet_test - the oracle's event ratchet: a row's listed GTE
# events, VBL events and gameplay bursts pass, an unlisted one is new, a
# listed one the run never produced is stale; and the carry, VBL-event and
# load-span rules on synthetic record sequences. Compiles
# psxtrace_ratchet.c, log.c and tomlc99 directly.
add_executable(psxtrace_ratchet_test
    psxtrace_ratchet_test.c
    ${CMAKE_SOURCE_DIR}/core/diag/psxtrace_ratchet.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(psxtrace_ratchet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psxtrace_ratchet_test PROPERTIES C_STANDARD 11)
add_test(NAME psxtrace_ratchet_test
         COMMAND psxtrace_ratchet_test ${CMAKE_CURRENT_BINARY_DIR}/psxtrace_ratchet_test.toml)

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
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    psx_span_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(psx_span_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psx_span_test PROPERTIES C_STANDARD 11)
add_test(NAME psx_span_test COMMAND psx_span_test)

# psx_span_test_nomirror - the same checks over a bus built with the RAM
# mirror removed (OPENPETE_PSX_RAM_MIRROR_END = 2 MB): the mirror checks
# must fail, so the test is expected to fail.
add_executable(psx_span_test_nomirror
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    psx_span_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(psx_span_test_nomirror PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_compile_definitions(psx_span_test_nomirror PRIVATE OPENPETE_PSX_RAM_MIRROR_END=0x00200000u)
set_target_properties(psx_span_test_nomirror PROPERTIES C_STANDARD 11)
add_test(NAME psx_span_test_nomirror COMMAND psx_span_test_nomirror)
set_tests_properties(psx_span_test_nomirror PROPERTIES WILL_FAIL TRUE)

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
# in-place frame writes, the card guard). POSIX only: the cases fork.
if(NOT WIN32)
    add_executable(memcard_test
        memcard_test.c
        ${CMAKE_SOURCE_DIR}/core/host/memcard.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/diag/error_ids.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(memcard_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(memcard_test PROPERTIES C_STANDARD 11)
    add_test(NAME memcard_test
             COMMAND memcard_test ${CMAKE_CURRENT_BINARY_DIR}/memcard_test.d)
endif()

# atomic_write_test - every write class of the atomic-file helper (synced,
# copy, unsynced, the .part rename, the directory swap) against an injected
# OPENPETE_WRITE_FAIL: the previous file or tree must be byte-identical, and
# the run refuses success when no failure was injected. POSIX only: the
# directory case uses dirent and rmdir.
if(NOT WIN32)
    add_executable(atomic_write_test
        atomic_write_test.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(atomic_write_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(atomic_write_test PROPERTIES C_STANDARD 11)
    add_test(NAME atomic_write_test
             COMMAND atomic_write_test ${CMAKE_CURRENT_BINARY_DIR}/atomic_write_test.d)
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

# identify_test - game identification: the serial table against every
# game's recomp.conf, and openpete_identify's source order and refusals.
# The disc read and identify.c's calls into the parser, the config layer,
# the boot line, the clock and the disc trace are stubbed in the test.
add_executable(identify_test
    identify_test.c
    ${CMAKE_SOURCE_DIR}/core/config/identify.c
    ${CMAKE_SOURCE_DIR}/core/config/game_registry.c
    ${CMAKE_SOURCE_DIR}/core/diag/error_ids.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(identify_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_compile_definitions(identify_test PRIVATE
    OPENPETE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(identify_test PROPERTIES C_STANDARD 11)
add_test(NAME identify_test COMMAND identify_test)

# options_validate_test - the option parser's shared value validator: a
# refused value fails when typed and warns and keeps the default from
# openpete.toml or an env spelling, strict mode fails both, the env spellings
# resolved after the flags (an [env] table reaches them), and
# openpete_options_effective. options.c calls SDL for the --bk2-record
# directory, so the test links SDL statically; the game hooks, the SDL key
# names and the anchor parser are stubbed.
if(TARGET SDL3::SDL3-static)
    add_executable(options_validate_test
        options_validate_test.c
        ${CMAKE_SOURCE_DIR}/core/config/options.c
        ${CMAKE_SOURCE_DIR}/core/config/config.c
        ${CMAKE_SOURCE_DIR}/core/config/config_schema.c
        ${CMAKE_SOURCE_DIR}/core/input/keymap.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
    target_include_directories(options_validate_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(options_validate_test PRIVATE SDL3::SDL3-static)
    if(NOT WIN32)
        target_link_libraries(options_validate_test PRIVATE m)
    endif()
    set_target_properties(options_validate_test PROPERTIES C_STANDARD 11)
    add_test(NAME options_validate_test COMMAND options_validate_test)
endif()

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
# line stays SDL-free like the rest of the console tests here. log.c comes
# along for the toast ring's debug lines.
add_executable(openpete_osd_test
    openpete_osd_test.c
    ${CMAKE_SOURCE_DIR}/core/ui/openpete_osd.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(openpete_osd_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
target_include_directories(openpete_osd_test SYSTEM PRIVATE
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

# mod_arena_test - the guest arena's two pools (core/mods/mod_arena.c):
# pool selection by flag, the upper-then-lower segment order, per-segment
# exhaustion naming every owner, the version-3 section mapped onto the
# upper segment, the version-4 and in-memory snapshot round trips, KSEG2
# refusal, a read past a committed extent answered by the ROM model, and
# the rewind page tables (core/record/rewind_pages.c) over the three ranges.
# Links the region table, the bus accessors, the stream-handle table and
# the flat vector; the logger, CD-ROM registers and BIOS ROM are stubs.
add_executable(mod_arena_test
    mod_arena_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_arena.c
    ${CMAKE_SOURCE_DIR}/core/record/rewind_pages.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/psx/vstream.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(mod_arena_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_arena_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_arena_test COMMAND mod_arena_test)
set_tests_properties(mod_arena_test PROPERTIES LABELS "mods")

# text_moby_id_test - the HUD text-glyph provenance table
# (games/spyro-1/renderer/text_moby_id.c): keys are the emitter call, not
# the pool address; a generation survives its own draw's drain and is
# cleared by a drain with no emit between it (a draw that allocates pool
# mobys without the text emitters, e.g. the credits roll); the class guard.
add_executable(text_moby_id_test
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    text_moby_id_test.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/renderer/text_moby_id.c)
target_include_directories(text_moby_id_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(text_moby_id_test PROPERTIES C_STANDARD 99)
add_test(NAME text_moby_id_test COMMAND text_moby_id_test)

# mod_shader_compile_test - compiles every shipped mod shader (mods/*/**.vert
# and .frag) against the SDK staged next to Spyro 1's executable, through the
# compile-on-boot front end (core/mods/mod_glsl.c) - same glslang settings,
# include path and profiles - and fails on any error or when no material
# fragment was compiled. The shaders are otherwise compiled only when a mod
# loads at boot, so an SDK header change can break every material mod with
# no build or test noticing.
if(OPENPETE_BUILD_GLSLANG AND TARGET openpete-spyro1)
    add_executable(mod_shader_compile_test
        mod_shader_compile_test.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_glsl.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_cdata.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(mod_shader_compile_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/mods
        ${CMAKE_SOURCE_DIR}/third_party)
    target_include_directories(mod_shader_compile_test SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/third_party/glslang/include)
    target_link_libraries(mod_shader_compile_test PRIVATE
        glslang glslang-default-resource-limits)
    set_target_properties(mod_shader_compile_test PROPERTIES C_STANDARD 99)
    # The staged SDK is the directory compile-on-boot resolves includes in.
    add_dependencies(mod_shader_compile_test spyro1_sdk_stage)
    openpete_game_bin(spyro-1 _shader_bin _shader_args)
    get_filename_component(_shader_dir "${_shader_bin}" DIRECTORY)
    # spirv-val, when the host has it, validates every module the
    # constant-table move rewrites; the fixture run requires one to move.
    find_program(OPENPETE_SPIRV_VAL spirv-val)
    set(_spirv_val "${OPENPETE_SPIRV_VAL}")
    if(NOT OPENPETE_SPIRV_VAL)
        set(_spirv_val "")
    endif()
    add_test(NAME mod_shader_compile
             COMMAND mod_shader_compile_test ${CMAKE_SOURCE_DIR}/mods
                     ${_shader_dir}/sdk "${_spirv_val}")
    set_tests_properties(mod_shader_compile PROPERTIES LABELS "mods")
    add_test(NAME mod_shader_table_move
             COMMAND mod_shader_compile_test
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/spv-cdata-mods
                     ${_shader_dir}/sdk "${_spirv_val}" --expect-moved)
    set_tests_properties(mod_shader_table_move PROPERTIES LABELS "mods")

    # mod_material_link_test - material composition by source prefixing
    # (core/mods/mod_mat_link.c): every in-tree material shader classified
    # linkable or not with its reason (each linkable but mat-chain-unlinkable,
    # which declares a buffer of its own to be unlinkable), the fixture cases under
    # data/mat-link/cases.tsv, and every combined module compiled, checked by
    # spirv-val and rebuilt in other member orders to the same bytes. The
    # _vk test creates each module's pipelines on a headless device under
    # the Khronos validation layer (exit 77 without a device or the layer).
    add_executable(mod_material_link_test
        mod_material_link_test.c
        mod_material_link_vk.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_mat_link.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_reflect.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_glsl.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_cdata.c
        ${CMAKE_SOURCE_DIR}/core/host/sha256.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(mod_material_link_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/mods
        ${CMAKE_SOURCE_DIR}/core/rhi
        ${CMAKE_SOURCE_DIR}/third_party)
    target_include_directories(mod_material_link_test SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/third_party/glslang/include)
    target_link_libraries(mod_material_link_test PRIVATE
        glslang glslang-default-resource-limits openpete_rhi SDL3::SDL3-static ${CMAKE_DL_LIBS})
    set_target_properties(mod_material_link_test PROPERTIES C_STANDARD 11)
    add_dependencies(mod_material_link_test spyro1_sdk_stage)
    add_test(NAME mod_material_link
             COMMAND mod_material_link_test ${_shader_dir}/sdk "${_spirv_val}"
                     --expect-linkable
                     --expect-unlinkable mat-chain-unlinkable/shaders/tint_table.frag
                     --census ${CMAKE_SOURCE_DIR}/mods
                     --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/data/mat-link)
    set_tests_properties(mod_material_link PROPERTIES LABELS "mods")
    add_test(NAME mod_material_link_vk
             COMMAND mod_material_link_test ${_shader_dir}/sdk "${_spirv_val}" --vk
                     --expect-linkable
                     --expect-unlinkable mat-chain-unlinkable/shaders/tint_table.frag
                     --census ${CMAKE_SOURCE_DIR}/mods
                     --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/data/mat-link)
    set_tests_properties(mod_material_link_vk PROPERTIES LABELS "mods;vulkan"
                         SKIP_RETURN_CODE 77)
endif()

# rhi_smoketest --headless - the RHI self-checking cases on a surfaceless
# device (llvmpipe where no GPU is present): a vertex stage reading its
# positions and colours from a retained storage buffer must reproduce the
# expected image exactly on four frames that cover commit, persistence and
# a same-frame rewrite (core/rhi/smoketest.c). Exit 77 without a device.
if(TARGET rhi_smoketest)
    add_test(NAME rhi_smoketest_headless COMMAND rhi_smoketest --headless)
    set_tests_properties(rhi_smoketest_headless PROPERTIES LABELS "vulkan"
                         SKIP_RETURN_CODE 77 TIMEOUT 120)
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

# mod_material_resolve_test - the material resolver
# (core/mods/mod_material_resolve.c) against stub loader hooks: per case,
# materials for stub mods (enabled-list position, api pin), the extraction,
# bake and draw-time outcome, and the resolve census counters.
add_executable(mod_material_resolve_test
    mod_material_resolve_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_material_resolve.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(mod_material_resolve_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods)
target_compile_definitions(mod_material_resolve_test PRIVATE OPENPETE_ENGINE_BUILD)
set_target_properties(mod_material_resolve_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_material_resolve_test COMMAND mod_material_resolve_test)
set_tests_properties(mod_material_resolve_test PROPERTIES LABELS "mods")

# mod_games_test - the games a mod names besides the one it runs on
# (core/mods/mod_games.c): list merging and limits, the route of a disc call
# for every role and install state against a fake library lookup, the first
# missing required game, the installed query and the refused-mod rows.
add_executable(mod_games_test
    mod_games_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_games.c)
target_include_directories(mod_games_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_games_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_games_test COMMAND mod_games_test)
set_tests_properties(mod_games_test PROPERTIES LABELS "mods")

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
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
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
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
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
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
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

# spual_interp_test - the SPU mixer's 4-point gaussian for stock voices,
# looping sample-override voices across the loop seam, and host clips,
# against the psx-spx formula over the sequence each voice plays. The TU
# includes PsyX_SPUAL.cpp and stubs its other PsyCross dependencies.
add_executable(spual_interp_test spual_interp_test.cpp)
target_include_directories(spual_interp_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio)
target_include_directories(spual_interp_test PRIVATE ${OPENAL_INCLUDE_DIR})
target_link_libraries(spual_interp_test PRIVATE SDL3::SDL3-static ${OPENAL_LIBRARY})
target_compile_options(spual_interp_test PRIVATE -Wno-unused-parameter -Wno-maybe-uninitialized)
set_target_properties(spual_interp_test PROPERTIES CXX_STANDARD 20)
add_test(NAME spual_interp_test COMMAND spual_interp_test)

# xa_image_end_test - the XA decoder's image reads stay inside the disc
# image while the drive head runs past its last sector, and it reads up to
# that sector. The TU includes PsyX_XA.cpp with the image reader and the CD
# input stubbed.
add_executable(xa_image_end_test xa_image_end_test.cpp)
target_include_directories(xa_image_end_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio)
target_link_libraries(xa_image_end_test PRIVATE SDL3::SDL3-static)
set_target_properties(xa_image_end_test PROPERTIES CXX_STANDARD 20)
add_test(NAME xa_image_end_test COMMAND xa_image_end_test)

# sfx_name_parse_test - sound pack file names classify as hash, symbolic or
# ignored, with case-insensitive ".wav" and "-flat". sfx_ident.c compiles
# with PsyCross's include/psx on the path, as in openpete_core.
add_executable(sfx_name_parse_test
    sfx_name_parse_test.c
    ${CMAKE_SOURCE_DIR}/core/host/sfx_ident.c)
target_include_directories(sfx_name_parse_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/xxhash)
set_target_properties(sfx_name_parse_test PROPERTIES C_STANDARD 11)
add_test(NAME sfx_name_parse_test COMMAND sfx_name_parse_test)

# rhi_compile_stress - variant-explicit compile jobs run on worker threads
# while the mod registry's per-tick writers, enable toggles and hot reloads
# run on a guest thread and the display owner makes jobs, edits the
# environment, installs results and retires pipelines (contract in the
# source). Compiles the mod registry with local stubs for the loader, the
# GLSL front end and the renderer, and links the RHI. Needs a Vulkan device:
# exit 77 (no device) is a skip.
set(RHI_STRESS_SOURCES
    rhi_compile_stress.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_shader.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_material_resolve.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_cdata.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_reflect.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_mat_link.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/host/sha256.c
    ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
set(RHI_STRESS_INCLUDES
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/rhi)
add_executable(rhi_compile_stress ${RHI_STRESS_SOURCES})
target_include_directories(rhi_compile_stress PRIVATE ${RHI_STRESS_INCLUDES})
target_compile_definitions(rhi_compile_stress PRIVATE OPENPETE_NO_GLSLANG)
target_link_libraries(rhi_compile_stress PRIVATE openpete_rhi SDL3::SDL3-static ${CMAKE_DL_LIBS})
set_target_properties(rhi_compile_stress PROPERTIES C_STANDARD 11)
add_test(NAME rhi_compile_stress COMMAND rhi_compile_stress 4 3)
set_tests_properties(rhi_compile_stress PROPERTIES SKIP_RETURN_CODE 77 LABELS "vulkan")

# The same harness with the RHI compiled in under ThreadSanitizer. Built on
# request only (cmake --build --preset linux-release --target
# rhi_compile_stress_tsan); run under `setarch "$(uname -m)" -R` where the
# kernel's mmap randomization exceeds the sanitizer's layout.
if(NOT WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    get_property(OPENPETE_RHI_SOURCES GLOBAL PROPERTY OPENPETE_RHI_SOURCES)
    foreach(src IN LISTS OPENPETE_RHI_SOURCES)
        if(src MATCHES "rhi_shaders_embedded\\.c$")
            set_source_files_properties(${src} PROPERTIES GENERATED TRUE)
        endif()
    endforeach()
    add_executable(rhi_compile_stress_tsan EXCLUDE_FROM_ALL
        ${RHI_STRESS_SOURCES} ${OPENPETE_RHI_SOURCES})
    target_include_directories(rhi_compile_stress_tsan PRIVATE ${RHI_STRESS_INCLUDES}
        ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
        ${CMAKE_SOURCE_DIR}/third_party/volk)
    target_compile_definitions(rhi_compile_stress_tsan PRIVATE OPENPETE_NO_GLSLANG VK_NO_PROTOTYPES)
    target_compile_options(rhi_compile_stress_tsan PRIVATE -fsanitize=thread -g -O1)
    target_link_options(rhi_compile_stress_tsan PRIVATE -fsanitize=thread)
    target_link_libraries(rhi_compile_stress_tsan PRIVATE SDL3::SDL3-static ${CMAKE_DL_LIBS})
    set_target_properties(rhi_compile_stress_tsan PROPERTIES C_STANDARD 11)
    # openpete_rhi's build generates the embedded-shader source.
    add_dependencies(rhi_compile_stress_tsan openpete_rhi)
endif()

# write_census_test - the write census range table: a spec of twelve ranges
# logs a row for a store into each range past the eighth, and a malformed
# range refuses the whole spec. Compiles write_census.c with stubs for the
# overlay table; the CPU is a struct with counting store hooks. SDL3 is
# linked for the headers openpete_fiber.h (openpete_frame) includes.
add_executable(write_census_test
    write_census_test.c
    ${CMAKE_SOURCE_DIR}/games/common/write_census.c)
target_include_directories(write_census_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/games/common)
set_target_properties(write_census_test PROPERTIES C_STANDARD 11)
target_link_libraries(write_census_test PRIVATE SDL3::SDL3-static)
add_test(NAME write_census_ranges COMMAND write_census_test ranges
    ${CMAKE_CURRENT_BINARY_DIR}/write_census_ranges.csv)
add_test(NAME write_census_refuse COMMAND write_census_test refuse
    ${CMAKE_CURRENT_BINARY_DIR}/write_census_refuse.csv)

# launcher_fit_test - header-only. The launcher window's design size scaled
# by a display content scale and fitted to a display's full and usable
# bounds, as on Windows, at four scales on three taskbar displays.
add_executable(launcher_fit_test launcher_fit_test.c)
target_include_directories(launcher_fit_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(launcher_fit_test PRIVATE m)
endif()
set_target_properties(launcher_fit_test PROPERTIES C_STANDARD 99)
add_test(NAME launcher_fit_test COMMAND launcher_fit_test)

# launcher_page_lines_test - the game page's lines below its boxes: their text
# for 0 to 3 games in the library, and their fit at the supported windows,
# measured with the real body font in an imgui context with no backend.
if(TARGET openpete_imgui)
    add_executable(launcher_page_lines_test
        launcher_page_lines_test.cpp
        ${CMAKE_SOURCE_DIR}/core/launcher/launcher_font.cpp
        ${CMAKE_SOURCE_DIR}/core/config/game_registry.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(launcher_page_lines_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/launcher
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    target_link_libraries(launcher_page_lines_test PRIVATE openpete_imgui)
    set_target_properties(launcher_page_lines_test PROPERTIES CXX_STANDARD 20 C_STANDARD 11)
    add_test(NAME launcher_page_lines_test COMMAND launcher_page_lines_test)
endif()

# These compile sources that carry role asserts and binds, whose definitions
# live in the role runtime.
foreach(t boot_memfill_test line_quad_test mod_arena_test mod_material_link_test
          mod_material_resolve_test mod_vec_test psx_span_test rhi_compile_stress
          rhi_compile_stress_tsan sfx_smpl_walk_test text_moby_id_test
          write_census_test)
    if(TARGET ${t})
        target_sources(${t} PRIVATE ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
    endif()
endforeach()

# audio_feed_sim - the audio device feed on its synthetic device, driven by
# a synthetic producer through a producer-speed span (the fourth field of
# OPENPETE_AUDIO_FEED_SIM); reports the queued latency after the span and
# the splice step of every drop. The TU includes PsyX_SPUFeed.cpp; the
# OpenAL calls link but never run. tests/harness/check_audio_feed_latency.sh
# runs its rows.
add_executable(audio_feed_sim audio_feed_sim.cpp)
target_include_directories(audio_feed_sim PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio)
target_include_directories(audio_feed_sim PRIVATE ${OPENAL_INCLUDE_DIR})
target_link_libraries(audio_feed_sim PRIVATE SDL3::SDL3-static ${OPENAL_LIBRARY})
set_target_properties(audio_feed_sim PROPERTIES CXX_STANDARD 20)
