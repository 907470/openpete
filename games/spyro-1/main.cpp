// games/spyro-1/main.cpp — Spyro 1 binary entry point.
//
// Tiny wrapper: parse opts, set the display name, hand off to core's run loop.
// SDL3's SDL_main.h re-#defines main → SDL_main and supplies the platform
// entry shim (links SDL3::SDL3main); must be included by the TU with main().

#include <SDL3/SDL_main.h>
#include "openpete.h"
#include "openpete_config.h"
#include "openpete_log.h"
#include "openpete_keymap.h"
#include "game_extras.h"
#include "openpete_mods.h"
#include "recomp_attach.h"
#include "mod_wad_overlay.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
/* Distribution Windows builds link as a GUI subsystem app (-mwindows) so
 * no console window pops up when an end user double-clicks. But when the
 * exe is launched from a terminal or with redirected output — cmd.exe, a
 * `> log` redirect, or WSL2 interop (which passes inherited *pipe*
 * handles, not a console) — we still want logs to flow. So: attach to a
 * parent console if there is one, then bind the C stdio streams to
 * whatever the standard OS handles actually point at (console, pipe, or
 * file). Harmless no-op for console-subsystem (dev) builds. */
static void reconnect_std(DWORD which, FILE* stream) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    int target = _fileno(stream);

    /* Leave a stream that is ALREADY wired to this handle alone. A
     * console-subsystem build — and any build whose CRT inherited its
     * handles, which is every WSL-interop launch — arrives here with
     * working stdio, and "reconnecting" it is destructive: _open_osfhandle
     * takes a SECOND CRT reference to the same HANDLE, then _dup2 closes
     * the destination fd first, which shuts that one handle for both
     * references. The stream is left pointing at a closed handle and every
     * write is silently dropped. That is what made stdout-based output
     * (--config-report, --keys-report) vanish while stderr-based output
     * (--help, the log) kept working. */
    if (target >= 0 && (HANDLE)_get_osfhandle(target) == h) {
        setvbuf(stream, NULL, _IONBF, 0);
        return;
    }

    int fd = _open_osfhandle((intptr_t)h, _O_TEXT);
    if (fd == -1) return;
    if (fd != target) { _dup2(fd, target); _close(fd); }
    setvbuf(stream, NULL, _IONBF, 0);
}
static void openpete_attach_parent_console(void) {
    AttachConsole(ATTACH_PARENT_PROCESS);   /* ok if it fails (pipe/file) */
    reconnect_std(STD_OUTPUT_HANDLE, stdout);
    reconnect_std(STD_ERROR_HANDLE,  stderr);
    reconnect_std(STD_INPUT_HANDLE,  stdin);
}
#else
static void openpete_attach_parent_console(void) {}
#endif

extern "C" int spyro1_dump_sfx_pack(const char* cue_path, const char* out_dir,
                                    const char* extra_wads);
extern "C" int spyro1_extract_recomp_inputs(const char* cue_path,
                                            const char* out_dir);
extern "C" int spyro1_config_disc_identity(const char* disc_path,
                                           char* serial, size_t serial_cap,
                                           char* sha, size_t sha_cap);
extern "C" int spyro1_library_resolve(openpete_options_t* opts,
                                      char* module_path_out,
                                      size_t module_path_cap);
extern "C" void spyro_state_regions_register(void);
/* Mod SDK name→vaddr table, generated from manifest/symbols.toml by
 * tools/gen_mod_sdk.py into the build dir (sdk_symbols_table.c). */
extern "C" {
extern const openpete_sdk_symbol_t openpete_sdk_symbols[];
extern const size_t                openpete_sdk_symbol_count;
/* Draw-service text rasterizer (features/text_moby_native.c — that header
 * has no C++ guards, so declare the one symbol we need directly). */
int text_moby_native_mod_emit(const char* str, float x, float y,
                              float cap_h, unsigned rgb,
                              void* out_verts, int cap);
/* Actor render service (features/spyro_native.c, rung 16): pose capture +
 * recorded-pose ghost emit, registered with the core draw service below. */
int spyro1_actor_pose_capture(int kind, void* buf, int cap);
int spyro1_actor_emit(CPUState* cpu, int kind, const void* pose, int len,
                      uint32_t tint_rgba);
/* Moby-type registry game ops (features/mod_moby_ops.c, rung 11). */
void spyro1_moby_ops_register(void);
/* Level-slot registry game ops (features/mod_level_ops.c, rung 12). */
void spyro1_level_ops_register(void);
}
#include "splash.h"
extern "C" {
extern void (*spyro1_progress_cb)(const char* stage, int done, int total);
}
#include "spyro_interp_regions.h"
#include "spyro_native_reextract.h"

