/* portal_native.c cross-feeder exports.
 *
 * The portal-sky feeder and the terrain feeder cooperate on the PSX portal
 * OT semantics: the sky feeder draws the preview at per-pixel opening-plane
 * depth; the terrain feeder mirrors the per-FACE painter verdict for faces
 * that STRADDLE the opening plane (the OT-conformance clamp). This header
 * carries the plane handoff between them. */
#pragma once
#include <stdint.h>
#include "psx_runtime_internal.h"

/* Portal-guard gate (terrain PORTAL_GUARD experiment): true while a portal
 * fed sky within the last couple of presents. */
int openpete_portal_sky_recent(void);

/* One active portal-sky opening plane, in screen space: 1/view-Z of the
 * opening plane at pixel (x, y) is a·x + b·y + c (full-world view-Z), valid
 * where the result is > 0 (the plane is in front of the camera along that
 * pixel's ray). x0..y1 is the projected-outline screen bbox, inflated — the
 * conformance clamp only considers faces touching it. When an outline point
 * projects behind the camera the bbox degenerates to full-screen. */
typedef struct {
    uint32_t portal;                /* owning Portal* (identity/debug) */
    double   a, b, c;               /* screen-space inv-vz field of the plane */
    double   back_vz;               /* farthest outline view-Z (the sky's behind-
                                     * ring spill depth base) */
    double   ot_vz;                 /* 128 x the sky's mirrored .s OT bucket B
                                     * (mean outline SZ >> 7, +64 bump; validated
                                     * == live D_PORTAL_DEPTH) — the depth the .s
                                     * actually inserts the sky at. Terrain faces
                                     * whose OT mean sorts behind THIS are drawn
                                     * before the sky in the walk (sky covers
                                     * them whole-face). 0 if unavailable. */
    double   push_vz;               /* world-unit view-Z push the sky RENDERS
                                     * with (the .s's +64-slot far-bucket bump =
                                     * 8192; 0 for near portals). Sky-relative
                                     * depth constructions — the conformance
                                     * sink target — must add it so covered
                                     * faces still land just BEHIND the sky. */
    float    x0, y0, x1, y1;        /* outline screen bbox (broadphase) */
    /* The projected outline POLYGON. The conformance sink is confined to
     * faces whose screen centroid is INSIDE it — that is where the sky
     * actually rasters (fan/dome) and wins; sinking a face outside it just
     * punches a hole to whatever world geometry is behind (mc f3040: ground
     * faces with one vertex in an edge portal's inflated bbox went whole-
     * face deep and exposed the distant terrain). */
    float    px[16], py[16];
    int      pn;
    /* The outline in VIEW space, transformed straight from the portal's world
     * m_Points without going through the projection. The screen arrays above
     * exist only when EVERY outline point is in front of the camera — get
     * close to an arch at a sheer angle and one crosses behind, the projection
     * fails, and a consumer keyed on px/py loses the portal entirely just when
     * it matters most. View space is a rigid transform of world space, so
     * these are always valid and any distance or containment measured from
     * them is the world-space answer. */
    double   vx[16], vy[16], vz[16];
    int      vn;                    /* outline points in view space (0 = none) */
} openpete_portal_skyplane;

/* Fill out[] with the opening planes of every portal whose sky was drawn on
 * the current or immediately-previous canonical frame (one entry per portal).
 * Live-derived per call from g_Portals + the camera RAM, so canonical and
 * sub-tick presents each get the plane matching their own camera bracket.
 * Returns the count (0 when no portal sky is active). */
int openpete_portal_sky_planes(CPUState* cpu, openpete_portal_skyplane* out, int max);

/* Plane-cover near margin M in world units (OPENPETE_PORTAL_PLANE_MARGIN,
 * default 256): inside an opening's footprint the preview covers everything
 * at or behind plane − M. Shared by the portal depth construction, the
 * terrain feeder's transparent-cover drop, and the cover-check instrument. */
float openpete_portal_plane_margin(void);
