/* games/spyro-3/psyq_game_provides.h — the PSYQ hooks this game defines.
 *
 * Naming a hook here suppresses its inert default in
 * games/common/psyq_game_hooks_default.c, so exactly one definition of it
 * reaches the link. Adding a hook to the game without naming it here is a
 * duplicate-symbol error; naming one the game does not define is an undefined
 * reference.
 */
#ifndef OPENPETE_PSYQ_GAME_PROVIDES_H
#define OPENPETE_PSYQ_GAME_PROVIDES_H

#define PSYQ_GAME_PROVIDES_CD_READ_CB_ADDR 1
#define PSYQ_GAME_PROVIDES_LIBCD_BLOCK_ADDR 1
#define PSYQ_GAME_PROVIDES_OVERLAY_VADDR 1
#endif /* OPENPETE_PSYQ_GAME_PROVIDES_H */
