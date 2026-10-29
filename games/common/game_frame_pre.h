// games/common/game_frame_pre.h - the shared per-tick pre-hook and the
// per-game data it runs.
//
// game_frame_pre_on_tick is a game's on_frame_pre (openpete_game_runtime.h)
// when its pre-tick sequence is: the game's lead hooks, its level skip, its
// level warp, its trail hooks, the scripted player pose and free-camera
// engage, the write census phase, then the host text layers. A game listing
// games/common/game_frame_pre.c points its game_host_desc_t's frame_pre at
// its steps; a game whose sequence differs keeps its own on_frame_pre and
// does not list the file.

#ifndef OPENPETE_GAME_FRAME_PRE_H
#define OPENPETE_GAME_FRAME_PRE_H

#include <stdint.h>

struct CPUState;

/** The game-specific steps of game_frame_pre_on_tick, run in field order; a
 *  NULL step is skipped. */
typedef struct game_frame_pre {
    /** Hooks run before the skip, whose state the skip reads this tick. */
    void (*lead)(struct CPUState* cpu, uint32_t frame_counter);
    /** The --skip-to-level driver. */
    void (*skip_on_tick)(struct CPUState* cpu, uint32_t frame_counter);
    /** The dev-overlay level warp's per-tick drain. */
    void (*levelwarp_on_tick)(struct CPUState* cpu);
    /** Hooks run after the warp, which read this tick's skip and warp stores. */
    void (*trail)(struct CPUState* cpu, uint32_t frame_counter);
    /** Nonzero holds the scripted free-camera engage for this tick. */
    int (*freecam_engage_held)(void);
} game_frame_pre_t;

/** @brief Run the registered game's pre-tick sequence over its host descriptor's frame_pre steps before the game fiber runs a tick.
 *
 * Order: lead, skip_on_tick, levelwarp_on_tick, trail, the
 * OPENPETE_STATE_WATCH line (state_watch_on_tick), the scripted player
 * pose, the free-camera engage unless freecam_engage_held reports nonzero,
 * the write census phase entry, then text_moby_native_layers_tick. A NULL
 * frame_pre runs the sequence with every game step skipped.
 * @param cpu Guest CPU state.
 * @param frame_counter The openpete_frame counter before the tick. */
void game_frame_pre_on_tick(struct CPUState* cpu, uint32_t frame_counter);

#endif /* OPENPETE_GAME_FRAME_PRE_H */
