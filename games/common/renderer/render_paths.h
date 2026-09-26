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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RP_CYCLORAMA = 0,
    RP_SPYRO,
    RP_MOBY,      /* regular + specular + shaded mobys; also HUD mobys (CopyHudMobysToShaded) */
    RP_TERRAIN,   /* world/environment geometry (func_8002B9CC -> func_800258F0) */
    RP_SHADOW,    /* the player's drop-shadow: Spyro 1 func_80059A48, the Spyro 2/3 player
                   * shadow routine */
    RP_FLAME,     /* Spyro's flame breath (func_80058D64) */
    RP_PARTICLES, /* particle systems (func_800573C8) */
    RP_TRACER,    /* additive tracer/beam (func_800189F0) — charge streak, fireball trail */
    RP_TRANSITION,/* level-exit / balloonist cross-fade backdrop (func_8004F000) */
    RP_PORTAL,    /* portal skybox previews (func_80050BE0 RenderPortals) */
    RP_GLOW,      /* additive glow rings (func_800580F4); a separate .s renderer the game
                   * skips in some states (dragon-rescue cutscenes) while the pool stays active */
    RP_SPARKLE,   /* sparkle twinkles (func_800584C4), same independence as RP_GLOW */
    RP_MOBY_SHADOW,/* moby drop-shadows (Spyro 1 func_80059F8C). Separate from RP_SHADOW:
                   * the rescue-cutscene states 5 and 6 draw the player's shadow and not
                   * the moby shadows while the moby draw still fills their list */
    RP_COUNT
} render_path_t;

/* Note: there is no separate HUD path — HUD content is copied into the shaded
 * moby list (CopyHudMobysToShaded) and rendered through the moby pipeline, so it
 * is part of RP_MOBY, not separately isolatable via a dispatch bracket. */

/** Parse the --render-paths CSV. NULL/empty = no selection (all paths render,
 * filtering off). Unknown names are warned and ignored.
 * @param spec comma- or space-separated path names */
void render_paths_init(const char* spec);

/* Live re-selection (dev overlay): re-parse the CSV and re-sync the native
 * renderer's isolation background. Safe mid-session — the selection only
 * gates prim submission, never game RAM. */
void render_paths_set(const char* spec);

/* Current selection as a CSV ("(all)" when no selection is active,
 * "(none)" when a selection matched nothing). */
void render_paths_current(char* buf, size_t cap);

/* Is a selection in effect? false = render everything (the default; callers can
 * fast-path out). */
bool render_paths_active(void);

/* Is this path selected? Always true when no selection is in effect. */
bool render_path_enabled(render_path_t p);

/* Per-frame interval table. frame_reset clears it (call once per frame after the
 * OT walk consumes it); record appends one path's emitted polybuf range. */
void render_paths_frame_reset(void);
void render_path_record(render_path_t p, uint32_t start, uint32_t end);

/* Mark a path canon-drawn for this frame WITHOUT a polybuf interval. For
 * producers whose native feed isn't 1:1 with their polybuf output — e.g.
 * RenderPortals under widescreen can feed native-revealed portals the .s
 * culled (empty polybuf interval, real native content). Interval-less, so it
 * feeds only the canon-drawn mask, never the OT-walk addr filter. Cleared by
 * render_paths_frame_reset like the intervals. */
void render_path_mark_drawn(render_path_t p);

/* "Canon drawn" mask — which paths emitted geometry in the MOST RECENT canonical
 * render. render_paths_canon_snapshot() latches the current per-frame interval
 * set into this persistent mask (call once per canonical frame, right before
 * render_paths_frame_reset() clears the intervals). render_path_canon_drawn()
 * reads it. The high-FPS sub-tick spine gates each reextract feeder on this so an
 * intermediate present reproduces exactly the path set the canonical frame drew —
 * e.g. during a level transition the game stops rendering terrain, so the spine
 * must not replay stale level geometry over the cyclorama. Independent of the
 * --render-paths selection (the intervals are recorded whenever the native feed
 * is active). */
void render_paths_canon_snapshot(void);
bool render_path_canon_drawn(render_path_t p);

/* OT-walk filter: should a geometry prim at this PSX polybuf address be drawn?
 * True iff `addr` is inside a SELECTED path's recorded interval. Untracked
 * geometry (no interval) returns false (dropped) when a selection is active.
 * Only meaningful while render_paths_active(); callers gate on that. */
bool render_path_addr_visible(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SPYRO_RENDER_PATHS_H */
