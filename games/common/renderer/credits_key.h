/* games/common/renderer/credits_key.h - the interp ring key of a credits
 * letter moby: its CreditsLetter record index plus a per-index reuse
 * generation, from the per-game fields of game_layout.credits.
 *
 * Every game's credits roll keeps its letters in an array of CreditsLetter
 * records (a state byte per record) and draws one moby per drawn record, in
 * record order, into consecutive moby slots. A slot's address is not a
 * letter's identity: Spyro 1 bump-allocates the slots from the HUD pool top
 * and draws every record, so a slot is renamed whenever the pool's earlier
 * traffic changes; Spyro 2 and 3 draw only the records in states 1-3 into
 * the moby array, so a letter leaving the drawn set moves every later letter
 * to the slot before. The update hands a new letter the first record in
 * state 0 or appends one, so a record index is stable for a letter's life
 * but is reused by a later letter; the generation, bumped when the record's
 * state byte enters the fly-in state, separates the two. */
#ifndef OPENPETE_CREDITS_KEY_H
#define OPENPETE_CREDITS_KEY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Updates the per-record generations and the slot-to-record table from the credits records as the tick left them.
 *
 * Runs once per tick capture, before any key lookup of that capture. While
 * the roll is inactive it clears the remembered state bytes and the table and
 * keeps the generations. Reads guest RAM only. */
void game_credits_track(void);

/** @brief The interp ring key of a moby: the credits letter key for a moby in a credits letter slot, else its address.
 *
 * A letter's key is 0x60000000 | class << 16 | generation << 10 | record
 * index (class 10 bits, generation 6 bits, index 10 bits), which no guest
 * address takes. The slot index comes from the moby's address against the
 * slot base; Spyro 1 maps slot j to record j, Spyro 2 and 3 through the table
 * game_credits_track built. Reads guest RAM only.
 * @param m The moby's PSX address.
 * @return The letter key, or m when the roll is inactive or m is not a letter slot. */
uint32_t game_credits_letter_key(uint32_t m);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CREDITS_KEY_H */
