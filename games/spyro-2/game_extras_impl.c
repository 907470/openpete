// games/spyro-2/game_extras_impl.c — Spyro 2: Ripto's Rage!'s per-game contract.
//
// Identity and the address-only PSYQ hooks are generated from recomp.conf
// (game_identity.c). Bring-up state: the overlay hooks, --skip-to-level and
// the freecam wiring carry real values; the remaining per-tick and CLI
// entries are stubs that core calls and that must not crash.

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

/* The pad-read site is not localised for this game yet. Returning 0 makes the
 * pacing census report the pad-read phase as unmeasured rather than as zero
 * reads. */
uint32_t game_get_pad_read_pc(void) { return 0u; }

/* No load-read classifier for this game yet, so loads are instant. */
uint32_t game_get_load_delay_default(void) { return 0u; }

/* --- Overlay handling --- */
int  game_on_overlay_load(uint32_t overlay_id) {
    spyro_set_active_overlay_by_magic(overlay_id);
    return 0;
}
void game_on_overlay_init(CPUState* cpu) { (void)cpu; }


void game_apply_runtime_quirks(CPUState* cpu) { (void)cpu; }
/* The write census installs over the store hooks before any guest
 * instruction runs; inert without OPENPETE_WRITE_CENSUS. */
void game_on_boot(CPUState* cpu) {
    /* The atlas and the cutscene overlays are staged into a scratch buffer and
     * copied into the overlay region, so the disc read that fetched them never
     * named them; the dispatcher re-derives the resident one from the id word
     * instead. */
    spyro_overlay_set_resync(psyq_overlay_resync_from_ram);
    /* The write census installs over the store hooks before any guest
     * instruction runs; inert without OPENPETE_WRITE_CENSUS. */
    write_census_install(cpu);
}

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
    psyq_overlay_resync_from_ram();
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

/* Disc layout is not mapped for this game yet. */
const game_disc_layout_t* game_get_disc_layout(void) { return NULL; }
