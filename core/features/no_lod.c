// core/features/no_lod.c — see features/no_lod.h.
//
// Owns the openpete_no_lod flag and the apply_options entry. All
// Spyro-specific knob storage + region allocation lives in
// games/spyro-1/features/no_lod.c, reached through the
// no_lod_set_setup_hook callback below.

#include "features/no_lod.h"

#include "openpete_log.h"

#include <stddef.h>
#include <stdio.h>

/* Definition of the global. Storage previously lived in
 * core/runtime/fiber.c next to the cooperative-scheduler state; moved
 * here as part of the no-lod feature isolation. The
 * third_party/psxrecomp/runner/include/psx_runtime.h declaration still
 * resolves to this symbol. */
int openpete_no_lod = 0;

static no_lod_setup_fn s_setup_hook = NULL;

void no_lod_set_setup_hook(no_lod_setup_fn fn) {
    s_setup_hook = fn;
}

int no_lod_apply_options(const openpete_options_t* opts) {
    openpete_no_lod = 0;
    if (!opts || !opts->no_lod) return 0;

    if (!s_setup_hook) {
        OPL_ERROR_F("no-lod", "%s",
                    "--no-lod requested but no per-game setup hook "
                    "registered (feature_list missing the module?)");
        return 1;
    }
    if (s_setup_hook() != 0) {
        OPL_ERROR_F("no-lod", "%s", "per-game --no-lod setup failed");
        return 2;
    }
    openpete_no_lod = 1;
    return 0;
}
