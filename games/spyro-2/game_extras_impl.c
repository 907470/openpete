// games/spyro-2/game_extras_impl.c — Spyro 2: Ripto's Rage!'s per-game contract.
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
#include "skip_to_level.h"
#include "write_census.h"
#include "freecam_game.h"

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* --- Identity --- */
uint32_t    game_get_entry_point(void)    { return 0x8005478Cu; }
const char* game_get_exe_filename(void)   { return "SCUS_944.25"; }
uint32_t    game_get_expected_crc32(void) { return 0u; }
const char* game_get_display_name(void)   { return "Spyro 2: Ripto's Rage!"; }
const char* game_get_id(void)             { return "spyro-2"; }

/* The pad-read site is not localised for this game yet. Returning 0 makes the
 * pacing census report the pad-read phase as unmeasured rather than as zero
 * reads. */
uint32_t game_get_pad_read_pc(void) { return 0u; }

/* No load-read classifier for this game yet, so loads are instant. */
uint32_t game_get_load_delay_default(void) { return 0u; }

/* --- Memory layout --- */
uint32_t game_get_ram_size(void)              { return 2u * 1024u * 1024u; }
uint32_t game_get_compiled_range_start(void)  { return 0x80010000u; }
uint32_t game_get_compiled_range_end(void)    { return 0x8006D264u; }
/* main_BSS_END from the decomp's psx.map, through the largest overlay. */
uint32_t game_get_overlay_range_start(void)   { return 0x8006D264u; }
uint32_t game_get_overlay_range_end(void)     { return 0x8008C264u; }

/* --- Overlay handling --- */
int  game_on_overlay_load(uint32_t overlay_id) {
    spyro_set_active_overlay_by_magic(overlay_id);
    return 0;
}
void game_on_overlay_init(CPUState* cpu) { (void)cpu; }

void game_apply_runtime_quirks(CPUState* cpu) { (void)cpu; }
/* The write census installs over the store hooks before any guest
 * instruction runs; inert without OPENPETE_WRITE_CENSUS. */
void game_on_boot(CPUState* cpu) { write_census_install(cpu); }

/* --- Per-tick hooks --- */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    /* --skip-to-level: drives the title's demo-timeout transition toward the
     * target and holds the demo flag clear. Inert without the flag. */
    spyro2_skip_on_tick(cpu, frame_counter);
    /* Scripted engage of the debug free camera (OPENPETE_FREECAM=1 /
     * OPENPETE_FREECAM_POSE / OPENPETE_FREECAM_PATH) once gameplay is live. */
    freecam_maybe_autoengage(cpu);
    /* Write census: every fiber resume in a tick happens inside the draw's
     * VSync waits, after the view matrix has been built, so a store seen from
     * here until the next phase entry precedes it for that tick. */
    if (write_census_active()) write_census_set_phase(0);
}

/* Debug free camera (F): while engaged and gameplay is live, drive the
 * camera in place of the camera update and suppress it and the player update.
 * Inactive: one bool test. */
int psx_override_dispatch_game(CPUState* cpu, uint32_t addr) {
    if (freecam_should_freeze(cpu, addr)) return 1;
    if (write_census_active()) write_census_note_entry(addr);
    return 0;
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
    uint32_t off = 0x8006D264u & 0x001FFFFFu;
    uint32_t id  = (uint32_t)ram[off] | ((uint32_t)ram[off + 1] << 8) |
                   ((uint32_t)ram[off + 2] << 16) | ((uint32_t)ram[off + 3] << 24);
    spyro_set_active_overlay_by_magic(id);
    (void)cpu;
}

/* The extra savestate payload is the shared thunk layer's latch pack
 * (games/common/psyq_thunk_state.c). */

/* A level overlay resident with its state machine running (loader state -1).
 * Title, loading and the exit transition report "not live", which keeps the
 * in-memory snapshot users (rewind keyframes, runahead) out of them. */
int game_is_gameplay_live(CPUState* cpu) { return spyro2_skip_gameplay_live(cpu); }

/* The player struct at 0x80069FF0 holds the position in its first three
 * words. */
int game_get_player_position(int32_t* out_x, int32_t* out_y, int32_t* out_z) {
    uint8_t* ram = openpete_psx_get_ram();
    if (!ram) return -1;
    const uint32_t base = 0x80069FF0u - 0x80000000u;
    int32_t x, y, z;
    memcpy(&x, ram + base + 0, 4);
    memcpy(&y, ram + base + 4, 4);
    memcpy(&z, ram + base + 8, 4);
    *out_x = x;
    *out_y = y;
    *out_z = z;
    return 0;
}

/* --- CLI --- */
void game_set_skip_to_level(int level_id) { spyro2_skip_set(level_id); }
int  game_resolve_level_arg(const char* arg) { return spyro2_level_resolve(arg); }
int  game_validate_cheats_arg(const char* spec) { (void)spec; return -1; }
/* No cutscene targets: every --skip-to-cutscene value is refused. */
void game_set_skip_to_cutscene(int cutscene_id) { (void)cutscene_id; }
int  game_resolve_cutscene_arg(const char* arg) {
    (void)arg;
    fprintf(stderr, "%s has no --skip-to-cutscene targets\n", game_get_display_name());
    return -1;
}

/* The recompiler carries a compiled-in substitution for a `lui 0x8007` +
 * `addiu` pair composing into [0x8006FCF4, 0x80072E94), rewriting it to
 * `psx_runtime_g_sony_image_base + offset`. That window is Spyro 1's
 * g_SonyImage scratch pool, but the substitution is not data-driven, so it
 * fires on any game whose code composes an address in that span. Holding the
 * base at the canonical value makes the emitted expression reduce to the
 * original address, so the rewrite is identity here. Making the window
 * per-game belongs to the recompiler, not to this file. */
uint32_t psx_runtime_g_sony_image_base = 0x8006FCF4u;

/* Every overlay of this game loads at main_BSS_END; its first word is the
 * overlay id, which re-points the dispatcher after each load. */
uint32_t psyq_game_overlay_vaddr(void) { return 0x8006D264u; }

/* libcd is translated in this image, so CdReadCallback (func_800582A4) stores the
 * read-completion handler here rather than in the thunk layer. */
uint32_t psyq_game_cd_read_cb_addr(void) { return 0x800663B8u; }

/* libcd's state block, read by the translated CdStatus, CdLastPos, CdMode and
 * CdLastCom. */
uint32_t psyq_game_libcd_block_addr(void) { return 0x80066700u; }

/* Disc layout is not mapped for this game yet. */
const game_disc_layout_t* game_get_disc_layout(void) { return NULL; }
