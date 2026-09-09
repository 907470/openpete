// games/spyro-3/game_extras_impl.c — Spyro: Year of the Dragon's per-game contract.
//
// Bring-up state: identity, memory layout and the overlay hooks carry real
// values; the per-tick, savestate and CLI-resolver entries are stubs. Core
// calls only fourteen of this header's declarations, and every stub below is
// one core will call and must not crash. The rest are defined because the
// header declares them.

#include "game_extras.h"
#include "openpete_psx.h"
#include "spyro_overlay_table.h"
#include "psyq_game_hooks.h"

#include <stddef.h>
#include <stdint.h>

/* --- Identity --- */
uint32_t    game_get_entry_point(void)    { return 0x80059444u; }
const char* game_get_exe_filename(void)   { return "SCUS_944.67"; }
uint32_t    game_get_expected_crc32(void) { return 0u; }
const char* game_get_display_name(void)   { return "Spyro: Year of the Dragon"; }
const char* game_get_id(void)             { return "spyro-3"; }

/* The pad-read site is not localised for this game yet. Returning 0 makes the
 * pacing census report the pad-read phase as unmeasured rather than as zero
 * reads. */
uint32_t game_get_pad_read_pc(void) { return 0u; }

/* --- Memory layout --- */
uint32_t game_get_ram_size(void)              { return 2u * 1024u * 1024u; }
uint32_t game_get_compiled_range_start(void)  { return 0x80010000u; }
uint32_t game_get_compiled_range_end(void)    { return 0x800742D0u; }
/* main_BSS_END from the decomp's psx.map, through the largest overlay. */
uint32_t game_get_overlay_range_start(void)   { return 0x800742D0u; }
uint32_t game_get_overlay_range_end(void)     { return 0x800A1AD0u; }

/* --- Overlay handling --- */
int  game_on_overlay_load(uint32_t overlay_id) {
    spyro_set_active_overlay_by_magic(overlay_id);
    return 0;
}
void game_on_overlay_init(CPUState* cpu) { (void)cpu; }

void game_apply_runtime_quirks(CPUState* cpu) { (void)cpu; }

/* --- Per-tick hooks --- */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
}
void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
}
void game_on_hash_emit(uint32_t frame) { (void)frame; }

/* --- Savestate --- */
/* Re-point the dispatcher after a restore: the overlay resident in the
 * restored RAM decides which dispatcher is valid, and the id is the first word
 * of the overlay region. */
void game_on_state_loaded(CPUState* cpu) {
    uint8_t* ram = openpete_psx_get_ram();
    if (!ram) return;
    uint32_t off = 0x800742D0u & 0x001FFFFFu;
    uint32_t id  = (uint32_t)ram[off] | ((uint32_t)ram[off + 1] << 8) |
                   ((uint32_t)ram[off + 2] << 16) | ((uint32_t)ram[off + 3] << 24);
    spyro_set_active_overlay_by_magic(id);
    (void)cpu;
}

uint32_t game_state_extra_size(void) { return 0u; }
void game_state_extra_save(void* dst) { (void)dst; }
void game_state_extra_load(const void* src, uint32_t size) {
    (void)src; (void)size;
}

/* No gameplay-live probe yet: reporting "not live" keeps the in-memory
 * snapshot users (rewind keyframes, runahead) disabled rather than letting
 * them capture a state whose steadiness has not been established. */
int game_is_gameplay_live(CPUState* cpu) { (void)cpu; return 0; }

/* The player struct is not localised for this game yet. Non-zero means "not
 * ready", which is what the position log expects before the struct exists. */
int game_get_player_position(int32_t* out_x, int32_t* out_y, int32_t* out_z) {
    (void)out_x; (void)out_y; (void)out_z;
    return -1;
}

/* --- CLI --- */
void game_set_skip_to_level(int level_id) { (void)level_id; }
int  game_resolve_level_arg(const char* arg) { (void)arg; return -1; }
int  game_validate_cheats_arg(const char* spec) { (void)spec; return -1; }

/* The recompiler carries a compiled-in substitution for a `lui 0x8007` +
 * `addiu` pair composing into [0x8006FCF4, 0x80072E94), rewriting it to
 * `psx_runtime_g_sony_image_base + offset`. That window is Spyro 1's
 * g_SonyImage scratch pool, but the substitution is not data-driven, so it
 * fires on any game whose code composes an address in that span. Holding the
 * base at the canonical value makes the emitted expression reduce to the
 * original address, so the rewrite is identity here. Making the window
 * per-game belongs to the recompiler, not to this file. */
uint32_t psx_runtime_g_sony_image_base = 0x8006FCF4u;

/* This game waits for a frame by polling VSync(-1) until the count changes
 * rather than by calling VSync(0), so the layer must let a VBL land during the
 * poll or the loop never exits. */
unsigned psyq_game_vsync_poll_advance(void) { return 1; }

/* Every overlay of this game loads at main_BSS_END; its first word is the
 * overlay id, which re-points the dispatcher after each load. */
uint32_t psyq_game_overlay_vaddr(void) { return 0x800742D0u; }

/* libcd is translated in this image, so CdReadCallback (func_8005DB08) stores the
 * read-completion handler here rather than in the thunk layer. */
uint32_t psyq_game_cd_read_cb_addr(void) { return 0x8006B3D0u; }

/* Disc layout is not mapped for this game yet. */
const game_disc_layout_t* game_get_disc_layout(void) { return NULL; }
