// games/common/psyq/psyq_game_hooks.c - the per-game PSYQ hooks, forwarded to
// the running game's descriptor.
//
// Every psyq_game_* entry the shared thunk layer calls reads one field of the
// psyq_game_hooks_t the registered runtime descriptor carries
// (openpete_game_runtime_t.psyq). A field the game leaves 0 or NULL takes
// the inert default: no PSX RAM is written, no filter rejects anything, no
// table is registered. A game whose PSYQ-internal mirrors have not been
// constructed therefore writes nothing rather than writing another game's
// addresses, and the resulting gap shows up in a whole-RAM diff as missing
// bytes instead of as corruption. The file compiles the same for every game.

#include "psyq/psyq_game_hooks.h"

#include "openpete_game_runtime.h"

/* Before registration, and for a game that registers no PSYQ descriptor,
 * every field is 0. */
static const psyq_game_hooks_t k_no_hooks = { 0 };
static const psyq_game_conf_t  k_no_conf  = { 0 };

const psyq_game_hooks_t* psyq_game_hooks(void) {
    const psyq_game_hooks_t* h = openpete_game_runtime()->psyq;
    return h ? h : &k_no_hooks;
}

/** @brief Return the running game's recomp.conf addresses, or the all-zero set when it names none. */
static const psyq_game_conf_t* conf(void) {
    const psyq_game_conf_t* c = psyq_game_hooks()->conf;
    return c ? c : &k_no_conf;
}

int psyq_game_stack_frame(psyq_frame_site_t site, psyq_game_frame_t* out) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    return h->stack_frame ? h->stack_frame(site, out) : 0;
}

void psyq_game_mirror_main(CPUState* cpu) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->mirror_main) h->mirror_main(cpu);
}
void psyq_game_mirror_resetgraph(CPUState* cpu, int mode) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->mirror_resetgraph) h->mirror_resetgraph(cpu, mode);
}
void psyq_game_mirror_vsync_callback(CPUState* cpu, uint32_t cb) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->mirror_vsync_callback) h->mirror_vsync_callback(cpu, cb);
}
void psyq_game_mirror_pad_init(CPUState* cpu, uint32_t buf0, uint32_t buf1) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->mirror_pad_init) h->mirror_pad_init(cpu, buf0, buf1);
}
void psyq_game_mirror_pad_slot2(CPUState* cpu) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->mirror_pad_slot2) h->mirror_pad_slot2(cpu);
}
void psyq_game_mirror_memcard_init(CPUState* cpu) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->mirror_memcard_init) h->mirror_memcard_init(cpu);
}

uint32_t psyq_game_rand_seed_addr(void) { return psyq_game_hooks()->rand_seed_addr; }
uint16_t psyq_game_root_counter2(void) { return psyq_game_hooks()->root_counter2; }
uint32_t psyq_game_libgpu_env_addr(void) { return psyq_game_hooks()->libgpu_env_addr; }
uint32_t psyq_game_libgpu_clear_block_addr(void) {
    return psyq_game_hooks()->libgpu_clear_block_addr;
}

uint32_t psyq_game_overlay_vaddr(void) { return conf()->overlay_vaddr; }
uint32_t psyq_game_libcd_block_addr(void) { return conf()->libcd_block_addr; }
uint32_t psyq_game_cd_read_cb_addr(void) { return conf()->cd_read_cb_addr; }

void psyq_game_cd_setloc(const void* cdl_loc) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->cd_setloc) h->cd_setloc(cdl_loc);
}
void psyq_game_cd_apply_buf(unsigned char* buf, int sectors) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->cd_apply_buf) h->cd_apply_buf(buf, sectors);
}
void psyq_game_cd_read_done(CPUState* cpu, uint32_t read_cb) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->cd_read_done) h->cd_read_done(cpu, read_cb);
}
int psyq_game_cd_read_in_load(CPUState* cpu) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    return h->cd_read_in_load ? h->cd_read_in_load(cpu) : 0;
}
uint32_t psyq_game_cd_read_delay_cap(void) { return psyq_game_hooks()->cd_read_delay_cap; }
int psyq_game_cd_read_streamed(CPUState* cpu) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    return h->cd_read_streamed ? h->cd_read_streamed(cpu) : 0;
}
int psyq_game_cd_spin_wait(CPUState* cpu, uint32_t addr) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    return h->cd_spin_wait ? h->cd_spin_wait(cpu, addr) : 0;
}
uint32_t psyq_game_load_window_word(void) { return psyq_game_hooks()->load_window_word; }
int psyq_game_load_window_active(uint32_t value) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    return h->load_window_active ? h->load_window_active(value) : 0;
}
uint32_t psyq_game_cd_raw_pc_min(void) {
    const uint32_t pc = psyq_game_hooks()->cd_raw_pc_min;
    return pc ? pc : 0xFFFFFFFFu;
}
uint32_t psyq_game_cd_done_witness_ra(void) { return psyq_game_hooks()->cd_done_witness_ra; }

void psyq_game_music_route(int sector, psyq_music_route_t* out) {
    const psyq_game_hooks_t* h = psyq_game_hooks();
    if (h->music_route) {
        h->music_route(sector, out);
        return;
    }
    out->track = -1;            /* no override by track */
    out->custom_path = NULL;    /* no custom song: plain XA at the sector */
}

const psyq_sfx_slots_t* psyq_game_sfx_slots(void) { return psyq_game_hooks()->sfx_slots; }
