// games/common/psyq/psyq_game_hooks_default.c - the inert default for every
// per-game PSYQ hook.
//
// A game supplies only the hooks it has and names them in its own
// psyq_game_provides.h; this file defines the rest. psyq_game_overlay_vaddr,
// psyq_game_libcd_block_addr and psyq_game_cd_read_cb_addr have no default
// here: the generated game_identity.c defines them for every game. The defaults are inert: no
// PSX RAM is written, no filter rejects anything, no table is registered. A
// game whose PSYQ-internal mirrors have not been constructed therefore writes
// nothing rather than writing another game's addresses, and the resulting gap
// shows up in a whole-RAM diff as missing bytes instead of as corruption.
//
// Every hook ends up with exactly one definition in the link, which is what
// makes this portable. Weak definitions would be the obvious mechanism and do
// not work: on PE/COFF a weak definition is emitted as a renamed symbol plus
// an undefined weak external, and the linker leaves a reference to it
// unresolved unless some object defines the name strongly. The same code links
// on ELF, so the gap appears only when cross-building for Windows.
//
// A game that defines a hook without naming it here is a duplicate-symbol
// error, and one that names a hook it does not define is an undefined
// reference. Both are loud, at link time, on every platform.

#include "psyq/psyq_game_hooks.h"

// Per-game list of PSYQ_GAME_PROVIDES_* macros, from the game's own directory.
#include "psyq_game_provides.h"

#ifndef PSYQ_GAME_PROVIDES_STACK_FRAME
int psyq_game_stack_frame(psyq_frame_site_t site,
                                         psyq_game_frame_t* out) {
    (void)site; (void)out;
    return 0;
}
#endif

#ifndef PSYQ_GAME_PROVIDES_MIRROR_MAIN
void psyq_game_mirror_main(CPUState* cpu) { (void)cpu; }
#endif
#ifndef PSYQ_GAME_PROVIDES_MIRROR_RESETGRAPH
void psyq_game_mirror_resetgraph(CPUState* cpu, int mode) {
    (void)cpu; (void)mode;
}
#endif
#ifndef PSYQ_GAME_PROVIDES_MIRROR_VSYNC_CALLBACK
void psyq_game_mirror_vsync_callback(CPUState* cpu, uint32_t cb) {
    (void)cpu; (void)cb;
}
#endif
#ifndef PSYQ_GAME_PROVIDES_MIRROR_PAD_INIT
void psyq_game_mirror_pad_init(CPUState* cpu, uint32_t buf0,
                                              uint32_t buf1) {
    (void)cpu; (void)buf0; (void)buf1;
}
#endif
#ifndef PSYQ_GAME_PROVIDES_MIRROR_PAD_SLOT2
void psyq_game_mirror_pad_slot2(CPUState* cpu) { (void)cpu; }
#endif
#ifndef PSYQ_GAME_PROVIDES_MIRROR_MEMCARD_INIT
void psyq_game_mirror_memcard_init(CPUState* cpu) { (void)cpu; }
#endif

#ifndef PSYQ_GAME_PROVIDES_RAND_SEED_ADDR
uint32_t psyq_game_rand_seed_addr(void) { return 0u; }
#endif

#ifndef PSYQ_GAME_PROVIDES_LIBGPU_ENV_ADDR
uint32_t psyq_game_libgpu_env_addr(void) { return 0u; }
#endif
#ifndef PSYQ_GAME_PROVIDES_LIBGPU_CLEAR_BLOCK_ADDR
uint32_t psyq_game_libgpu_clear_block_addr(void) { return 0u; }
#endif

#ifndef PSYQ_GAME_PROVIDES_CD_SETLOC
void psyq_game_cd_setloc(const void* cdl_loc) { (void)cdl_loc; }
#endif
#ifndef PSYQ_GAME_PROVIDES_CD_APPLY_BUF
void psyq_game_cd_apply_buf(unsigned char* buf, int sectors) {
    (void)buf; (void)sectors;
}
#endif
#ifndef PSYQ_GAME_PROVIDES_CD_READ_DONE
void psyq_game_cd_read_done(CPUState* cpu, uint32_t read_cb) {
    (void)cpu; (void)read_cb;
}
#endif
#ifndef PSYQ_GAME_PROVIDES_CD_READ_IN_LOAD
int      psyq_game_cd_read_in_load(CPUState* cpu) { (void)cpu; return 0; }
uint32_t psyq_game_cd_read_delay_cap(void)        { return 0u; }
#endif
#ifndef PSYQ_GAME_PROVIDES_CD_STREAM
int psyq_game_cd_read_streamed(CPUState* cpu) { (void)cpu; return 0; }
int psyq_game_cd_spin_wait(CPUState* cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
#endif
#ifndef PSYQ_GAME_PROVIDES_ROOT_COUNTER2
uint16_t psyq_game_root_counter2(void) { return 0u; }
#endif
#ifndef PSYQ_GAME_PROVIDES_CD_RAW_PC_MIN
uint32_t psyq_game_cd_raw_pc_min(void) { return 0xFFFFFFFFu; }
uint32_t psyq_game_cd_done_witness_ra(void) { return 0u; }
#endif
#ifndef PSYQ_GAME_PROVIDES_LOAD_WINDOW
uint32_t psyq_game_load_window_word(void)              { return 0u; }
int      psyq_game_load_window_active(uint32_t value)  { (void)value; return 0; }
#endif

#ifndef PSYQ_GAME_PROVIDES_MUSIC_ROUTE
void psyq_game_music_route(int sector, psyq_music_route_t* out) {
    (void)sector;
    out->track = -1;            /* no override by track */
    out->custom_path = NULL;    /* no custom song: plain XA at the sector */
}
#endif

/* psyq_game_render_paths_active and psyq_game_render_path_addr_visible have no
 * default here: renderer/render_paths.c defines them for every game. */

#ifndef PSYQ_GAME_PROVIDES_SFX_SLOTS
const psyq_sfx_slots_t* psyq_game_sfx_slots(void) { return NULL; }
#endif
