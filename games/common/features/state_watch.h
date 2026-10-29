// games/common/features/state_watch.h - OPENPETE_STATE_WATCH: a stderr line
// per change of a list of guest words, keyed on the run's feed key.
//
// OPENPETE_STATE_WATCH=NAME=ADDR[:SIZE][,NAME=ADDR[:SIZE]...] names up to 16
// guest addresses (hex, 0x prefix optional; SIZE b, h or w, default w). Before
// each tick, after the game's skip, warp and trail hooks, the words are read
// and, when any differs from the last line's, one line is printed:
//   [state] vbl=<feed key> frame=<openpete_frame> NAME=<signed value> ...
// The feed key is the counter the recorder's [recq] lines and the one-shot
// frame triggers use, so a capture's log maps each line to its encoded
// frames. Reads only; speculative ticks print nothing.

#ifndef OPENPETE_STATE_WATCH_H
#define OPENPETE_STATE_WATCH_H

#include <stdint.h>

struct CPUState;

/** @brief Print the watched words when any of them changed since the last line.
 *
 * Inert unless OPENPETE_STATE_WATCH is set; a malformed entry is reported
 * once and skipped.
 * @param cpu Guest CPU state, read through its read callbacks.
 * @param frame_counter The openpete_frame counter before the tick. */
void state_watch_on_tick(struct CPUState* cpu, uint32_t frame_counter);

#endif /* OPENPETE_STATE_WATCH_H */
