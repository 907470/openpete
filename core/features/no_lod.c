// core/features/no_lod.c — see features/no_lod.h.
//
// 2026-07-06: the game-side PRODUCER stack (openpete_no_lod_producers —
// relocated/grown env+moby queues, distance/tier/zsf4 saturation hooks,
// LP-face knob, polybuf grow) was DELETED with the PsyCross-mode
// enhancement stacks (Phase 6, openpete-native-renderer.md). --no-lod is
// a native-renderer display policy: the native feeders derive full
// detail from gamestate (moby far-record builder + zsf4/tier
// neutralization, terrain HQ classifier, portal always-dome, fog
// default). In PsyCross reference mode the flag warns and is ignored —
// the reference renders canonical PSX detail.

#include "features/no_lod.h"

#include "openpete_log.h"

#include <stddef.h>

/* Definition of the global. The psx_runtime.h declaration resolves to
 * this symbol. */
int openpete_no_lod = 0;
float openpete_fog_scale = 1.0f;

int no_lod_apply_options(const openpete_options_t* opts) {
    openpete_no_lod = 0;
    /* Resolve the fog-distance multiplier before the no-lod early-out:
     * explicit --fog-scale wins; otherwise --no-lod defaults to the
     * classic stretched curve (4x) and stock to the authentic PSX one. */
    if (opts) {
        openpete_fog_scale = (opts->fog_scale > 0.0f) ? opts->fog_scale
                             : (opts->no_lod ? 4.0f : 1.0f);
    }
    if (!opts || !opts->no_lod) return 0;

    if (opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        OPL_WARN_F("no-lod", "%s",
                   "--no-lod is native-renderer-only (the game-side "
                   "producer stack was retired); the PsyCross reference "
                   "renders canonical PSX detail — flag ignored");
        return 0;
    }

    openpete_no_lod = 1;
    OPL_INFO_F("no-lod", "%s",
               "--no-lod: native display policy (native feeders own "
               "detail selection; producers run canonical)");
    return 0;
}
