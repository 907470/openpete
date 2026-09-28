// games/spyro-2/game_extras_impl.c, Spyro 2: Ripto's Rage!'s per-game contract.
//
// Identity is generated from recomp.conf (game_identity.c). The PSYQ address
// hooks defined below (rand seed slot, root counter 2) are hand-written and
// named in psyq_game_provides.h, which suppresses their inert defaults.
// Bring-up state: the overlay hooks, --skip-to-level and the freecam wiring
// carry real values; the remaining per-tick and CLI entries are stubs that
// core calls and that must not crash.

#include "game_extras.h"
#include "openpete_psx.h"
#include "spyro_overlay_table.h"
#include "psyq/psyq_game_hooks.h"
#include "skip_to_level.h"
#include "features/levelwarp.h"
#include "features/cutscene_skip.h"
#include "write_census.h"
#include "freecam_game.h"
#include "feature_registry.h"

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
/* game_on_boot and game_on_state_loaded are the shared bodies in
 * games/common/game_extras_overlay_resync.c. */

/* --- Per-tick hooks --- */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    /* --skip-to-level: drives the title's demo-timeout transition toward the
     * target and holds the demo flag clear. Inert without the flag. */
    spyro2_skip_on_tick(cpu, frame_counter);
    /* Dev-overlay level warp (M): drains a queued Jump into the game's
     * own level transition. Inert until one is clicked. */
    spyro2_levelwarp_on_tick(cpu);
    /* Scripted player placement (--player-pose) on the first placeable
     * tick, then scripted engage of the debug free camera (OPENPETE_FREECAM=1
     * / OPENPETE_FREECAM_POSE / OPENPETE_FREECAM_PATH) once gameplay is live;
     * placement first so one run can view the placed player. */
    freecam_player_pose_maybe_apply(cpu);
    freecam_maybe_autoengage(cpu);
    /* Write census: every fiber resume in a tick happens inside the draw's
     * VSync waits, after the view matrix has been built, so a store seen from
     * here until the next phase entry precedes it for that tick. */
    if (write_census_active()) write_census_set_phase(0);
}

/** @brief Runs this game's intercepts at a recompiled function's entry: the free camera, the write census, then the feature modules.
 *
 * The debug free camera (F), while engaged and gameplay is live, drives the
 * camera in place of the camera follow (0x8001D87C) and suppresses it and the
 * player update (0x8002A07C); disengaged it costs one bool test. Under the
 * write census the address becomes the entry column of later rows. An address
 * the free camera does not claim goes to the registered feature modules
 * (feature_list.c), which is how the shared presentation feeders reach their
 * hook addresses.
 * @param cpu The guest CPU state.
 * @param addr The guest address of the function being entered.
 * @return 1 when the recompiled body must return at once, else 0. */
int psx_override_dispatch_game(CPUState* cpu, uint32_t addr) {
    if (freecam_should_freeze(cpu, addr)) return 1;
    if (write_census_active()) write_census_note_entry(addr);
    return feature_registry_dispatch(cpu, addr);
}
void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
}
void game_on_hash_emit(uint32_t frame) { (void)frame; }

/* --- Savestate --- */

/* The extra savestate payload is the shared thunk layer's latch pack
 * (games/common/psyq/psyq_thunk_state.c). */

/* A level overlay resident with its state machine running (loader state -1).
 * Title, loading and the exit transition report "not live", which keeps the
 * in-memory snapshot users (rewind keyframes, runahead) out of them. */
int game_is_gameplay_live(CPUState* cpu) { return spyro2_skip_gameplay_live(cpu); }

/* --- CLI --- */
void game_set_skip_to_level(int level_id) { spyro2_skip_set(level_id); }
int  game_resolve_level_arg(const char* arg) { return spyro2_level_resolve(arg); }
int  game_validate_cheats_arg(const char* spec) { (void)spec; return -1; }
/* The target rides a --skip-to-level to its source level; a different explicit --skip-to-level refuses it. */
void game_set_skip_to_cutscene(int cutscene_id) {
    if (cutscene_id <= 0) return;
    int source = spyro2_cutscene_source_level(cutscene_id);
    if (source <= 0) return;
    int level = spyro2_skip_target();
    if (level > 0 && level != source) {
        fprintf(stderr, "openpete: --skip-to-cutscene: this cutscene starts "
                        "from level %d, not --skip-to-level %d; refused\n",
                source, level);
        return;
    }
    spyro2_cutscene_skip_arm(cutscene_id);
    if (level <= 0) spyro2_skip_set(source);
}
int game_resolve_cutscene_arg(const char* arg) { return spyro2_cutscene_resolve(arg); }
/* No sub-area targets: every --skip-to-area value is refused. */
void game_set_skip_to_area(int level_id, int area) { (void)level_id; (void)area; }
int  game_resolve_area_arg(const char* arg, int* out_area) {
    (void)arg; (void)out_area;
    fprintf(stderr, "%s has no --skip-to-area targets\n", game_get_display_name());
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

/* D_8006D144 (psyq .bss): rand (func_80059D2C) steps it, srand (func_80059D5C) overwrites it. */
uint32_t psyq_game_rand_seed_addr(void) { return 0x8006D144u; }

/* The reference boot's `lhu` at 0x8001C2DC (func_8001C2D8, stdutil.s) read this; it seeds srand. */
uint16_t psyq_game_root_counter2(void) { return 0x4876u; }

/* Disc layout is not mapped for this game yet. */
const game_disc_layout_t* game_get_disc_layout(void) { return NULL; }
