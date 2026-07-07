// games/spyro-1/main.cpp — Spyro 1 binary entry point.
//
// Tiny wrapper: parse opts, set the display name, hand off to core's run loop.
// SDL3's SDL_main.h re-#defines main → SDL_main and supplies the platform
// entry shim (links SDL3::SDL3main); must be included by the TU with main().

#include <SDL3/SDL_main.h>
#include "openpete.h"
#include "game_extras.h"
#include "recomp_attach.h"

extern "C" int spyro1_extract_recomp_inputs(const char* cue_path,
                                            const char* out_dir);
extern "C" int spyro1_library_resolve(openpete_options_t* opts,
                                      char* module_path_out,
                                      size_t module_path_cap);
#include "spyro_interp_regions.h"
#include "spyro_native_reextract.h"

#include <cstdio>

int main(int argc, char** argv) {

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

    int rc = openpete_options_parse(&opts, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;

    /* Distribution D2: standalone extraction stage — carve PSX.EXE +
     * overlay blobs from the disc and exit (also the first stage of the
     * first-boot pipeline). Runs before module attach: extraction must
     * work when no module exists yet. */
    if (opts.extract_recomp_inputs_dir) {
        return spyro1_extract_recomp_inputs(opts.disc_path,
                                            opts.extract_recomp_inputs_dir)
                   ? 1 : 0;
    }

    /* Distribution D2.5: resolve disc + module through the
     * content-addressed library (--ingest copies an image in; no disc
     * at all boots the most recent library entry). Explicit --disc
     * bypasses the library entirely (dev/CI path). */
    static char library_module_path[4096];
    if (spyro1_library_resolve(&opts, library_module_path,
                               sizeof library_module_path) != 0)
        return 1;

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
    if (spyro1_recomp_attach(module_override, opts.disc_path, allow_build) != 0)
        return 1;

    /* fps-unlock Phase 2: register the interp ring's region table. Must
     * happen before openpete_run() so openpete_interp_init() (called
     * from inside the run loop) sees the regions. Explicit registration
     * instead of the weak-symbol pattern used by openpete_state_regions
     * — MinGW's PE/COFF linker mis-resolves weak overrides in this
     * configuration. */
    spyro_interp_register();

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
