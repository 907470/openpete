# core/CMakeLists.txt — game-agnostic OpenPete runtime.
#
# Builds a static lib `openpete_core` consumed by per-game targets under
# games/<game>/. Anything Spyro-specific belongs in games/spyro-1/, not
# here. See notes/plans/openpete-pc-port.md.

set(PSXRECOMP_RUNNER ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/runner)

add_library(openpete_core STATIC
    # Diagnostics: the log and its file mirror, the crash handler, the
    # pacing event log (--pace-log PATH) with the always-on pacing census,
    # the per-run perf summary, the CPU+GPU performance trace
    # (--profile-out PATH), the per-function tracing hooks (fire only when
    # a TU is built with -finstrument-functions, -DOPENPETE_FUNC_TRACE),
    # the sub-tick spine identity audit (OPENPETE_IDENT_AUDIT: composes
    # the canonical scene and the alpha-1 re-extract of the same tick and
    # reports their pixel difference), the per-present frame-time recorder
    # (--frame-time-log PATH), and the psxtrace schedule and verifier.
    diag/log.c
    diag/log_file.c
    diag/crash.c
    diag/pace_log.c
    diag/perf_stats.c
    diag/profile.c
    diag/func_trace.c
    diag/ident_audit.c
    diag/openpete_frame_time_log.c
    diag/psxtrace_schedule.c
    diag/psxtrace_verify.c

    # Configuration: the command-line flag parser, openpete.toml (the
    # layered config file, resolved into an argv splice and a setenv pass;
    # config.c is SDL-free so the resolver can be unit-tested standalone in
    # tests/unit/config_resolve_test.c), the knob registry behind the M-key
    # overlay, and the data-directory layout (the game folder for a
    # portable install, the platform's per-user directories otherwise;
    # SDL-free for the same reason config.c is).
    config/options.c
    config/config.c
    config/config_schema.c
    config/knob_registry.c
    config/paths.c

    # Host input.
    # [keys]: the input binding registry. Split the same way and for the
    # same reason as config.c — keymap.c holds the registry, the layered
    # store and the edge latches with no SDL in sight (so it links into
    # tests/unit/keymap_test.c), and keymap_sdl.c is the SDL glue that
    # turns binding names into codes and reads whether they are held.
    input/keymap.c
    input/keymap_sdl.c
    # Host-owned pad decode: producer publishes an SDL-decoded snapshot, the
    # sim-side consumer arbitrates it against the other override writers and
    # stamps the PsyCross override channel. Split producer/consumer because
    # the pump and the sim run on different threads. See
    # openpete_host_input.h.
    input/host_input.c
    # The host mouse channel: event-derived state, running totals, ownership
    # arbitration, capture requests and the injection seam. See
    # openpete_host_mouse.h.
    input/host_mouse.c
    # Controller vibration. rumble_curve.c is the measured actuator
    # curve, kept SDL-free so the unit test can link it alone; host_rumble.c is
    # the registration / per-tick sample / per-pump apply seam.
    input/rumble_curve.c
    input/host_rumble.c
    # Per-VBL input log — single source of truth for "what was pressed at
    # VBL i"; shared by bk2_record and the rewind/runahead drivers. See
    # openpete_input_log.h.
    input/input_log.c
    renderer/subtick_view.c
    renderer/screenshot.c
    runtime/run.c

    # Recording and savestates: true-VFR gameplay video capture (--record;
    # links libav* in-process and reads the present FBO so output is
    # bars-free at internal resolution), BizHawk .bk2 movie replay (--bk2)
    # and record (--bk2-record), save states (F2/F4 quick save/load), the
    # rewind keyframe ring (--rewind N seconds; Backspace scrubs back;
    # driver in run.c, see openpete_rewind.h), and the guest-state
    # discontinuity broadcast (host caches derived from guest RAM register
    # resync actions fired after a savestate load; see
    # openpete_discontinuity.h).
    record/record.c
    record/bk2_movie.c
    record/bk2_record.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c
    record/savestate.c
    record/rewind.c
    record/discontinuity.c

    # The mod install's checkpoint, exec and restore path.
    runtime/handoff.c

    # The PSX machine model: RAM and the CPUState bindings, the EXE boot
    # sequence, dispatch glue and the hang watchdog, the BIOS model, the
    # CD-ROM controller, the disc cache, the EXE loader, the RAM
    # virtualization region table, the moby vertex-stream resolver and the
    # cooperative scheduler globals.
    psx/cpu.c
    psx/boot.c
    psx/dispatch.c
    psx/psx_bios.c
    psx/cdrom_mmio.c
    psx/disc_cache.c
    psx/exe_loader.cpp
    psx/psx_region.c
    psx/vstream.c
    psx/fiber.c

    renderer/openpete_ui_moby.c

    # Debug and in-game UI: the O settings menu; the placement ledger for
    # on-screen chrome (six anchors, a usable-x range that dodges the open
    # settings panel, a per-anchor stacking cursor so two surfaces never
    # claim one corner; see openpete_overlay_anchor.h); OSD toast
    # notifications (the ring, coalescing, rate limit and animation state
    # machine on an injected clock; drawing lives in native_render.c and
    # text_moby.c; see openpete_osd.h); the M-key dev config overlay (the
    # ImGui panel over the knob registry, C++ TU, flat C API in
    # openpete_devui.h; native-vk-only, live presents only); the G camlock
    # and F freecam tools; and the top-left FPS overlay drawn via direct
    # OpenGL during PsyCross's post-render hook (g_psyx_postRenderOverlay).
    ui/openpete_menu.c
    ui/overlay_anchor.c
    ui/openpete_osd.c
    ui/devui.cpp
    ui/camlock.c
    ui/freecam.c
    ui/fps_overlay.cpp

    renderer/fillpick.c
    runtime/feature_registry.c

    # Modding framework P1: per-address override chains riding the
    # psx_override_dispatch prologue chokepoint + compile-on-load mod
    # loader (mod.toml manifests via vendored tomlc99, POSIX dlopen).
    # See notes/plans/openpete-modding-framework.md.
    mods/mod_chain.c
    mods/mod_loader.c
    mods/mod_draw.c
    mods/mod_services.c
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

    runtime/present_thread.c
    renderer/native_terrain_cache.c

    # Render-side state interpolation snapshot ring (game-agnostic;
    # regions supplied by games/<game>/). Capture-only in v1; lerp +
    # re-emission lands in fps-unlock Phase 2a. See
    # notes/plans/openpete-fps-unlock.md.
    renderer/openpete_interp.c

    # Per-object prev-state snapshot table (the INDIRECT counterpart to the
    # singleton interp ring above). Keyed by object identity (game-supplied
    # walk); double-buffered for sub-tick lerp/re-feed. Step 1 of the
    # native-renderer high-FPS plan. See notes/plans/openpete-native-renderer.md.
    renderer/openpete_interp_objs.c

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

    # Native Vulkan renderer. Host-side display consumer built on core/rhi;
    # never writes PSX RAM.
    renderer/native_render.c
    renderer/native_vram.c

    # Per-feature modules. See notes/plans/openpete-feature-modules.md.
    renderer/no_lod.c
    renderer/widescreen.c

    # Music WAV override: play user PCM WAVs from assets/music/ in place
    # of XA music. See notes/plans/openpete-music-wav-override.md.
    host/host_spawn.c
    host/music_override.c
    host/mod_sound.c
    host/sha256.c
    host/sfx_ident.c
    host/sfx_pack.c
    host/texture_pack.c
    host/wav_loader.c

    # Host-backed PSX memory card (save/load).
    # See notes/plans/openpete-memcard-host-backed.md.
    host/memcard.c

    # Data-parallel extraction worker pool (--extract-threads). See
    # openpete_workpool.h for the threading contracts (C1-C5).
    runtime/work_pool.c

    # Pacing-clock lifecycle + the fast-forward flag. The host loop paces
    # itself against op_sleep_until deadlines; there is no pacing thread.
    runtime/ticker.c

    # Phase 1e infra-triangle: hardcoded POLY_F3 emit to validate the full
    # PsyCross render pipeline without depending on overlay-resident Spyro
    # code. C++ TU because PsyCross's libgpu.h uses static_assert.
    renderer/debug_render.cpp

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

# Build identity for the pacing log's environment block. Baselines are only
# comparable within a matching environment, and "which build produced this"
# is the field that decides it. Resolved at configure time, so a rebuild
# without a reconfigure reports the revision the tree was configured at --
# which is why pacing baselines carry a capture timestamp as well.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OPENPETE_GIT_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT OPENPETE_GIT_REV)
    set(OPENPETE_GIT_REV "unknown")
endif()
target_compile_definitions(openpete_core
    PRIVATE OPENPETE_BUILD_ID="${OPENPETE_GIT_REV}")

# Native renderer foundation: the thin RHI + Vulkan backend (core/rhi).
# Always available (added unconditionally by the top-level CMakeLists);
# renderer/native_render.c is the only consumer for now. PUBLIC so the RHI's
# headers (and its embedded SPIR-V) reach any downstream renderer TUs.
target_link_libraries(openpete_core PUBLIC openpete_rhi)

# ImGui (third_party/imgui, pinned release tag) — the M-key dev config
# overlay's UI library (ui/devui.cpp). SDL3 + Vulkan backends; the Vulkan
# backend rides volk's already-loaded function pointers
# (IMGUI_IMPL_VULKAN_USE_VOLK — openpete_rhi runs volkInitialize/volkLoadInstance
# at rhi_create, and volk.c is compiled ONLY there, so no duplicate symbols).
# See notes/plans/openpete-imgui-config-overlay.md.
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

# dear_bindings (third_party/dear_bindings, generated output vendored from the
# release matching the pinned ImGui tag) is the C API mods call. The two must
# describe the same ImGui: a stale dcimgui.h would declare functions the
# library does not define and lay out ImGuiIO differently, so the version is
# compared here rather than discovered at link or at runtime. Refresh with
# scripts/update-dear-bindings.sh.
set(DCIMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/dear_bindings)
file(STRINGS ${DCIMGUI_DIR}/dcimgui.h _dcimgui_ver_line
     REGEX "^#define +IMGUI_VERSION_NUM +[0-9]+")
file(STRINGS ${IMGUI_DIR}/imgui.h _imgui_ver_line
     REGEX "^#define +IMGUI_VERSION_NUM +[0-9]+")
string(REGEX MATCH "[0-9]+" _dcimgui_ver "${_dcimgui_ver_line}")
string(REGEX MATCH "[0-9]+" _imgui_ver "${_imgui_ver_line}")
if(NOT _dcimgui_ver OR NOT _imgui_ver OR NOT _dcimgui_ver STREQUAL _imgui_ver)
    message(FATAL_ERROR
        "ImGui version mismatch: third_party/dear_bindings/dcimgui.h "
        "reports IMGUI_VERSION_NUM '${_dcimgui_ver}', "
        "third_party/imgui/imgui.h reports '${_imgui_ver}'. "
        "Run scripts/update-dear-bindings.sh.")
endif()

# The mod ImGui table: a function-pointer table over the dear_bindings API, generated
# from its JSON metadata. openpete_imgui_table.h and openpete_imgui_macros.h
# are staged into the mod SDK; openpete_imgui_guard.cpp is the engine side,
# one call-site-checking wrapper per entry.
set(_imgui_table_dir ${CMAKE_CURRENT_BINARY_DIR}/imgui_table)
set(_imgui_table_outputs
    ${_imgui_table_dir}/openpete_imgui_table.h
    ${_imgui_table_dir}/openpete_imgui_macros.h
    ${_imgui_table_dir}/openpete_imgui_guard.cpp)
# The two headers are staged into the mod SDK from games/<game>/.
set(OPENPETE_IMGUI_TABLE_DIR ${_imgui_table_dir} CACHE INTERNAL
    "Generated ImGui table headers")
add_custom_command(
    OUTPUT  ${_imgui_table_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_imgui_table_dir}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_imgui_table.py
            --json ${DCIMGUI_DIR}/dcimgui.json
            --out-dir ${_imgui_table_dir}
    DEPENDS ${DCIMGUI_DIR}/dcimgui.json
            ${CMAKE_SOURCE_DIR}/tools/gen_imgui_table.py
    COMMENT "Generating the mod ImGui table (gen_imgui_table.py)"
    VERBATIM)

add_library(openpete_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    # imgui_demo.cpp defines the five ImGui::Show* functions dcimgui.cpp
    # references; without it the exe does not link.
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${DCIMGUI_DIR}/dcimgui.cpp
    ${_imgui_table_dir}/openpete_imgui_guard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/imgui_guard.c
)
target_include_directories(openpete_imgui PRIVATE
    ${DCIMGUI_DIR}
    ${_imgui_table_dir}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ui)
# The Linux exe links -rdynamic. Without this, a prebuilt mod object built
# against raw dcimgui.h — no table, no version check, no guard — would resolve
# ImGui_Begin to the exe at dlopen and drive the context directly, while the
# same object fails to load on Windows. Hidden on both, so it fails on both.
set_source_files_properties(
    ${DCIMGUI_DIR}/dcimgui.cpp
    ${_imgui_table_dir}/openpete_imgui_guard.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fvisibility=hidden>")
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

# libav* for --record gameplay video capture (record/record.c). On a native
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
    target_sources(openpete_core PRIVATE psx/pcsxr_gte_stub.c)
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
    #   - config/paths.c      GetModuleFileNameA (the exe directory, which
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
        COMMAND bash -c "! grep -rE 'CreateFiber|<windows.h>' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' --exclude='crash.c' --exclude='openpete_time.c' --exclude='openpete_frame_time_log.c' --exclude='mod_loader.c' --exclude='host_spawn.c' --exclude='paths.c' --exclude='log_file.c'"
        COMMENT "core/ purity check: no Spyro addresses, no Win32-only APIs"
        VERBATIM)

    # The display seam has ONE owner (the present thread while eager is
    # active, the host loop otherwise). scripts/fences/seam_guard.py refuses
    # new frame/present call sites outside the owner and window ops off the
    # window's thread — the build-time half of the fence; the runtime half
    # (rhi_display_claim + the per-entry owner check) lives in rhi_vulkan.c.
    add_custom_target(openpete_seam_guard ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking display-seam ownership..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/seam_guard.py ${CMAKE_SOURCE_DIR}
        COMMENT "display-seam fence: presents and window ops only from their owner"
        VERBATIM)

    # A scene is built for a PRESENT instant. scripts/fences/clock_guard.py
    # refuses a wall-clock read (op_now / SDL_GetTicks* / clock_gettime) in
    # the scene-producer files unless the line says `clock-guard: wall`: the
    # interpolation alpha is (present clock - tick stamp) / period, and a
    # channel phased by op_now() sits at alpha~0 under eager production and
    # flakes under deterministic capture — both invisible to every gate.
    add_custom_target(openpete_clock_guard ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking scene-producer clocks..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/clock_guard.py ${CMAKE_SOURCE_DIR}
        COMMENT "clock fence: scene producers read the present clock, not the wall clock"
        VERBATIM)

    # The moby frame word's 21-bit stream field is decoded by one idiom at
    # 21 sites; each is substituted with the stream-handle resolver. A
    # missed reader walks garbage vertices silently, so the census over the
    # GENERATED C (opcode words, not C text) and the manifest must agree,
    # and every row's line must carry the substitution (a stale regen runs
    # the stock decode).
    add_custom_target(openpete_vstream_sites ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking frame-word field readers..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/vstream_sites.py ${CMAKE_SOURCE_DIR}
        COMMENT "vstream fence: every 21-bit field reader is substituted, at its srl"
        VERBATIM)

    # Launcher hygiene, the two silent-failure defaults. A scripted run must
    # silence the pad (nobody is holding it, and the buzzing reaches no log),
    # and a script that sets OPENPETE_* knobs for a Windows .exe must name
    # WSLENV or the knob never crosses and the run measures the default.
    # Both run on every build, since a scanner run only by hand
    # is not enforced.
    add_custom_target(openpete_launch_guards ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking launcher hygiene..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/rumble_guard.py ${CMAKE_SOURCE_DIR}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/wslenv_guard.py ${CMAKE_SOURCE_DIR}
        # The keymap's KM_LEVEL / KM_DELTA flags against their consumer sites:
        # the parser enforces them for mouse sources and the unit test cannot
        # see the consumers.
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/keymap_flags.py ${CMAKE_SOURCE_DIR}
        COMMENT "launcher fences: silence the pad, make env knobs cross into WSL, keymap flags match consumers"
        VERBATIM)
endif()
