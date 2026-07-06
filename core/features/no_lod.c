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
int openpete_no_lod_producers = 0;
float openpete_fog_scale = 1.0f;

static no_lod_setup_fn s_setup_hook = NULL;

void no_lod_set_setup_hook(no_lod_setup_fn fn) {
    s_setup_hook = fn;
}

int no_lod_apply_options(const openpete_options_t* opts) {
    openpete_no_lod = 0;
    openpete_no_lod_producers = 0;
    /* Resolve the fog-distance multiplier before the no-lod early-out:
     * explicit --fog-scale wins; otherwise --no-lod defaults to the
     * classic stretched curve (4x) and stock to the authentic PSX one. */
    if (opts) {
        openpete_fog_scale = (opts->fog_scale > 0.0f) ? opts->fog_scale
                             : (opts->no_lod ? 4.0f : 1.0f);
    }
    if (!opts || !opts->no_lod) return 0;

    /* Native (vk/ab/diff): display-policy only — the .s verdict producers
     * run pure canonical 4:3 (Phase 6 stack-revert step 2, sibling of the
     * widescreen cull flip). The game-side arming below (relocated/grown
     * queues, distance/tier/zsf4 saturation hooks, LP-face knob, polybuf
     * grow) exists to make the ORIGINAL renderer draw full detail; under
     * the native renderer that output is headless verdict-only, and the
     * native feeders derive no-LOD from gamestate themselves (moby far-
     * record builder + zsf4/tier neutralization, terrain HQ classifier,
     * portal always-dome). */
    if (opts->renderer != OPENPETE_RENDERER_PSYCROSS) {
        openpete_no_lod = 1;
        OPL_INFO_F("no-lod", "%s",
                   "native renderer: --no-lod is a display policy (native "
                   "feeders own detail selection); game-side producer stack "
                   "not armed — producers run canonical");
        return 0;
    }

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
    openpete_no_lod_producers = 1;
    return 0;
}
