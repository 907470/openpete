// core/renderer/no_lod.c - see no_lod.h.
//
// --no-lod is a native-renderer display policy with no game-side producer:
// the native feeders derive full detail from gamestate (moby far-record
// builder + zsf4/tier neutralization, terrain HQ classifier, portal
// always-dome, fog default). In PsyCross reference mode the flag warns and
// is ignored - the reference renders canonical PSX detail.

#include "no_lod.h"

#include "openpete_log.h"

#include <math.h>
#include <stddef.h>

/* Definition of the global. The psx_runtime.h declaration resolves to
 * this symbol. */
int openpete_no_lod = 0;
float openpete_fog_scale = 1.0f;

/* Explicit --fog-scale from the CLI: a live toggle (settings menu) passes
 * fog_scale < 0 meaning "the mode default" - but a user-chosen curve must
 * survive toggling draw distance, so remember it and let it keep winning. */
static float s_cli_fog_scale = -1.0f;

/* Fog scale at/above this is "effectively off": saturation ~100x farther
 * than any level's fog band. The menu's FOG OFF uses it;
 * openpete_fog_enabled() reads it back. */
#define FOG_OFF_SCALE 100.0f

/* Set once the user sets fog directly (the menu FOG row). From then on the
 * LOD toggle stops restoring the default fog scale - an explicit fog choice
 * survives draw-distance changes, like an explicit --fog-scale. */
static int s_fog_touched = 0;

int no_lod_apply_options(const openpete_options_t* opts) {
    openpete_no_lod = 0;
    /* Resolve the fog-distance multiplier before the no-lod early-out:
     * explicit --fog-scale wins; otherwise both LOD modes take the
     * authentic PSX curve. Extended LOD fogs its far detail on that curve,
     * as the game fogs the faces it draws. */
    int extended = opts && opts->lod == OPENPETE_LOD_EXTENDED;
    if (opts) {
        s_cli_fog_scale = opts->fog_scale;
        openpete_fog_scale = (opts->fog_scale > 0.0f) ? opts->fog_scale : 1.0f;
    }
    if (!extended) return 0;

    if (opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        OPL_WARN_F("no-lod", "%s",
                   "--no-lod is native-renderer-only (the game-side "
                   "producer stack was retired); the PsyCross reference "
                   "renders canonical PSX detail - flag ignored");
        return 0;
    }

    openpete_no_lod = 1;
    OPL_INFO_F("no-lod", "%s",
               "--no-lod: native display policy (native feeders own "
               "detail selection; producers run canonical)");
    return 0;
}

void openpete_no_lod_set(int on, float fog_scale) {
    openpete_no_lod = on ? 1 : 0;
    /* While fog is on autopilot the switch restores the explicit
     * --fog-scale, else the authentic curve (1.0) in either mode; a manual
     * FOG toggle (s_fog_touched) keeps winning. */
    if (fog_scale > 0.0f)
        openpete_fog_scale = fog_scale;
    else if (!s_fog_touched)
        openpete_fog_scale = (s_cli_fog_scale > 0.0f) ? s_cli_fog_scale : 1.0f;
    OPL_INFO_F("no-lod", "live toggle -> %s (fog x%.2f)",
               openpete_no_lod ? "EXTENDED" : "ORIGINAL", openpete_fog_scale);
}

int openpete_fog_enabled(void) {
    return openpete_fog_scale < FOG_OFF_SCALE;
}

void openpete_fog_scale_set(float scale) {
    if (isnan(scale)) return;   /* NaN passes both clamps below */
    if (scale < 0.25f) scale = 0.25f;
    if (scale > FOG_OFF_SCALE) scale = FOG_OFF_SCALE;
    /* A manual value takes fog off autopilot, same as openpete_fog_set -
     * a later LOD switch must not clobber it. Terrain re-resolves the
     * scale every walk, so this is live with no latch to poke. */
    s_fog_touched = 1;
    openpete_fog_scale = scale;
    OPL_INFO_F("no-lod", "fog scale -> x%.2f", openpete_fog_scale);
}

void openpete_fog_set(int on) {
    s_fog_touched = 1;
    /* ON restores the user's own curve when they chose one (an explicit
     * --fog-scale below the off-sentinel), else the authentic PSX curve. */
    openpete_fog_scale =
        on ? ((s_cli_fog_scale > 0.0f && s_cli_fog_scale < FOG_OFF_SCALE)
                  ? s_cli_fog_scale : 1.0f)
           : FOG_OFF_SCALE;
    OPL_INFO_F("no-lod", "fog -> %s (x%.2f)",
               on ? "ON" : "OFF", openpete_fog_scale);
}
