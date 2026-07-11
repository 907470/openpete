// s3_actor_native.h — the Spyro 3 player model as a host-side actor
// source (see s3_actor_native.c).

#ifndef S3_ACTOR_NATIVE_H
#define S3_ACTOR_NATIVE_H

#include <stdint.h>

struct CPUState;

/* Emit one pose of the S3 player model into the current native scene.
 * pose = spyro1_s3_actor_pose_t (kind 1 of the actor render service).
 * Returns triangles emitted; 0 = skipped safely (no blob / bad pose /
 * scene not live). */
int spyro1_s3_actor_emit(struct CPUState* cpu, const void* pose, int len,
                         uint32_t tint_rgba);

/* OPENPETE_S3_ACTOR_TEST=1 harness: hover cycle beside the player,
 * called from the canonical Spyro hook + the sub-tick re-extract. */
void spyro1_s3_actor_test_emit(struct CPUState* cpu);

#endif /* S3_ACTOR_NATIVE_H */
