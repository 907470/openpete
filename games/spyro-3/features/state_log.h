#pragma once
/* state_log.h - OPENPETE_S3_STATE_LOG: a read-only log of the game's state
 * words, one line per change, for harnesses that classify where a run spent
 * its VBLs (gameplay, speech, speedway menu, loading).
 *
 * Called from game_extras_impl.c (game_on_frame_pre). */

#include <stdint.h>

typedef struct CPUState CPUState;

/** @brief Logs the game's state words on every tick they differ from the previous logged tick, when OPENPETE_S3_STATE_LOG is set.
 *
 * The words are game.state (0x8006E344), loadStage (0x8006C518),
 * currentLevel (0x8006C5BC) and the area word (0x8006C5C8). Each change
 * prints one stderr line `[s3-state] frame=<openpete_frame> state=<n>
 * load=<n> level=<n> area=<n> live=<0|1>`, where live is the playable
 * predicate spyro3_skip_gameplay_live tests (game.state 0 with the level
 * overlay resident). Speculative ticks are skipped. The function only reads
 * guest RAM.
 * @param cpu The guest CPU state.
 * @param frame_counter openpete_frame, printed on each line. */
void spyro3_state_log_on_tick(CPUState* cpu, uint32_t frame_counter);
