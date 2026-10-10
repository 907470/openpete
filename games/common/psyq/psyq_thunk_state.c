/* games/common/psyq/psyq_thunk_state.c - the savestate game-extra payload for a
 * game whose only extra state is the shared thunk layer's latch pack.
 *
 * Such a game's runtime descriptor names psyq_thunk_state_size and
 * psyq_thunk_state_save for its state_extra_size and state_extra_save and
 * psyq_thunk_state_extra_load for its state_extra_load. A game with more to
 * carry defines its own three hooks and embeds the pack in its layout
 * through psyq_thunk_state_save/_load.
 */
#include "psyq/psyq_thunk_state.h"

void psyq_thunk_state_extra_load(const void* src, uint32_t size) {
    (void)psyq_thunk_state_load(src, size);
}
