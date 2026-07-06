// core/features/no_lod.h — `--no-lod` feature module.
//
// See notes/plans/openpete-disable-lods.md for the design + history;
// notes/plans/openpete-feature-modules.md for the consolidation
// pattern (widescreen is the sibling reference).
//
// Two halves:
//   - core/features/no_lod.c:     owns the openpete_no_lod flag and
//                                 the apply_options entry that runs
//                                 per-game setup when --no-lod is set.
//   - games/spyro-1/features/no_lod.c: owns all Spyro-specific knob
//                                 storage (g_SonyImage redirect,
//                                 grown env-queue offsets, moby
//                                 LP-face offset, DBG_KILL_LP), the
//                                 NO_LOD_REGION_VADDR layout, the
//                                 region allocation, and the
//                                 polybuf-grow call. Registered as
//                                 `no_lod_module` via feature_list.c.
//
// `openpete_no_lod` itself is also re-declared in
// third_party/psxrecomp/runner/include/psx_runtime.h so recompiler-
// generated C can read it without dragging in this header.

#ifndef OPENPETE_FEATURES_NO_LOD_H
#define OPENPETE_FEATURES_NO_LOD_H

#include "openpete.h"   /* openpete_options_t */

#ifdef __cplusplus
extern "C" {
#endif

/* --no-lod FEATURE flag: full detail at any distance. Set once at boot
 * from --no-lod, in every renderer mode. The native feeders read it as
 * a DISPLAY policy (moby zsf4/tier neutralization + far-record builder
 * in moby_native.c, terrain HQ classifier, portal always-dome, fog
 * default). See notes/plans/openpete-disable-lods.md. */
extern int openpete_no_lod;

/* Game-side PRODUCER arming (PsyCross renderer only): the relocated +
 * grown env/moby queues, the m_LodDistance / m_Distance / zsf4 / tier
 * saturation hooks, the moby LP-face knob, and the polybuf grow — the
 * machinery that makes the ORIGINAL .s renderer draw full detail. On
 * the native path the .s producers run pure canonical 4:3 (Phase 6
 * stack-revert; the native feeders own no-LOD), so this stays 0 there
 * and every no_lod_* substitution hook is identity. */
extern int openpete_no_lod_producers;

/* --fog-scale: terrain fog-distance multiplier, resolved at boot in
 * no_lod_apply_options (explicit --fog-scale wins; else 4.0 under
 * --no-lod, 1.0 otherwise). 1.0 = the authentic PSX curve; N scales
 * every channel of the fog model together (DPCS vertex curve, CLUT-row
 * palette ladder, near-tile select, fog-saturation LP handoff — see
 * games/spyro-1/features/terrain_native.c). Always > 0 after boot. */
extern float openpete_fog_scale;

/* Boot-time entry. If opts->no_lod is set, allocate + register the
 * relocated env-renderer scratch pool (via the per-game module's
 * setup hook), swing the recompiler's value-keyed
 * g_SonyImage / queue-offset / moby-LP knobs, and force-arm the
 * widescreen polybuf-grow path. Returns 0 on success, non-zero on
 * setup error. Called from core/runtime/run.c AFTER
 * widescreen_apply_disp_w_factor and BEFORE the first dispatch into
 * recompiled code. */
int no_lod_apply_options(const openpete_options_t* opts);

/* Per-game setup hook installed by the per-game no_lod module
 * (games/<game>/features/no_lod.c) before the apply_options entry
 * runs. The setup callback returns 0 on success. NULL until the
 * module's feature_list registration kicks in. */
typedef int (*no_lod_setup_fn)(void);
void no_lod_set_setup_hook(no_lod_setup_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_FEATURES_NO_LOD_H */
