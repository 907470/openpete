// games/common/psyq_game_hooks_default.c — weak defaults for every per-game
// PSYQ hook.
//
// A game supplies only the hooks it has. The defaults are inert: no PSX RAM is
// written, no filter rejects anything, no table is registered. A game whose
// PSYQ-internal mirrors have not been constructed therefore writes nothing
// rather than writing another game's addresses, and the resulting gap shows up
// in a whole-RAM diff as missing bytes instead of as corruption.

#include "psyq_game_hooks.h"

#if defined(__GNUC__) || defined(__clang__)
#define PSYQ_HOOK_WEAK __attribute__((weak))
#else
#define PSYQ_HOOK_WEAK
#endif

PSYQ_HOOK_WEAK int psyq_game_stack_frame(psyq_frame_site_t site,
                                         psyq_game_frame_t* out) {
    (void)site; (void)out;
    return 0;
}

PSYQ_HOOK_WEAK void psyq_game_mirror_main(CPUState* cpu) { (void)cpu; }
PSYQ_HOOK_WEAK void psyq_game_mirror_resetgraph(CPUState* cpu, int mode) {
    (void)cpu; (void)mode;
}
PSYQ_HOOK_WEAK void psyq_game_mirror_vsync_callback(CPUState* cpu, uint32_t cb) {
    (void)cpu; (void)cb;
}
PSYQ_HOOK_WEAK void psyq_game_mirror_pad_init(CPUState* cpu, uint32_t buf0,
                                              uint32_t buf1) {
    (void)cpu; (void)buf0; (void)buf1;
}
PSYQ_HOOK_WEAK void psyq_game_mirror_pad_slot2(CPUState* cpu) { (void)cpu; }
PSYQ_HOOK_WEAK void psyq_game_mirror_memcard_init(CPUState* cpu) { (void)cpu; }

PSYQ_HOOK_WEAK unsigned psyq_game_vsync_poll_advance(void) { return 0u; }

PSYQ_HOOK_WEAK uint32_t psyq_game_rand_seed_addr(void) { return 0u; }

PSYQ_HOOK_WEAK uint32_t psyq_game_overlay_vaddr(void) { return 0u; }

PSYQ_HOOK_WEAK void psyq_game_cd_setloc(const void* cdl_loc) { (void)cdl_loc; }
PSYQ_HOOK_WEAK void psyq_game_cd_apply_buf(unsigned char* buf, int sectors) {
    (void)buf; (void)sectors;
}
PSYQ_HOOK_WEAK void psyq_game_cd_read_done(CPUState* cpu, uint32_t read_cb) {
    (void)cpu; (void)read_cb;
}

PSYQ_HOOK_WEAK int  psyq_game_render_paths_active(void) { return 0; }
PSYQ_HOOK_WEAK int  psyq_game_render_path_addr_visible(uint32_t addr) {
    (void)addr;
    return 1;   /* no filter declared: everything is visible */
}
PSYQ_HOOK_WEAK void psyq_game_ot_begin(void) {}
PSYQ_HOOK_WEAK void psyq_game_ot_prim_full(unsigned code_byte,
                                           const void* payload,
                                           unsigned payload_bytes) {
    (void)code_byte; (void)payload; (void)payload_bytes;
}
PSYQ_HOOK_WEAK void psyq_game_ot_end(void) {}

PSYQ_HOOK_WEAK void psyq_game_sfx_arm(void) {}
PSYQ_HOOK_WEAK unsigned psyq_game_spu_bank_blast_len(void) { return 0u; }
PSYQ_HOOK_WEAK int  psyq_game_sfx_slot_names(unsigned spu_addr,
                                             const char** out, int max) {
    (void)spu_addr; (void)out; (void)max;
    return 0;
}
