# core/CMakeLists.txt — game-agnostic OpenPete runtime.
#
# Builds a static lib `openpete_core` consumed by per-game targets under
# games/<game>/. Anything Spyro-specific belongs in games/spyro-1/, not
# here. See notes/plans/openpete-pc-port.md.

set(PSXRECOMP_RUNNER ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner)

add_library(openpete_core STATIC
    runtime/log.c
    runtime/log_file.c
    runtime/options.c

    # openpete.toml: the layered config file that replaced the openpete.opts
    # argv splice and the openpete.env setenv pass. config.c is SDL-free so
    # the resolver can be unit-tested standalone (tests/unit/config_resolve_test.c).
    # See notes/plans/openpete-config-consolidation.md.
    runtime/config.c
    runtime/config_schema.c

    # Where the files openpete writes actually live: the game folder for a
    # portable install (the Windows default), the platform's per-user
    # directories otherwise. SDL-free for the same reason config.c is.
    runtime/paths.c

    # [keys]: the input binding registry. Split the same way and for the
    # same reason as config.c — keymap.c holds the registry, the layered
    # store and the edge latches with no SDL in sight (so it links into
    # tests/unit/keymap_test.c), and keymap_sdl.c is the SDL glue that
    # turns binding names into codes and reads whether they are held.
    runtime/keymap.c
    runtime/keymap_sdl.c

    # Host-owned pad decode: producer publishes an SDL-decoded snapshot, the
    # sim-side consumer arbitrates it against the other override writers and
    # stamps the PsyCross override channel. Split producer/consumer because
    # the pump and the sim are headed for different threads. See
    # openpete_host_input.h.
    runtime/host_input.c
    runtime/subtick_view.c
    runtime/screenshot.c
    runtime/run.c
    runtime/psxtrace_verify.c

    # True-VFR gameplay video capture (--record). Links libav* in-process;
    # reads the present FBO so output is bars-free at internal resolution.
    # See notes/plans/openpete-record-video.md.
    runtime/record.c

    # BizHawk .bk2 movie replay (--bk2) + record (--bk2-record). See
    # plans/openpete-bk2-input.md.
    runtime/bk2_movie.c
    runtime/bk2_record.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c

    # Per-VBL input log — single source of truth for "what was pressed at
    # VBL i"; shared by bk2_record and the rewind/runahead drivers. See
    # openpete_input_log.h (savestate plan P1).
    runtime/input_log.c

    # Save states (F2/F4 quick save/load). See
    # plans/openpete-savestate-frame-step-rewind.md.
    runtime/savestate.c
    runtime/handoff.c

    # Guest-state discontinuity broadcast: host caches derived from guest
    # RAM register resync actions fired after a savestate load (later:
    # rewind restore). See openpete_discontinuity.h.
    runtime/discontinuity.c

    # Rewind keyframe ring (--rewind N seconds; Backspace scrubs back).
    # Driver lives in run.c. See openpete_rewind.h (savestate plan P4).
    runtime/rewind.c

    # Phase 1b: slim PSX runtime
    runtime/cpu.c
    runtime/boot.c
    runtime/dispatch.c
    runtime/openpete_ui_moby.c
    runtime/openpete_menu.c

    # M-key dev config overlay: table-driven knob registry (C) + Dear ImGui
    # panel (C++ TU, flat C API in openpete_devui.h). Native-vk-only, live
    # presents only. See notes/plans/openpete-imgui-config-overlay.md.
    runtime/knob_registry.c
    runtime/devui.cpp

    runtime/camlock.c
    runtime/freecam.c
    runtime/fillpick.c
    runtime/feature_registry.c

    # Modding framework P1: per-address override chains riding the
    # psx_override_dispatch prologue chokepoint + compile-on-load mod
    # loader (mod.toml manifests via vendored tomlc99, POSIX dlopen).
    # See notes/plans/openpete-modding-framework.md.
    mods/mod_chain.c
    mods/mod_loader.c
    mods/mod_draw.c
    mods/mod_services.c
    mods/mod_sites.c
    mods/mod_registry.c
    mods/mod_arena.c
    mods/mod_sfx.c
    mods/mod_texture.c
    mods/mod_config.c
    mods/mod_shader.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c

    # High-resolution monotonic clock + hybrid sleep+spin waiter. Leaf
    # module (no SDL); used by ticker + dual-pacer. See
    # notes/plans/openpete-frame-pacing.md.
    runtime/openpete_time.c

    # Binary per-present frame-time recorder for the pacing harness.
    # Off by default; enabled by --frame-time-log PATH.
    runtime/openpete_frame_time_log.c

    # Empirical CPU+GPU performance trace (per present / canonical tick).
    # Off by default; enabled by --profile-out PATH. See openpete_profile.h.
    runtime/profile.c
    runtime/native_terrain_cache.c

    # Per-function tracing hooks (Tier B). Always compiled; the __cyg_profile
    # hooks fire only when a TU is built with -finstrument-functions
    # (-DOPENPETE_FUNC_TRACE, applied to the generated EXE). Inert otherwise
    # (the generated symtab TU self-registers at startup; none present = inert).
    # See openpete_func_trace.h.
    runtime/func_trace.c

    # Render-side state interpolation snapshot ring (game-agnostic;
    # regions supplied by games/<game>/). Capture-only in v1; lerp +
    # re-emission lands in fps-unlock Phase 2a. See
    # notes/plans/openpete-fps-unlock.md.
    runtime/openpete_interp.c

    # Per-object prev-state snapshot table (the INDIRECT counterpart to the
    # singleton interp ring above). Keyed by object identity (game-supplied
    # walk); double-buffered for sub-tick lerp/re-feed. Step 1 of the
    # native-renderer high-FPS plan. See notes/plans/openpete-native-renderer.md.
    runtime/openpete_interp_objs.c

    # World-space render cache. Populated per-vertex by psxrecomp's
    # gte_rtps_internal; consumed by PsyCross's OT walker (PsyX_GPU.cpp
    # MakeVertexTriangle/Quad) to recover world-space + matrix state at
    # draw time. Phase 0 foundation for draw-call matching and renderer-
    # side lerp. See notes/plans/openpete-renderer-foundation.md.

    # Draw-call matcher (renderer-foundation Phase 1). Groups OT prims
    # by matrix_span_id, hashes each batch by geometric fingerprint,
    # matches curr-frame batches against prev-frame. Produces stable
    # object_ids that Phase 2 lerp will use as identity for T/R/S
    # decompose + interpolate. No render changes in Phase 1.

    # Per-batch binary trace for the substitute pipeline. Off unless
    # --vertex-trace-log PATH is given. Consumed by
    # scripts/analyze-vtrace.py.

    # Native Vulkan renderer A/B harness (--native-ab). Host-side display
    # consumer built on core/rhi; never writes PSX RAM. Phase 1c of
    # notes/plans/openpete-native-renderer.md.
    runtime/native_render.c
    runtime/native_vram.c

    # Per-feature modules. See notes/plans/openpete-feature-modules.md.
    features/no_lod.c
    features/widescreen.c

    # Music WAV override: play user PCM WAVs from assets/music/ in place
    # of XA music. See notes/plans/openpete-music-wav-override.md.
    host/host_spawn.c
    host/music_override.c
    host/mod_sound.c
    host/sha256.c
    host/texture_pack.c
    host/wav_loader.c

    # Host-backed PSX memory card (save/load).
    # See notes/plans/openpete-memcard-host-backed.md.
    host/memcard.c

    runtime/crash.c
    runtime/exe_loader.cpp

    # PSX RAM virtualization region table — backs cold-path memory
    # resolvers. See notes/plans/openpete-ram-virtualization.md.
    runtime/psx_region.c

    # Phase 1d: cooperative scheduler globals (host/game fiber, frame counter).
    runtime/fiber.c

    # Data-parallel extraction worker pool (--extract-threads). See
    # openpete_workpool.h for the threading contracts (C1-C5).
    runtime/work_pool.c

    # Pacing-clock lifecycle + the fast-forward flag. The host loop paces
    # itself against op_sleep_until deadlines; there is no pacing thread.
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

# LIBCO_MP is REQUIRED, not an optimisation. libco/settings.h defines
# `thread_local` as NOTHING unless it is set:
#
#     #if !defined(LIBCO_MP)   /* Running in single-threaded environment */
#       #define thread_local
#
# so co_active_ (fiber.c) and co_active_handle (amd64.c) silently become
# plain globals shared by every thread. The failure that produces is not a
# subtle race: the first thread to call co_active() converts itself and
# stores its handle, and every LATER thread then sees a non-NULL handle,
# skips its own ConvertThreadToFiber, and calls SwitchToFiber while not
# being a fiber at all — an access violation inside the OS fiber
# bookkeeping. Reading the declaration is not enough to rule this out; the
# variables ARE spelled thread_local, the macro is just empty.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/third_party/libco/libco.c
    PROPERTIES COMPILE_DEFINITIONS LIBCO_MP)

