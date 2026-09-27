// games/spyro-3/game_extras_impl.c — Spyro: Year of the Dragon's per-game contract.
//
// Identity and the address-only PSYQ hooks are generated from recomp.conf
// (game_identity.c). Bring-up state: the overlay hooks, the CD-load
// classifier, --skip-to-level and the freecam wiring carry real values; the
// remaining per-tick and CLI entries are stubs that core calls and that must
// not crash.
//
// Level-load delay: a load keeps a disc read in flight across at least 33
// main-loop iterations (two VBLs each). The EXE's per-tick function
// func_8002CA50 walks a checksum over the EXE and the loading overlay only
// while a read is pending (func_800503F8 non-zero) and loadStage 0x8006C518
// is 2..7, 5120 bytes per iteration over 164139 bytes, and loader state 7
// waits for the walk's cursor 0x8006C6B8 to reach its end before it hands
// over to the level; with reads completing inside the iteration that issued
// them the walk never starts. The first read of a load is one sector, whose
// retry timeout is 121 VBLs (sectors + 120, at func_80050680), so the per-read
// cap stays under it. 66 VBLs on that read is the measured floor: 64 leaves
// the loader in state 7, 66 loads every demo level.
//
// Load-read classifier (psyq_game_cd_read_in_load): a read belongs to a level
// load while loadStage (the loading overlay's state machine, entry
// 0x8007494C) is 2..7, the window the checksum walk runs in; those reads go
// through the asynchronous path func_80050680 and are polled from the main
// loop by func_800503F8. State 0 reads the loading overlay itself and states
// below 2 belong to no load, so each pass through the loader opens a new
// budget. Entering a level through a portal (game.state 0x8006E344 ==
// GAMESTATE_LOADING_GLIDE, 7) runs the overlay's loading-image sequence
// func_loading_80076538 on pauseData sub-state 0x8006FBC8: sub-state 2
// fetches the loading image (the read issued at 0x8007665C, flagged in
// D_8007232C) and polls it to completion without calling the walk; the state
// machine's own reads start in sub-state 3, where the walk runs every
// iteration. A budget spent on the image fetch leaves the walk with no read in
// flight, the cursor never reaches its end, and the overlay's final
// `while (loadStage < 8) func_8002CA50()` spin at 0x80076D98, which never
// calls VSync, hangs; the image fetch is therefore not in load. The title path
// (GAMESTATE_LOADING_IMG, 5) reads its image synchronously through
// func_80050578 before the state machine starts and is unaffected.

#include "game_extras.h"
#include "openpete_psx.h"
#include "spyro_overlay_table.h"
#include "psyq/psyq_game_hooks.h"
#include "write_census.h"
#include "freecam_game.h"
#include "skip_to_level.h"
#include "features/levelwarp.h"
#include "features/cutscene_skip.h"
#include "features/skip_to_area.h"
#include "openpete_log.h"
#include "feature_registry.h"   /* the registered feature modules' dispatch tables */
#include "openpete_fiber.h"
#include "renderer/text_moby.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* The pad-read site is not localised for this game yet. Returning 0 makes the
 * pacing census report the pad-read phase as unmeasured rather than as zero
 * reads. */
uint32_t game_get_pad_read_pc(void) { return 0u; }

/* The measured floor for the checksum walk (the level-load delay note in the file header). */
uint32_t game_get_load_delay_default(void) { return 66u; }

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
    /* --skip-to-area: queues the sub-area entry once the level is live and
     * reads its release, before the level skip's fast-forward consults it.
     * Inert without the flag. */
    spyro3_area_on_tick(cpu, frame_counter);
    /* --skip-to-cutscene: queues the row's WarpToLevel call on its route's
     * tick. Inert without the flag. */
    spyro3_cutscene_on_tick(cpu, frame_counter);
    /* --skip-to-level: drives the title's demo-timeout transition toward the
     * target and holds isDemoMode clear. Inert without the flag. */
    spyro3_skip_on_tick(cpu, frame_counter);
    /* Dev-overlay level warp (M): drains a queued Jump into the game's
     * own level transition. Inert until one is clicked. */
    spyro3_levelwarp_on_tick(cpu);
    /* Scripted player placement (--player-pose) on the first placeable
     * tick, then scripted engage of the debug free camera (OPENPETE_FREECAM=1
     * / OPENPETE_FREECAM_POSE / OPENPETE_FREECAM_PATH) once gameplay is live;
     * placement first so one run can view the placed player. The engage
     * waits for a --skip-to-area target's release: the sub-area transition
     * re-seeds the camera from the game's and would discard the pose. */
    freecam_player_pose_maybe_apply(cpu);
    if (!spyro3_area_pending()) freecam_maybe_autoengage(cpu);
    /* Write census: every fiber resume in a tick happens inside Draw's VSync
     * waits, after the view matrix has been built, so a store seen from here
     * until the next phase entry precedes it for that tick. */
    if (write_census_active()) write_census_set_phase(0);

    /* The host text layers, drawn with the game's own 3D glyph models: the
     * shimmer's clock first, so the settings menu and the OSD toasts are lit
     * by the same instant of it, then the performance readout, the menu and
     * the toasts. Each reads model geometry and writes renderer state only. */
    text_moby_native_shimmer_tick();
    text_moby_native_perf_overlay(cpu);
    text_moby_native_menu(cpu);
    text_moby_native_osd(cpu);
    text_moby_native_version(cpu);
}
void game_on_frame(CPUState* cpu, uint32_t frame_counter) {
    (void)cpu; (void)frame_counter;
}
void game_on_hash_emit(uint32_t frame) { (void)frame; }

