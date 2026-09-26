/* portal.c cross-feeder exports.
 *
 * The portal-sky feeder and the terrain feeder cooperate on the PSX portal
 * OT semantics: the sky feeder draws the preview at per-pixel opening-plane
 * depth; the terrain feeder mirrors the per-FACE painter verdict for faces
 * that STRADDLE the opening plane (the OT-conformance clamp). This header
 * carries the plane handoff between them. */
#pragma once
#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/native_gte.h"     /* ngte_view: the caller's projection view */
#include "renderer/terrain_game.h"   /* openpete_portal_skyplane */
#include "renderer/portal_plane.h"    /* openpete_portal_plane_margin */

/** @brief Portal-guard gate (terrain PORTAL_GUARD experiment): whether a portal fed
 * sky during the canonical render in flight or the last one presented, keyed
 * on the canonical-render epoch so a sub-tick present counts the capture of
 * the scene it re-extracts, runahead speculation included.
 * @return 1 when a portal sky feed is recent, 0 otherwise */
int openpete_portal_sky_recent(void);

/** @brief 1 when the world draw now running is func_8002B9CC's and this tick's
 * RenderPortals has not run yet: the draw runs RenderPortals next, and the
 * terrain feed waits for its capture (terrain_game.env_feed_waits_for_portals).
 * @param cpu guest state at the world-draw hook
 * @return 1 when the terrain feed must wait for this tick's portal capture */
int openpete_portal_capture_follows(CPUState* cpu);

/* openpete_portal_skyplane is declared in renderer/terrain_game.h (the shared
 * terrain feeder consumes it through terrain_game.portal_sky_planes). */

/** @brief Fill out[] with the opening planes of every portal captured by the
 * canonical render in flight or the last one presented (one entry per portal;
 * freshness is the canonical-render epoch, not openpete_frame, which a
 * runahead speculation's capture leads). Live-derived per call from g_Portals
 * and the camera RAM, so canonical and sub-tick presents each get the plane
 * matching their own camera bracket.
 * @param cpu guest state
 * @param v   the caller's pass view (camera, offset, H)
 * @param out receives the plane entries
 * @param max capacity of out
 * @return the entry count (0 when no portal sky is active) */
int openpete_portal_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max);


