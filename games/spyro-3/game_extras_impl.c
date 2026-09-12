// games/spyro-3/game_extras_impl.c — Spyro: Year of the Dragon's per-game contract.
//
// Bring-up state: identity, memory layout, the overlay hooks, the CD-load
// classifier and --skip-to-level carry real values; the remaining per-tick
// and CLI entries are stubs. Core calls only fourteen of this header's
// declarations, and every stub below is one core will call and must not
// crash. The rest are defined because the header declares them.

#include "game_extras.h"
#include "openpete_psx.h"
#include "spyro_overlay_table.h"
#include "psyq_game_hooks.h"
#include "skip_to_level.h"
#include "openpete_log.h"
#include "openpete_fiber.h"

#include <stddef.h>
#include <stdio.h>
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

/* The load must keep a disc read in flight across at least 33 main-loop
 * frames (two VBLs each): the EXE's per-frame function at 0x8002CA50 walks a
 * checksum over the EXE and the loading overlay only on frames a read is
 * pending (func_800503F8 non-zero) and only while the loading state
 * 0x8006C518 is 2..7, 5120 bytes per frame over 164139 bytes, and the
 * state machine's state 7 waits for the walk's cursor (0x8006C6B8) to reach
 * its end before it hands over to the level. With reads completing inside
 * the frame that issued them the walk never starts. The first read of a
 * load is one sector, whose retry timeout is 121 VBLs (sectors + 120, at
 * func_80050400), so the per-read cap stays under it. 66 VBLs on that read
 * is the measured floor: 64 leaves the loader in state 7, 66 loads every
 * demo level. */
uint32_t game_get_load_delay_default(void) { return 66u; }

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
void game_on_boot(CPUState* cpu) { (void)cpu; }

/* --- Per-tick hooks --- */
void game_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
    /* --skip-to-level: drives the title's demo-timeout transition toward the
     * target and holds isDemoMode clear. Inert without the flag. */
    spyro3_skip_on_tick(cpu, frame_counter);
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

/* The extra savestate payload is the shared thunk layer's latch pack
 * (games/common/psyq_thunk_state.c). */

/* A level overlay resident (loadStage -1) with game.state in gameplay. Title,
 * loading, fade-in and the exit transition report "not live", which keeps the
 * in-memory snapshot users (rewind keyframes, runahead) out of them. */
int game_is_gameplay_live(CPUState* cpu) { return spyro3_skip_gameplay_live(cpu); }

/* The player struct is not localised for this game yet. Non-zero means "not
 * ready", which is what the position log expects before the struct exists. */
int game_get_player_position(int32_t* out_x, int32_t* out_y, int32_t* out_z) {
    (void)out_x; (void)out_y; (void)out_z;
    return -1;
}

/* --- CLI --- */
void game_set_skip_to_level(int level_id) { spyro3_skip_set(level_id); }
int  game_resolve_level_arg(const char* arg) { return spyro3_level_resolve(arg); }
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
uint32_t psyq_game_overlay_vaddr(void) { return 0x800742D0u; }

/* A read belongs to a level load while the loading overlay's state machine
 * (entry 0x8007494C, state word loadStage 0x8006C518) is in states 2..7, the
 * window the EXE's checksum walk (func_8002CA50) runs in; those reads are
 * issued through the asynchronous path (func_80050680) and polled from the
 * main loop by func_80050400. State 0 reads the loading overlay itself and
 * states below 2 belong to no load, so each pass through the loader opens a
 * new budget.
 *
 * The walk hashes only on frames a read is in flight, so the budget must be
 * spent on reads the walk observes. Entering a level through a portal
 * (game.state 0x8006E344 == GAMESTATE_LOADING_GLIDE, 7) runs the overlay's
 * loading-image sequence (func_loading_80076538) on pauseData sub-state
 * 0x8006FBC8: sub-state 2 fetches the loading image (the read issued at
 * 0x8007665C, flagged in D_8007232C) and polls it to completion without
 * calling the walk; the state machine's own reads start in sub-state 3, where
 * the walk runs every frame. A budget spent on the image fetch leaves the walk
 * with no read in flight, the cursor never reaches its end, and the overlay's
 * final `while (loadStage < 8) func_8002CA50()` spin (0x80076D98), which never
 * calls VSync, hangs. The image fetch is therefore classified as not in load.
 * The title path (GAMESTATE_LOADING_IMG, 5) reads its image synchronously
 * through func_80050578 before the state machine starts and is unaffected.
 *
 * The cap keeps every deferred read under the loader's shortest retry
 * timeout, 121 VBLs for a one-sector read. */
int psyq_game_cd_read_in_load(CPUState* cpu) {
    uint32_t state = cpu->read_word(0x8006C518u);
    if (state < 2u || state > 7u) return 0;
    if (cpu->read_word(0x8006E344u) == 7u && cpu->read_word(0x8006FBC8u) == 2u)
        return 0;
    return 1;
}
uint32_t psyq_game_cd_read_delay_cap(void) { return 100u; }

/* The checksum walk's verdict. Loading state 8 in func_8002CA50 (0x8002CD24)
 * takes it once per level load: when the cursor 0x8006C6B8 has not reached
 * 0x8007B6E4 or the CRC 0x8006C6B4 is not 0, a rand()-picked word of the
 * table at 0x8002AB38 is zeroed and the game misbehaves later with no
 * message; otherwise both words are cleared. Both conditions are readable at
 * the walk's entry on the state-8 frame, so the verdict is logged there,
 * once per load, before the game acts on it. A mismatch on the retail disc
 * means the walk ran over the wrong bytes or too few frames (a load path the
 * classifier above does not cover, or a load delay below the floor). */
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
    if (addr == 0x8002CA50u) checksum_verdict(cpu);
    return 0;
}

/* libcd is translated in this image, so CdReadCallback (func_8005DB08) stores the
 * read-completion handler here rather than in the thunk layer. */
uint32_t psyq_game_cd_read_cb_addr(void) { return 0x8006B3D0u; }

/* libcd's state block, read by the translated CdStatus, CdLastPos, CdMode and
 * CdLastCom. */
uint32_t psyq_game_libcd_block_addr(void) { return 0x8006B568u; }

/* Disc layout is not mapped for this game yet. */
const game_disc_layout_t* game_get_disc_layout(void) { return NULL; }

