/* render_paths.h — per-path render isolation registry (--render-paths debug).
 *
 * Lets a debugging session restrict rendering to a chosen set of render paths
 * (cyclorama, Spyro, mobys, ...) in BOTH renderers, so a single path can be
 * compared native-vs-PsyCross in isolation (no occlusion noise from paths that
 * native doesn't draw yet). The native side gates its feed on the selection;
 * the PsyCross side drops the other paths' geometry prims during the OT walk
 * (psyq_DrawOTag). It is presentation-only — it never writes game RAM (the OT
 * filter just doesn't *submit* a prim), so gameplay state is unaffected.
 *
 * Mechanism: each render path brackets the polybuf range it emits each frame
 * (POLYBUF_CURSOR before/after its dispatch) and records it here; the OT walk
 * then keeps a geometry prim only if its PSX address falls inside a SELECTED
 * path's recorded interval. Control/env prims (draw-env, loads, anchors) always
 * pass so the framebuffer is still set up. */

#ifndef OPENPETE_SPYRO_RENDER_PATHS_H
#define OPENPETE_SPYRO_RENDER_PATHS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RP_CYCLORAMA = 0,
    RP_SPYRO,
    RP_MOBY,      /* regular + specular + shaded mobys; also HUD mobys (CopyHudMobysToShaded) */
    RP_TERRAIN,   /* world/environment geometry (func_8002B9CC -> func_800258F0) */
    RP_SHADOW,    /* moby shadows (func_80059F8C) + Spyro silhouette shadow (func_80059A48) */
    RP_FLAME,     /* Spyro's flame breath (func_80058D64) */
    RP_PARTICLES, /* particle systems (func_800573C8) */
    RP_COUNT
} render_path_t;

/* Note: there is no separate HUD path — HUD content is copied into the shaded
 * moby list (CopyHudMobysToShaded) and rendered through the moby pipeline, so it
 * is part of RP_MOBY, not separately isolatable via a dispatch bracket. */

/* Parse the --render-paths CSV. NULL/empty = no selection (all paths render,
 * filtering off). Unknown names are warned and ignored. */
void render_paths_init(const char* spec);

/* Is a selection in effect? false = render everything (the default; callers can
 * fast-path out). */
bool render_paths_active(void);

/* Is this path selected? Always true when no selection is in effect. */
bool render_path_enabled(render_path_t p);

/* Per-frame interval table. frame_reset clears it (call once per frame after the
 * OT walk consumes it); record appends one path's emitted polybuf range. */
void render_paths_frame_reset(void);
void render_path_record(render_path_t p, uint32_t start, uint32_t end);

/* OT-walk filter: should a geometry prim at this PSX polybuf address be drawn?
 * True iff `addr` is inside a SELECTED path's recorded interval. Untracked
 * geometry (no interval) returns false (dropped) when a selection is active.
 * Only meaningful while render_paths_active(); callers gate on that. */
bool render_path_addr_visible(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SPYRO_RENDER_PATHS_H */
