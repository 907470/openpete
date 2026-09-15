/* games/common/psyq_thunk_state.c — the savestate game-extra payload for a
 * game whose only extra state is the shared thunk layer's latch pack.
 *
 * A game with more to carry defines the three functions itself, names
 * PSYQ_GAME_PROVIDES_STATE_EXTRA in its psyq_game_provides.h, and embeds the
 * pack in its own layout through psyq_thunk_state_save/_load. The guard, not
 * a weak symbol, keeps exactly one definition in the link on every platform.
 */
#include "game_extras.h"
#include "psyq_game_provides.h"
#include "psyq_thunk_state.h"

#ifndef PSYQ_GAME_PROVIDES_STATE_EXTRA
uint32_t game_state_extra_size(void) { return psyq_thunk_state_size(); }
void     game_state_extra_save(void* dst) { psyq_thunk_state_save(dst); }
void     game_state_extra_load(const void* src, uint32_t size) {
    (void)psyq_thunk_state_load(src, size);
}
#endif
