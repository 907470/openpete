# core/CMakeLists.txt — game-agnostic OpenPete runtime.
#
# Builds a static lib `openpete_core` consumed by per-game targets under
# games/<game>/. Anything Spyro-specific belongs in games/spyro-1/, not
# here. See notes/plans/openpete-pc-port.md.

set(PSXRECOMP_RUNNER ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner)

add_library(openpete_core STATIC
    runtime/log.c
    runtime/options.c
    runtime/screenshot.c
    runtime/run.c
    runtime/psxtrace_verify.c

    # BizHawk .bk2 movie replay (--bk2) + record (--bk2-record). See
    # plans/openpete-bk2-input.md.
    runtime/bk2_movie.c
    runtime/bk2_record.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c

    # Save states (F2/F4 quick save/load). See
    # plans/openpete-savestate-frame-step-rewind.md.
    runtime/savestate.c

    # Phase 1b: slim PSX runtime
    runtime/cpu.c
    runtime/boot.c
    runtime/dispatch.c
    runtime/openpete_ui_moby.c
    runtime/openpete_world_geo.c
    runtime/feature_registry.c

    # High-resolution monotonic clock + hybrid sleep+spin waiter. Leaf
    # module (no SDL); used by ticker + dual-pacer. See
    # notes/plans/openpete-frame-pacing.md.
    runtime/openpete_time.c

    # Binary per-present frame-time recorder for the pacing harness.
    # Off by default; enabled by --frame-time-log PATH.
    runtime/openpete_frame_time_log.c

    # Render-side state interpolation snapshot ring (game-agnostic;
    # regions supplied by games/<game>/). Capture-only in v1; lerp +
    # re-emission lands in fps-unlock Phase 2a. See
    # notes/plans/openpete-fps-unlock.md.
    runtime/openpete_interp.c

    # World-space render cache. Populated per-vertex by psxrecomp's
    # gte_rtps_internal; consumed by PsyCross's OT walker (PsyX_GPU.cpp
    # MakeVertexTriangle/Quad) to recover world-space + matrix state at
    # draw time. Phase 0 foundation for draw-call matching and renderer-
    # side lerp. See notes/plans/openpete-renderer-foundation.md.
    runtime/openpete_render_cache.c

    # Draw-call matcher (renderer-foundation Phase 1). Groups OT prims
    # by matrix_span_id, hashes each batch by geometric fingerprint,
    # matches curr-frame batches against prev-frame. Produces stable
    # object_ids that Phase 2 lerp will use as identity for T/R/S
    # decompose + interpolate. No render changes in Phase 1.
    runtime/openpete_render_matcher.c

    # Per-batch binary trace for the substitute pipeline. Off unless
    # --vertex-trace-log PATH is given. Consumed by
    # scripts/analyze-vtrace.py.
    runtime/openpete_vertex_trace.c

    # Per-feature modules. See notes/plans/openpete-feature-modules.md.
    features/no_lod.c
    features/widescreen.c

    runtime/crash.c
    runtime/exe_loader.cpp

    # PSX RAM virtualization region table — backs cold-path memory
    # resolvers. See notes/plans/openpete-ram-virtualization.md.
    runtime/psx_region.c

    # Phase 1d: cooperative scheduler globals (host/game fiber, frame counter).
    runtime/fiber.c

    # Free-running 60 Hz vblank ticker (drives openpete_vbl + paces the host
    # loop). See plans/openpete-free-running-vblank-counter.md.
    runtime/ticker.c

    # Phase 1e infra-triangle: hardcoded POLY_F3 emit to validate the full
    # PsyCross render pipeline without depending on overlay-resident Spyro
    # code. C++ TU because PsyCross's libgpu.h uses static_assert.
    runtime/debug_render.cpp

    # Top-left FPS overlay drawn via direct OpenGL during PsyCross's
    # post-render hook (g_psyx_postRenderOverlay). C++ TU for the GL
    # pipeline boilerplate.
    runtime/fps_overlay.cpp

    # Vendored psxrecomp pieces we keep verbatim (per notes plan §5 Phase 1b
    # "keep gte.cpp + memory helpers + iso_reader"). Compiled directly here so
    # we don't drag the rest of psxrecomp's runtime.c into the build.
    ${PSXRECOMP_RUNNER}/src/gte.cpp
    ${PSXRECOMP_RUNNER}/src/iso_reader.cpp
)

