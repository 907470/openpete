// games/spyro-2/main.cpp — Spyro 2: Ripto's Rage! binary entry point.
//
// Parse options, attach the recompiled module, hand off to core's run loop.
// SDL3's SDL_main.h re-#defines main -> SDL_main and supplies the platform
// entry shim; it must be included by the TU defining main().

#include <SDL3/SDL_main.h>
#include "openpete.h"
#include "openpete_config.h"
#include "openpete_log.h"
#include "game_extras.h"
#include "openpete_recomp_module.h"

#include <cstdio>

extern "C" const openpete_recomp_module spyro2_recomp_module;

int main(int argc, char** argv) {
    /* abort() does not flush, and the PSYQ stubs use it to surface an
     * unimplemented surface. Unbuffer so that diagnostic survives. */
    setvbuf(stderr, NULL, _IONBF, 0);

    openpete_options_t opts;
    openpete_options_init(&opts);
    opts.display_name = game_get_display_name();
    /* No native renderer walks exist for this game: the native path would draw
     * its geometry with Spyro 1's decode, so PsyCross is what it starts with.
     * An explicit --renderer still wins, since this runs before parsing. */
    openpete_options_set_default_renderer(&opts, OPENPETE_RENDERER_PSYCROSS);
    opts.run_recomp_hint_addr = 0x80011ADCu;

    openpete_config_set_game_id(game_get_id());

    int rc = openpete_options_parse(&opts, argc, argv);
    if (rc != OPENPETE_EXIT_OK) return rc;
    if (opts.config_report) { openpete_config_report(stdout); return 0; }

    openpete_log_init(opts.log_format, opts.log_level_spec);
    openpete_log_file_open(opts.log_file);

    {
        int src_rc = openpete_options_require_exe_source(&opts);
        if (src_rc != OPENPETE_EXIT_OK) return src_rc;
    }

    /* The runtime reaches recompiled code only through this descriptor. */
    if (openpete_recomp_register(&spyro2_recomp_module) != 0) return 1;

    /* gp is set by PSYQ's _96_init boot stub, which is not recompiled, and
     * main()'s prologue assumes it already holds. Without it every
     * gp-relative store lands near PSX address 0 instead of the real global.
     * The value is _gp from the decomp's psx.map. --test-recomp-gp overrides. */
    if (opts.run_recomp_addr != 0 && opts.test_recomp_gp == 0) {
        opts.test_recomp_gp = 0x80066D38u;
    }

    return openpete_run(&opts);
}
