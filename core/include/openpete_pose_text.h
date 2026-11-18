#ifndef OPENPETE_POSE_TEXT_H
#define OPENPETE_POSE_TEXT_H

/*
 * openpete_pose_text.h - the grammar of a free-camera pose
 * (--freecam-pose, OPENPETE_FREECAM_POSE), checked before a run starts.
 *
 * The runtime reader (games/common/freecam_game.c) skips every non-numeric
 * run of the text, so it reads "pos=(bogus" as zero numbers and leaves the
 * camera where it was. The check refuses such text at option parse, where
 * a typed value fails the run.
 *
 * Accepted forms, with spaces and commas between the parts optional:
 *
 *   pos=(X, Y, Z) [yaw=YAW] [pitch=PITCH] [roll=ROLL]
 *   (X, Y, Z[, YAW[, PITCH]]) [roll=ROLL]
 *   X, Y, Z[, YAW[, PITCH]] [roll=ROLL]
 *   topdown | sweepN          (the shared terrain resolver's named poses,
 *                              freecam_terrain_named_pose)
 *
 * Every number is a signed decimal integer. yaw= labels the fourth
 * positional number and pitch= the fifth, the positions the runtime
 * reader assigns them; roll= may appear once, anywhere after the position.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Check pose text against the free-camera pose grammar.
 *
 * @param text The pose text.
 * @param why Receives, on refusal, a sentence naming the offending text and
 *        its byte offset; may be NULL.
 * @param cap Capacity of @p why.
 * @return 0 when the text is a pose the grammar accepts, -1 otherwise. */
int openpete_pose_text_check(const char* text, char* why, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_POSE_TEXT_H */