# libco: cooperative single-fiber scheduler. third_party/libco/libco.c
# `#include`s a per-arch sibling (amd64.c on x86_64 Linux/macOS, fiber.c
# under MinGW). The Phase 1b shim has been retired now that we ship the
# real implementations.
target_sources(openpete_core PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/libco/libco.c)

target_include_directories(openpete_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PSXRECOMP_RUNNER}/include
        ${CMAKE_SOURCE_DIR}/third_party/libco
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${CMAKE_SOURCE_DIR}/third_party/miniz
)

set_target_properties(openpete_core PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    # iso_reader.cpp uses C++20 ends_with; bump the whole core lib so we
    # don't need per-source overrides.
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# SDL3 is required for window/render/screenshot. Built from the
# third_party/SDL submodule (see top-level CMakeLists.txt).
target_link_libraries(openpete_core PUBLIC SDL3::SDL3-static)

# PsyCross provides the PSYQ surface (libgs/libgte/libetc/libapi/...) used by
# PSYQ-targeted thunks. Phase 1c flips this default ON; downstream targets
# include PsyCross/include and link `psycross_static` automatically.
if(TARGET psycross_static)
    target_link_libraries(openpete_core PUBLIC psycross_static)
    target_include_directories(openpete_core PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
        ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HAS_PSYCROSS=1)
endif()

# Win32: openpete_time.c uses timeBeginPeriod from winmm.
if(WIN32)
    target_link_libraries(openpete_core PUBLIC winmm)
endif()

# Vendored pcsx-redux GTE — third reference engine for the trace logger in
# third_party/psxrecomp/runner/src/gte.cpp.
if(TARGET pcsx_redux_gte)
    target_link_libraries(openpete_core PUBLIC pcsx_redux_gte)
else()
    # When the vendored GTE is disabled (e.g. MinGW preset, where its C++23
    # requirement isn't met), define `pcsxr_gte_run` as a no-op so the
    # trace-logger path links. The trace logger is only invoked when the
    # logger is explicitly enabled, which it isn't on the runtime path.
    target_sources(openpete_core PRIVATE runtime/pcsxr_gte_stub.c)
endif()

# Fail the build (cheap automated check) if anyone smuggles a Spyro
# address or Win32-only header into core/. The exit-criteria greps in
# §5 Phase 0b live here as build-time guards.
if(NOT CMAKE_HOST_WIN32)
    # A handful of core/ TUs legitimately #include <windows.h>, each
    # behind `#ifdef _WIN32` so the POSIX build is unaffected:
    #   - crash.c             Win32 SEH unhandled-exception filter
    #   - openpete_time.c     QPC + waitable timer + timeBeginPeriod
    #   - openpete_frame_time_log.c  FILETIME / GetSystemTimeAsFileTime
    # The rest of core/ must remain game-agnostic AND platform-agnostic,
    # so keep the grep-guard but list the exempt TUs by name. New core
    # TUs that need Win32 APIs MUST be added here AND keep their
    # platform calls under `#ifdef _WIN32`, AND NOT pull CreateFiber
    # (we use libco's amd64.c on POSIX and fiber.c on Win32; CreateFiber
    # specifically is what core/ is forbidden from touching).
    #
    # The address regex matches Spyro-specific addresses like 0x80012204
    # AND generic PSX bus constants like 0x80000000 (KSEG0 base). We only
    # care about the former, so allow `#define`d named constants through —
    # OPENPETE_PSX_KSEG0_BASE etc. live in openpete_psx.h precisely so
    # core/ doesn't carry bare hex literals for them. grep-pipe-grep keeps
    # the check under POSIX bash without needing PCRE lookarounds.
    add_custom_target(openpete_core_purity_check ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking core/ purity..."
        COMMAND bash -c "! grep -rE '0x800[0-9a-fA-F]{4}' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' | grep -vE '^[^:]+:[[:space:]]*#[[:space:]]*define'"
        COMMAND bash -c "! grep -rE 'CreateFiber|<windows.h>' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' --exclude='crash.c' --exclude='openpete_time.c' --exclude='openpete_frame_time_log.c'"
        COMMENT "core/ purity check: no Spyro addresses, no Win32-only APIs"
        VERBATIM)
endif()
