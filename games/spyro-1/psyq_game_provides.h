/* games/spyro-1/psyq_game_provides.h — the PSYQ hooks this game defines.
 *
 * Naming a hook here suppresses its inert default in
 * games/common/psyq/psyq_game_hooks_default.c, so exactly one definition of it
 * reaches the link. Adding a hook to the game without naming it here is a
 * duplicate-symbol error; naming one the game does not define is an undefined
 * reference.
 */
#ifndef OPENPETE_PSYQ_GAME_PROVIDES_H
#define OPENPETE_PSYQ_GAME_PROVIDES_H

#define PSYQ_GAME_PROVIDES_CD_APPLY_BUF 1
#define PSYQ_GAME_PROVIDES_CD_READ_DONE 1
#define PSYQ_GAME_PROVIDES_CD_SETLOC 1
#define PSYQ_GAME_PROVIDES_LIBGPU_CLEAR_BLOCK_ADDR 1
#define PSYQ_GAME_PROVIDES_LIBGPU_ENV_ADDR 1
#define PSYQ_GAME_PROVIDES_MIRROR_MAIN 1
#define PSYQ_GAME_PROVIDES_MIRROR_MEMCARD_INIT 1
#define PSYQ_GAME_PROVIDES_MUSIC_ROUTE 1
#define PSYQ_GAME_PROVIDES_STATE_EXTRA 1
#define PSYQ_GAME_PROVIDES_MIRROR_PAD_INIT 1
#define PSYQ_GAME_PROVIDES_MIRROR_PAD_SLOT2 1
#define PSYQ_GAME_PROVIDES_MIRROR_RESETGRAPH 1
#define PSYQ_GAME_PROVIDES_MIRROR_VSYNC_CALLBACK 1
#define PSYQ_GAME_PROVIDES_OT_BEGIN 1
#define PSYQ_GAME_PROVIDES_OT_END 1
#define PSYQ_GAME_PROVIDES_OT_PRIM_FULL 1
#define PSYQ_GAME_PROVIDES_RAND_SEED_ADDR 1
#define PSYQ_GAME_PROVIDES_SFX_ARM 1
#define PSYQ_GAME_PROVIDES_SFX_SLOT_NAMES 1
#define PSYQ_GAME_PROVIDES_SPU_BANK_BLAST_LEN 1
#define PSYQ_GAME_PROVIDES_STACK_FRAME 1
#endif /* OPENPETE_PSYQ_GAME_PROVIDES_H */
