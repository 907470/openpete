/* games/common/renderer/credits_key.h - the interp ring key of a moby in a
 * rebuilt-slot set. A credits letter is keyed by its CreditsLetter record
 * index plus a per-index reuse generation, from the per-game fields of
 * game_layout.credits.
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
 * state byte enters the fly-in state, separates the two.
 *
 * The loading screen's treasure tally on Spyro 2 and 3 is a second
 * rebuilt-slot set (game_layout.tally): the overlay's draw rewrites the moby
 * array every tick with one gem moby per active tally record, in record
 * order, then the counter and total gems, so a gem that spawns or lands
 * moves every later gem and both HUD gems to another slot. A tally gem is
 * keyed by its record index plus an activation generation, a HUD gem by its
 * role. Spyro 1 leaves the tally block empty. */
#ifndef OPENPETE_CREDITS_KEY_H
#define OPENPETE_CREDITS_KEY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Updates every rebuilt-slot set's generations and slot tables from guest RAM as the tick left it.
 *
 * Runs once per tick capture, before any key lookup of that capture. A set
 * whose draw is inactive clears its remembered state bytes and its table and
 * keeps its generations. Reads guest RAM only. */
void game_credits_track(void);

/** @brief The interp ring key of a moby: its rebuilt-slot set key for a moby in a credits letter or tally slot, else its address.
 *
 * A letter's key is 0x60000000 | class << 16 | generation << 10 | record
 * index (class 10 bits, generation 6 bits, index 10 bits); a tally gem's
 * 0x70000000 | generation << 8 | record index, a tally HUD gem's 0x71000000
 * | role; no guest address takes them. The slot index comes from the moby's
 * address against the set's slot base; Spyro 1 maps letter slot j to record
 * j, every other set through the table game_credits_track built. Reads guest
 * RAM only.
 * @param m The moby's PSX address.
 * @return The set key, or m when no set's draw is active or m is not a keyed slot. */
uint32_t game_credits_letter_key(uint32_t m);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_CREDITS_KEY_H */
