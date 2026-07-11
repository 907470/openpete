// spyro1_actor_pose.h — the kind-0 (player) actor pose blob for the mod
// actor render service (openpete_mod_api.h capture_actor_pose/draw_actor;
// PoC ladder rung 16 of notes/plans/openpete-modding-framework.md).
//
// Staged into <exe>/sdk/ next to openpete_mod_api.h. The blob is EXACTLY
// the field set the native player extractor reads — a recorded pose
// replays completely by construction. Treat everything except `pos` as
// opaque: mods lerp `pos` between consecutive records for sub-tick
// smoothness (evaluate at ctx->alpha in a present hook) and pass the
// rest through untouched. `seg` carries the game's per-segment animation
// stream state (keyframe cursors into the loaded Spyro model), which is
// why `model` guards replay: after a level change the cursors dangle and
// draw_actor skips the pose safely.
//
// Capture is only valid while the game is rendering Spyro (returns -1
// before the first render / while a cutscene suppresses the player).

#ifndef SPYRO1_ACTOR_POSE_H
#define SPYRO1_ACTOR_POSE_H

#include <stdint.h>

#define SPYRO1_ACTOR_POSE_MAGIC   0x4F503153u /* "S1PO" */
#define SPYRO1_ACTOR_POSE_VERSION 1u
#define SPYRO1_ACTOR_KIND_PLAYER  0

typedef struct spyro1_actor_pose {
    uint32_t magic;    /* SPYRO1_ACTOR_POSE_MAGIC   */
    uint32_t version;  /* SPYRO1_ACTOR_POSE_VERSION */
    float    pos[3];   /* world position — the one field mods interpolate */
    uint32_t rot[3];   /* packed euler words: body / head / tail (opaque) */
    uint32_t anim_idx; /* body animation index (opaque)                   */
    uint32_t model;    /* g_Models[0] at capture — replay validity guard  */
    uint8_t  seg[204]; /* per-segment animation stream state (opaque)     */
} spyro1_actor_pose_t;

#endif /* SPYRO1_ACTOR_POSE_H */