target_include_directories(openpete_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${PSXRECOMP_RUNNER}/include
        ${CMAKE_SOURCE_DIR}/third_party/libco
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${CMAKE_SOURCE_DIR}/third_party/xxhash
        ${CMAKE_SOURCE_DIR}/third_party/miniz
)

# OpenAL for the music WAV override's streaming source. Same discovery
# PsyCross uses (the MinGW preset seeds the OPENAL_* cache vars; native
# Linux finds the system OpenAL).
find_package(OpenAL REQUIRED)
target_link_libraries(openpete_core PUBLIC ${OPENAL_LIBRARY})
target_include_directories(openpete_core PRIVATE ${OPENAL_INCLUDE_DIR})

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

# In-process glslang for the mod shader compile service (mod_shader.c).
# Include the repo root (for <glslang/Include/glslang_c_interface.h> and
# <glslang/Public/resource_limits_c.h>) and the generated build_info.h.
if(OPENPETE_BUILD_GLSLANG)
    target_link_libraries(openpete_core PUBLIC
        glslang glslang-default-resource-limits)
    target_include_directories(openpete_core PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_BINARY_DIR}/third_party/glslang/include)
else()
    target_compile_definitions(openpete_core PRIVATE OPENPETE_NO_GLSLANG)
endif()

