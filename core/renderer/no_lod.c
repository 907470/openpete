// core/renderer/no_lod.c - see no_lod.h.
//
// --no-lod is a native-renderer display policy with no game-side producer:
// the native feeders derive full detail from gamestate (moby far-record
// builder + zsf4/tier neutralization, terrain HQ classifier, portal
// always-dome). In PsyCross reference mode the flag warns and
// is ignored - the reference renders canonical PSX detail.

#include "no_lod.h"

#include "openpete_live_latch.h"
#include "openpete_log.h"

#include <math.h>
#include <stddef.h>

/* Definition of the global. The psx_runtime.h declaration resolves to
 * this symbol. openpete_no_lod, openpete_fog_scale and s_fog_pin are the
 * latched values the walks read; the setters store the wanted values below,
 * which openpete_no_lod_latch copies on the sim thread before a guest resume
 * and before a re-extract (openpete_live_latch.h). The wanted values start at
 * the latched initializers and no_lod_apply_options stores the boot values
 * into both. */
int openpete_no_lod = 0;
float openpete_fog_scale = 1.0f;
static _Atomic(int) s_no_lod_want    = 0;
static _Atomic(int) s_fog_scale_want = OP_LIVE_F_ONE;   /* float bits */
static _Atomic(int) s_fog_pin_want   = OPENPETE_FOG_AUTO;

/* Explicit --fog-scale from the CLI: a live toggle (settings menu) passes
 * fog_scale < 0 meaning "the mode default" - but a user-chosen curve must
 * survive toggling draw distance, so remember it and let it keep winning. */
static float s_cli_fog_scale = -1.0f;

/* Set once the fog scale is set directly (openpete_fog_scale_set). From
 * then on the LOD toggle stops restoring the default fog scale, like an
 * explicit --fog-scale. Atomic: the dev overlay and the settings menu set
 * it from different threads. */
static _Atomic(int) s_fog_touched = 0;

/* The pinned fog form (openpete_fog_mode_t), OPENPETE_FOG_AUTO while it
 * follows the LOD mode. */
static int s_fog_pin = OPENPETE_FOG_AUTO;

/** @brief Resolve the boot LOD mode, fog multiplier and fog pin into the latched values; no_lod_apply_options documents the rules.
 *
 * @param opts Parsed options; NULL leaves the fog scale and clears the flag. */
static void no_lod_resolve(const openpete_options_t* opts) {
    openpete_no_lod = 0;
    /* Resolve the fog-distance multiplier before the no-lod early-out:
     * explicit --fog-scale wins; otherwise both LOD modes take the
     * authentic PSX curve. Extended LOD fogs its far detail on that curve,
     * as the game fogs the faces it draws. */
    int extended = opts && opts->lod == OPENPETE_LOD_EXTENDED;
    s_fog_pin = OPENPETE_FOG_AUTO;
    if (opts) {
        s_fog_pin = opts->fog_mode;
        s_cli_fog_scale = opts->fog_scale;
        openpete_fog_scale = (opts->fog_scale > 0.0f) ? opts->fog_scale : 1.0f;
    }
    if (!extended) return;

    if (opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        OPL_WARN_F("no-lod", "%s",
                   "--no-lod is native-renderer-only (the game-side "
                   "producer stack was retired); the PsyCross reference "
                   "renders canonical PSX detail - flag ignored");
        return;
    }

    openpete_no_lod = 1;
    OPL_INFO_F("no-lod", "%s",
               "--no-lod: native display policy (native feeders own "
               "detail selection; producers run canonical)");
}

int no_lod_apply_options(const openpete_options_t* opts) {
    no_lod_resolve(opts);
    /* The wanted values take the boot values, so the getters read only them
     * and the next latch keeps what boot resolved. */
    op_live_set(&s_no_lod_want, openpete_no_lod);
    op_live_set_f(&s_fog_scale_want, openpete_fog_scale);
    op_live_set(&s_fog_pin_want, s_fog_pin);
    return 0;
}

void openpete_no_lod_set(int on, float fog_scale) {
    op_live_set(&s_no_lod_want, on ? 1 : 0);
    /* While the scale is on autopilot the switch restores the explicit
     * --fog-scale, else the authentic curve (1.0) in either mode; a manual
     * scale (s_fog_touched) keeps winning. */
    if (fog_scale > 0.0f)
        op_live_set_f(&s_fog_scale_want, fog_scale);
    else if (!atomic_load(&s_fog_touched))
        op_live_set_f(&s_fog_scale_want, (s_cli_fog_scale > 0.0f) ? s_cli_fog_scale : 1.0f);
    OPL_INFO_F("no-lod", "live toggle -> %s (fog x%.2f)",
               on ? "EXTENDED" : "ORIGINAL", openpete_fog_scale_wanted());
}

int openpete_no_lod_wanted(void) {
    return op_live_get(&s_no_lod_want);
}

float openpete_fog_scale_wanted(void) {
    return op_live_get_f(&s_fog_scale_want);
}

void openpete_no_lod_latch(void) {
    (void)op_live_latch(&s_no_lod_want, &openpete_no_lod);
    (void)op_live_latch_f(&s_fog_scale_want, &openpete_fog_scale);
    (void)op_live_latch(&s_fog_pin_want, &s_fog_pin);
}

int openpete_fog_mode(void) {
    if (s_fog_pin != OPENPETE_FOG_AUTO) return s_fog_pin;
    return openpete_no_lod ? OPENPETE_FOG_PER_PIXEL : OPENPETE_FOG_ORIGINAL;
}

int openpete_fog_mode_pinned(void) {
    return op_live_get(&s_fog_pin_want);
}

void openpete_fog_mode_set(int mode) {
    if (mode < OPENPETE_FOG_AUTO || mode > OPENPETE_FOG_OFF) return;
    op_live_set(&s_fog_pin_want, mode);
    static const char *const names[] = { "original", "per-pixel", "off" };
    const int shown = mode != OPENPETE_FOG_AUTO ? mode
                    : (openpete_no_lod_wanted() ? OPENPETE_FOG_PER_PIXEL : OPENPETE_FOG_ORIGINAL);
    OPL_INFO_F("no-lod", "fog -> %s%s", names[shown],
               mode == OPENPETE_FOG_AUTO ? " (follows LOD)" : "");
}

void openpete_fog_scale_set(float scale) {
    if (isnan(scale)) return;   /* NaN passes both clamps below */
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 100.0f) scale = 100.0f;
    /* A manual value takes the scale off autopilot: a later LOD switch
     * must not clobber it. Terrain re-resolves the latched scale every
     * walk, so the value applies from the walk after the next latch. */
    atomic_store(&s_fog_touched, 1);
    op_live_set_f(&s_fog_scale_want, scale);
    OPL_INFO_F("no-lod", "fog scale -> x%.2f", scale);
}