/* --- Savestate --- */

/* The extra savestate payload is the shared thunk layer's latch pack
 * (games/common/psyq/psyq_thunk_state.c). */

/* A level overlay resident (loadStage -1) with game.state in gameplay. Title,
 * loading, fade-in and the exit transition report "not live", which keeps the
 * in-memory snapshot users (rewind keyframes, runahead) out of them. */
int game_is_gameplay_live(CPUState* cpu) { return spyro3_skip_gameplay_live(cpu); }

/* --- CLI --- */
void game_set_skip_to_level(int level_id) { spyro3_skip_set(level_id); }
int  game_resolve_level_arg(const char* arg) { return spyro3_level_resolve(arg); }
int  game_validate_cheats_arg(const char* spec) { (void)spec; return -1; }
/* A row starting from a level rides a --skip-to-level there; a contradicting --skip-to-level refuses it. */
void game_set_skip_to_cutscene(int cutscene_id) {
    if (cutscene_id > 0) spyro3_cutscene_arm(cutscene_id, spyro3_skip_target());
}
int game_resolve_cutscene_arg(const char* arg) { return spyro3_cutscene_resolve(arg); }
void game_set_skip_to_area(int level_id, int area) { spyro3_area_set(level_id, area); }
int  game_resolve_area_arg(const char* arg, int* out_area) {
    return spyro3_area_resolve(arg, out_area);
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

/* loadStage 2..7 except the portal loading-image fetch (the load-read classifier note in the file header). */
int psyq_game_cd_read_in_load(CPUState* cpu) {
    uint32_t state = cpu->read_word(0x8006C518u);
    if (state < 2u || state > 7u) return 0;
    if (cpu->read_word(0x8006E344u) == 7u && cpu->read_word(0x8006FBC8u) == 2u)
        return 0;
    return 1;
}
uint32_t psyq_game_cd_read_delay_cap(void) { return 100u; }

/* loadStage: 2..7 the checksum walk, one step per iteration, 8..12 the level reads and parses, then -1. */
uint32_t psyq_game_load_window_word(void) { return 0x8006C518u; }

/* The overlay base: the title overlay's disc check writes CD-ROM commands from 0x8007BB3C, libcd below it. */
uint32_t psyq_game_cd_raw_pc_min(void) { return 0x800742D0u; }

/* func_800503F8's CdSync(1) at 0x800504E0, taken when D_8006E480 (cleared by callback func_80050504) is 0. */
uint32_t psyq_game_cd_done_witness_ra(void) { return 0x800504E8u; }
int      psyq_game_load_window_active(uint32_t value) { (void)value; return 0; }

/* D_80073F3C (psyq .bss): rand (func_8005C644) steps it, srand (func_8005C674) overwrites it. */
uint32_t psyq_game_rand_seed_addr(void) { return 0x80073F3Cu; }

/* The reference boot's `lhu` at 0x8004F6A4 (func_8004F6A0, stdutil.s) read this; it seeds srand. */
uint16_t psyq_game_root_counter2(void) { return 0x58C7u; }

/** @brief Log whether the level-load checksum walk passed, once per load, at entry to func_8002CA50.
 *
 * Loading state 8 in func_8002CA50 (0x8002CD24) takes the walk's result
 * once per level load: when the cursor 0x8006C6B8 has not reached
 * 0x8007B6E4 or the CRC 0x8006C6B4 is not 0, a rand()-picked word of the
 * table at 0x8002AB38 is zeroed and the game misbehaves later with no
 * message; otherwise both words are cleared. Both conditions are readable
 * at the function's entry on the first call with the loading state word
 * 0x8006C518 at 8, so the result is logged there before the game acts on
 * it: a warning on failure, an info line on pass. A failure on the retail
 * disc means the walk ran over the wrong bytes or too few main-loop passes
 * (a load path the classifier above does not cover, or a load delay below
 * the floor).
 * @param cpu Guest CPU state. */
static void checksum_verdict(CPUState* cpu) {
    static uint32_t s_last_state = 0;
    uint32_t state = cpu->read_word(0x8006C518u);
    if (state == 8u && s_last_state != 8u) {
        uint32_t cursor = cpu->read_word(0x8006C6B8u);
        uint32_t crc    = cpu->read_word(0x8006C6B4u);
        if (cursor != 0x8007B6E4u || crc != 0u)
            OPL_WARN_F("cd", "checksum walk failed: cursor 0x%08X (end 0x8007B6E4), "
                       "crc 0x%08X (want 0) at vbl=%d; the game will poison a word "
                       "of its table at 0x8002AB38",
                       cursor, crc, SDL_GetAtomicInt(&openpete_vbl));
        else
            OPL_INFO_F("cd", "checksum walk passed (vbl=%d)",
                       SDL_GetAtomicInt(&openpete_vbl));
    }
    s_last_state = state;
}

int psx_override_dispatch_game(CPUState* cpu, uint32_t addr) {
    /* Debug free camera (F): while engaged and gameplay is live, drive the
     * camera in place of func_8001204C and suppress it and the player update
     * func_8003E83C. Inactive: one bool test. */
    if (freecam_should_freeze(cpu, addr)) return 1;
    if (addr == 0x8002CA50u) checksum_verdict(cpu);
    if (write_census_active()) write_census_note_entry(addr);
    return feature_registry_dispatch(cpu, addr);
}

/* Disc layout is not mapped for this game yet. */
const game_disc_layout_t* game_get_disc_layout(void) { return NULL; }