#include <cstdio>

int main(int argc, char** argv) {
    /* Reconnect stdout/stderr to the parent terminal on a GUI-subsystem
     * (distribution) Windows build; no-op elsewhere. Before any output. */
    openpete_attach_parent_console();

    /* MinGW buffers stderr when it isn't a TTY (true for `>file 2>&1`
     * and for WSL2 interop pipes). abort() — which the PSYQ stubs use to
     * surface unimplemented-surface hits — doesn't flush, so otherwise
     * useful diagnostics get dropped right before exit. Unbuffer once at
     * startup. Linux behaviour is unchanged (line-buffered → unbuffered
     * is fine for stderr). */
    setvbuf(stderr, NULL, _IONBF, 0);

    openpete_options_t opts;
    openpete_options_init(&opts);
    opts.display_name = game_get_display_name();
    /* Hint for the parser's "missing --run-recomp" diagnostic. main()
     * lives at 0x80012204 in spyro-1/build/psx.map; that's what the user
     * almost always wants to dispatch. */
    opts.run_recomp_hint_addr = 0x80012204u;

    /* openpete.toml scopes, registered before the parse: the config stack
     * is resolved inside openpete_options_parse and needs to know which
     * game it is configuring and how to identify the disc that will boot. */
    openpete_config_set_game_id(game_get_id());
    openpete_config_set_disc_identity_fn(spyro1_config_disc_identity);

    int rc = openpete_options_parse(&opts, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;

    /* Config utility modes: the stack is resolved, so answer and exit. */
    if (opts.config_report) { openpete_config_report(stdout); return 0; }
    if (opts.keys_report)   { openpete_keymap_report(stdout); return 0; }
    if (opts.config_convert) return openpete_config_convert_legacy();

    /* Bring the logger and its file mirror up before the recomp module is
     * attached. First boot builds that module, and a build that fails is
     * exactly the report we need a file for — opening the sink inside
     * openpete_run() would be too late to record it. */
    openpete_log_init(opts.log_format, opts.log_level_spec);
    if (openpete_log_file_open(opts.log_file) == 0)
        OPL_INFO_F("log", "log file: %s (build %s %s)",
                   openpete_log_file_path(), __DATE__, __TIME__);

    /* Mod framework: hand the loader this game's symbol table so mods
     * override by name ("CameraUpdate") instead of raw vaddr. The loader
     * itself runs from openpete_run() after all setup. */
    openpete_mods_set_symbol_table(openpete_sdk_symbols,
                                   openpete_sdk_symbol_count);
    /* ...and the draw service's text rasterizer (Spyro's 3D gold
     * text-moby glyphs — the same font as the FPS overlay). */
    openpete_mods_draw_set_text_fn(text_moby_native_mod_emit);
    /* ...and the actor render service (rung 16): live-pose capture +
     * recorded-pose ghost emit through the native Spyro extractor. */
    openpete_mods_draw_set_actor_fns(spyro1_actor_pose_capture,
                                     spyro1_actor_emit);
    /* ...and the content registries' game ops (rungs 11+12). */
    spyro1_moby_ops_register();
    spyro1_level_ops_register();
    /* Level-segment asset overlay (rung 9): needs the disc image to read
     * the WAD TOC (lazy — first CdRead). */
    spyro1_mod_wad_configure(opts.disc_path);

    /* Distribution D2: standalone extraction stage — carve PSX.EXE +
     * overlay blobs from the disc and exit (also the first stage of the
     * first-boot pipeline). Runs before module attach: extraction must
     * work when no module exists yet. */
    if (opts.extract_recomp_inputs_dir) {
        return spyro1_extract_recomp_inputs(opts.disc_path,
                                            opts.extract_recomp_inputs_dir)
                   ? 1 : 0;
    }

    /* Build-time mod compile (--compile-mods): compile every mod in the
     * resolved mods dir against <exe>/sdk and exit — the cmake
     * `spyro1_mods` step for native presets; also pre-warms a dist
     * install's compile-on-boot cache. Needs no disc/library/window. */
    if (opts.compile_mods)
        return openpete_mods_compile_all(&opts) == 0 ? 0 : 1;

    /* sfx pack census (--dump-sfx-pack): walk the disc's WAD (+ custom
     * level files) and exit. Needs the disc only. */
    if (opts.dump_sfx_pack_dir)
        return spyro1_dump_sfx_pack(opts.disc_path, opts.dump_sfx_pack_dir,
                                    opts.dump_sfx_wads) == 0 ? 0 : 1;

    /* Distribution D2.5: resolve disc + module through the
     * content-addressed library (--ingest copies an image in; no disc
     * at all boots the most recent library entry). Explicit --disc
     * bypasses the library entirely (dev/CI path). The splash renders
     * the waits (ingest copy, first-boot build), the version selector,
     * and the drag-a-disc first-run screen; it lazy-opens only when
     * one of those is actually needed. */
    spyro1_splash_set_enabled(!opts.headless);
    /* Open at the game window's size (an explicit --window carries
     * through) so the vk-mode window adoption below needs no resize. */
    spyro1_splash_set_window_size(opts.window_w, opts.window_h);
    spyro1_progress_cb = spyro1_splash_progress;

    static char library_module_path[4096];
    if (spyro1_library_resolve(&opts, library_module_path,
                               sizeof library_module_path) != 0)
        return 1;

    if (!opts.disc_path && !opts.exe_path && !opts.headless) {
        /* Nothing to boot anywhere: first run. DROP mode. */
        if (spyro1_splash_open() == 0) {
            const char* dropped = spyro1_splash_wait_for_disc();
            if (!dropped) return 0;   /* user closed the window */
            opts.ingest_path = dropped;
            if (spyro1_library_resolve(&opts, library_module_path,
                                       sizeof library_module_path) != 0)
                return 1;
        }
    }

    {
        int src_rc = openpete_options_require_exe_source(&opts);
        if (src_rc != OPENPETE_EXIT_OK) return src_rc;
    }

    /* Distribution D0/D1: the runtime reaches recompiled code only
     * through the recomp-module descriptor. Static builds register the
     * linked-in module; shared-linkage builds dlopen spyro1-recomp and
     * resolve the same descriptor. Must precede any dispatch. */
    const char* module_override = opts.recomp_module_path;
    if (!module_override && library_module_path[0])
        module_override = library_module_path;
    /* Auto-building into the module location is fine for the default
     * path and for library entries; an explicit --recomp-module points
     * at a module the user asserts exists. */
    int allow_build = (opts.recomp_module_path == NULL);
    /* Re-point the WAD overlay at the resolved disc (the library path
     * above may have filled opts.disc_path). */
    spyro1_mod_wad_configure(opts.disc_path);
    if (spyro1_recomp_attach(module_override, opts.disc_path, allow_build) != 0)
        return 1;

    /* Mod compile-on-boot (openpete-mod-compile-on-boot): compile any
     * stale enabled source mods behind the splash while it is still
     * alive — run_task lazy-opens the window only if there is real work,
     * so a boot with nothing stale never flashes it. Loading stays in
     * openpete_run(); this pass only warms the build cache. */
    openpete_mods_prepare(&opts, spyro1_splash_progress,
                          spyro1_splash_run_task);

    /* In-process continuation: the splash's job ends where the game
     * window begins. In the default windowed vk mode the game ADOPTS the
     * splash window (native_render retitles + reuses it) so the window
     * never closes and reopens; PsyCross/ab/diff create their own windows
     * and close the splash as before. Progress is unhooked either way —
     * nothing may lazy-reopen a splash window mid-game. */
    spyro1_progress_cb = NULL;
    if (opts.renderer == OPENPETE_RENDERER_VK && !opts.headless)
        opts.adopted_window = spyro1_splash_detach_window();
    else
        spyro1_splash_close();

    /* fps-unlock Phase 2: register the interp ring's region table. Must
     * happen before openpete_run() so openpete_interp_init() (called
     * from inside the run loop) sees the regions. Explicit registration
     * instead of the weak-symbol pattern used by openpete_state_regions
     * — MinGW's PE/COFF linker mis-resolves weak overrides in this
     * configuration. */
    spyro_interp_register();
    /* Same explicit-registration rationale as spyro_interp_register()
     * (see comment above): the state-region table moved off the
     * weak-symbol pattern too. */
    spyro_state_regions_register();

    /* High-FPS spine: register the sub-tick re-feed orchestrator with the
     * native renderer so intermediate presents rebuild the scene against an
     * interpolated camera. Inert unless --renderer vk|ab|diff + --render-fps
     * exceeds the sim rate. */
    spyro_native_reextract_init();

    /* Spyro's gp comes from PSYQ's _96_init boot stub, which we don't
     * recompile (psyq___main is a no-op). main()'s prologue assumes gp is
     * already set; without it, every gp-relative store (g_PadMutex flips,
     * g_DeltaTime writes, …) goes to PSX address 0x6XX instead of the real
     * global. Hardcode Spyro's gp from spyro-1/build/psx.map (g_PadMutex
     * is at 0x80075868, encoded as gp+1540 in main, so gp = 0x80075264).
     * The user can still override via --test-recomp-gp. */
    if (opts.run_recomp_addr != 0 && opts.test_recomp_gp == 0) {
        opts.test_recomp_gp = 0x80075264u;
    }

    return openpete_run(&opts);
}
