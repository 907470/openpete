/* portal.h : the Spyro 3 portal preview feeder's exports: the orchestrator's
 * sub-tick replay and the opening planes the shared terrain feeder clamps
 * against. */
#pragma once
#include "psx_runtime_internal.h"
#include "renderer/native_gte.h"     /* ngte_view: the caller's projection view */
#include "renderer/terrain_game.h"   /* openpete_portal_skyplane */

/* Replay the tick's captured previews on a sub-tick present; `alpha` lerps
 * the rect and pentagon edges from the previous tick when `interp_on`. */
void portal3_reextract(CPUState* cpu, double alpha, int interp_on);

/* True while a portal preview fed within the last couple of presents. */
int portal3_sky_recent(void);

/* Fill out[] with the opening plane of every portal the pass previewed on the
 * current or immediately-previous tick, one entry per portal slot. Returns the
 * count (0 when no preview is active). */
int portal3_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max);