# Native renderer foundation: the thin RHI + Vulkan backend (core/rhi).
# Always available (added unconditionally by the top-level CMakeLists);
# runtime/native_render.c is the only consumer for now and is inert unless
# --native-ab is passed. PUBLIC so the RHI's headers (and its embedded SPIR-V)
# reach any downstream renderer TUs. See openpete-native-renderer.md.
target_link_libraries(openpete_core PUBLIC openpete_rhi)

# Dear ImGui (third_party/imgui, pinned release tag) — the M-key dev config
# overlay's UI library (runtime/devui.cpp). SDL3 + Vulkan backends; the Vulkan
# backend rides volk's already-loaded function pointers
# (IMGUI_IMPL_VULKAN_USE_VOLK — openpete_rhi runs volkInitialize/volkLoadInstance
# at rhi_create, and volk.c is compiled ONLY there, so no duplicate symbols).
# See notes/plans/openpete-imgui-config-overlay.md.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(openpete_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
)
# volk + vulkan headers are PUBLIC: imgui_impl_vulkan.h #includes volk.h under
# IMGUI_IMPL_VULKAN_USE_VOLK, so any TU that includes the backend header (the
# dev UI) needs the same include paths and defines.
target_include_directories(openpete_imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
    ${CMAKE_SOURCE_DIR}/third_party/volk)
