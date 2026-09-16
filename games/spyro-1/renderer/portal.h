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

/* Portal-guard gate (terrain PORTAL_GUARD experiment): true while a portal
 * fed sky within the last couple of presents. */
int openpete_portal_sky_recent(void);

/* openpete_portal_skyplane is declared in renderer/terrain_game.h (the shared
 * terrain feeder consumes it through terrain_game.portal_sky_planes). */

/* Fill out[] with the opening planes of every portal whose sky was drawn on
 * the current or immediately-previous canonical frame (one entry per portal).
 * Live-derived per call from g_Portals + the camera RAM, so canonical and
 * sub-tick presents each get the plane matching their own camera bracket.
 * Returns the count (0 when no portal sky is active). */
int openpete_portal_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max);

/* Plane-cover near margin M in world units (OPENPETE_PORTAL_PLANE_MARGIN,
 * default 256): inside an opening's footprint the preview covers everything
 * at or behind plane − M. Shared by the portal depth construction, the
 * terrain feeder's transparent-cover drop, and the cover-check instrument. */
float openpete_portal_plane_margin(void);