target_compile_definitions(openpete_imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK
    VK_NO_PROTOTYPES
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(openpete_imgui PUBLIC SDL3::SDL3-static)
set_target_properties(openpete_imgui PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
target_link_libraries(openpete_core PUBLIC openpete_imgui)

# Hide the debug/diagnostic CLI flags from `--help` (options.c only splits the
# text; every flag still PARSES and works — this is display-only). Enabled by
# the *-shared distribution presets so the distributed build shows a clean,
# user-facing flag list; OFF everywhere else (dev builds list everything).
option(OPENPETE_HIDE_DEBUG_FLAGS "Hide debug/diagnostic flags from --help output" OFF)
if(OPENPETE_HIDE_DEBUG_FLAGS)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HIDE_DEBUG_FLAGS=1)
endif()

# libav* for --record gameplay video capture (runtime/record.c). On a native
# build this is a hard dependency — see notes/plans/openpete-record-video.md
# ("hard-require" decision). On Debian/Ubuntu: libavformat-dev libavcodec-dev
# libavutil-dev libswscale-dev libswresample-dev. Min version libav* 58
# (ffmpeg 4.0).
#
# Cross-compiles (e.g. the MinGW Windows preset) are the exception: host
# pkg-config resolves the *host* (Linux) libav and injects host include paths
# (-isystem /usr/include/x86_64-linux-gnu), which poison the cross compile —
# pkg_check_modules does not honor CMAKE_FIND_ROOT_PATH. There is no target
# libav to link anyway, so when cross-compiling we compile --record out
# (record.c falls back to a stub that reports the feature unavailable).
option(OPENPETE_ENABLE_RECORD "Build --record gameplay video capture (needs libav*)" ON)
# For the windows-mingw cross build there is no system libav to link, so
# scripts/setup_mingw_ffmpeg.sh cross-builds a STATIC FFmpeg into this prefix and
# we link it here. Absent -> --record compiles to the stub (the old behaviour).
set(OPENPETE_FFMPEG_MINGW_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-mingw"
    CACHE PATH "Prefix of the cross-built static FFmpeg for windows-mingw --record")
if(NOT OPENPETE_ENABLE_RECORD)
    message(STATUS "openpete: --record disabled (OPENPETE_ENABLE_RECORD=OFF); record.c is a stub")
elseif(NOT CMAKE_CROSSCOMPILING)
    # Native (linux): host libav via pkg-config.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    target_link_libraries(openpete_core PUBLIC PkgConfig::LIBAV)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HAS_LIBAV=1)
elseif(EXISTS "${OPENPETE_FFMPEG_MINGW_PREFIX}/lib/libavcodec.a")
    # Cross (windows-mingw): static-link the FFmpeg from setup_mingw_ffmpeg.sh.
    # pkg_check_modules ignores the cross sysroot, so isolate pkg-config to OUR
    # prefix via PKG_CONFIG_LIBDIR; --static then resolves the full dep chain
    # (avformat..avutil + x264 + bcrypt/ole32/user32/m). --start-group wraps the
    # archives so inter-library refs resolve regardless of order. Static GPL
    # FFmpeg (x264) => the resulting .exe is GPL.
    find_package(PkgConfig REQUIRED)
    set(ENV{PKG_CONFIG_LIBDIR} "${OPENPETE_FFMPEG_MINGW_PREFIX}/lib/pkgconfig")
    pkg_check_modules(LIBAV REQUIRED
        libavformat libavcodec libavutil libswscale libswresample)
    target_include_directories(openpete_core PUBLIC ${LIBAV_STATIC_INCLUDE_DIRS})
    target_link_directories(openpete_core PUBLIC ${LIBAV_STATIC_LIBRARY_DIRS})
    target_link_libraries(openpete_core PUBLIC
        -Wl,--start-group ${LIBAV_STATIC_LIBRARIES} -Wl,--end-group)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HAS_LIBAV=1)
    message(STATUS "openpete: --record ENABLED for windows-mingw via static FFmpeg "
                   "at ${OPENPETE_FFMPEG_MINGW_PREFIX} (GPL: the .exe becomes GPL)")
else()
    message(STATUS "openpete: --record disabled for this cross build — no static "
                   "FFmpeg at ${OPENPETE_FFMPEG_MINGW_PREFIX} "
                   "(run scripts/setup_mingw_ffmpeg.sh to enable); record.c is a stub")
endif()

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
    #   - mods/mod_loader.c   LoadLibrary/GetProcAddress (prebuilt mod
    #                         DLL loading — Windows' dlopen analog)
    #   - runtime/paths.c     GetModuleFileNameA (the exe directory, which
    #                         is the root of the portable data layout)
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
        COMMAND bash -c "! grep -rE 'CreateFiber|<windows.h>' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' --exclude='crash.c' --exclude='openpete_time.c' --exclude='openpete_frame_time_log.c' --exclude='mod_loader.c' --exclude='host_spawn.c' --exclude='paths.c'"
        COMMENT "core/ purity check: no Spyro addresses, no Win32-only APIs"
        VERBATIM)
endif()
