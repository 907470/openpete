// games/spyro-3/game_extras_impl.c - Spyro: Year of the Dragon's per-game contract.
//
// Defines the runtime descriptor spyro3_game_runtime (openpete_game_runtime.h)
// over the hooks below and the tables of the game's other TUs. Identity and
// the PSYQ descriptor's recomp.conf addresses are generated from recomp.conf
// (game_identity.c). Bring-up state: the overlay hooks, the CD-load
// classifier, --skip-to-level and the freecam wiring carry real values; the
// descriptor leaves the hooks the game does not implement NULL.
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
// Load-read classifier (spyro3_cd_read_in_load): a read belongs to a level
// load when the walk can observe it in flight, which holds for exactly the
// reads the loadStage state machine issues. func_8002CA50 walks first and
// then dispatches on loadStage; stages 2..7 run the loading overlay's state
// machine func_loading_8007494C (0x8007494C..0x80075114, called only from
// 0x8002CD14), whose reads go through the asynchronous reader func_80050680
// and are polled by the next iteration's walk. State 0 reads the loading
// overlay itself and states below 2 belong to no load, so each pass through
// the loader opens a new budget. A read is classified at its CdRead: the
// asynchronous reader's call returns to 0x80050758, and the reader saved its
// own return address at sp+0x28 (0x800506A8) before it, which names the
// issuer. Reads issued outside the state machine while loadStage is 2..7
// are not in load. The loading overlay's image fetches are such reads: the
// level-entry glide (game.state 0x8006E344 == 7, func_loading_80076538
// sub-state 2, call at 0x80076658) and the vehicle ride (game.state 12,
// func_loading_80078678 sub-state 2, call at 0x80078798) each issue one with
// loadStage already 2 and poll it to completion without calling the walk;
// the state machine's reads start in sub-state 3. A budget spent on the
// image fetch leaves the walk with no read in flight, the cursor never
// reaches its end, and the overlay's final `while (loadStage < 8)
// func_8002CA50()` spin (0x80076D98 glide, 0x80078F40 vehicle), which never
// calls VSync, hangs. The synchronous reader func_80050578 spins at
// 0x80050650 on the read's completion flag without calling VSync, so a
// deferred completion never arrives inside its spin: the game-over image
// fetch (func_loading_80076FEC sub-state 1, issued with loadStage already 2)
// and the title path's image read (GAMESTATE_LOADING_IMG, 5) both go through
// it and both complete in the iteration that issued them.

#include "game_extras.h"
#include "game_host_desc.h"
#include "game_mod_desc.h"
#include "openpete_modsave.h"
#include "openpete_launcher.h"
#include "psyq/psyq_thunk_state.h"
#include "openpete_psx.h"
#include "spyro_overlay_table.h"
#include "psyq/psyq_game_hooks.h"
#include "dispatch_preamble.h"
#include "freecam_game.h"
#include "skip_to_level.h"
#include "features/levelwarp.h"
#include "features/areawarp.h"
#include "features/cutscene_skip.h"
#include "features/skip_to_area.h"
#include "features/state_log.h"
#include "features/antipiracy_scan.h"
#include "features/moby_update_all.h"
#include "openpete_log.h"
#include "feature_registry.h"   /* the registered feature modules' dispatch tables */
#include "features/levelwarp_ui.h"
#include "openpete_fiber.h"
#include "renderer/text_moby.h"
#include "spyro-3/gen/spyro3_sfx_names.h" /* generated: the 256 SoundTable slot names */
#include "scan_watch.h"
#include "code_patch_guard.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* The boot and savestate-restore hooks run the shared bodies in
 * games/common/game_extras_overlay_resync.c, then the scanned-range watch. */
void game_overlay_resync_on_boot(CPUState* cpu);
void game_overlay_resync_on_state_loaded(CPUState* cpu);

/** @brief Install the resident-overlay resync and the write census, then arm the scanned-range watch over them.
 *
 * Last, registers the exit report of the stub guard (code_patch_guard.h).
 * @param cpu Guest CPU state. */
static void spyro3_on_boot(CPUState* cpu) {
    game_overlay_resync_on_boot(cpu);
    spyro3_scan_watch_boot(cpu);
    spyro3_code_patch_guard_boot();
}

/** @brief Re-point the overlay dispatcher after a savestate restore, then report the scanned bytes the restore changed.
 *
 * @param cpu Guest CPU state. */
static void spyro3_on_state_loaded(CPUState* cpu) {
    game_overlay_resync_on_state_loaded(cpu);
    spyro3_scan_watch_state_loaded();
}

/** @brief Run the pre-tick hooks the level skip reads: the scanned-range watch, then the area and cutscene skips.
 *
 * The watch resolves the last tick's held overlay-window stores and resyncs
 * its shadow before this tick's hooks store. --skip-to-area queues the
 * sub-area entry once the level is live and reads its release, before the
 * level skip's fast-forward consults it; --skip-to-cutscene queues the
 * row's WarpToLevel call on its route's tick. Both are inert without their
 * flag.
 * @param cpu Guest CPU state.
 * @param frame_counter The openpete_frame counter before the tick. */
static void spyro3_frame_pre_lead(CPUState* cpu, uint32_t frame_counter) {
    spyro3_scan_watch_tick(cpu);
    spyro3_area_on_tick(cpu, frame_counter);
    spyro3_cutscene_on_tick(cpu, frame_counter);
}

/* The pre-tick sequence is the shared body in games/common/game_frame_pre.c.
 * The level skip drives the title's demo-timeout transition toward the
 * target and holds isDemoMode clear; the level warp drains a queued Jump
 * into the game's own level transition; the state log
 * (OPENPETE_S3_STATE_LOG) prints a stderr line per change of the game's
 * state words, read after the skip's and warp's stores for this tick. The
 * free-camera engage waits for a --skip-to-area target's release: the
 * sub-area transition re-seeds the camera from the game's and would discard
 * the pose. */
static const game_frame_pre_t k_frame_pre = {
    .lead                = spyro3_frame_pre_lead,
    .skip_on_tick        = spyro3_skip_on_tick,
    .levelwarp_on_tick   = spyro3_levelwarp_on_tick,
    .trail               = spyro3_state_log_on_tick,
    .freecam_engage_held = spyro3_area_pending,
};

/** @brief Report Spyro 3's feature rows and its level, cutscene and area vocabularies to the surface dump.
 *
 * @param sink The dump's collector. */
static void spyro3_surface(struct openpete_surface_sink* sink) {
    levelwarp_surface(sink, levelwarp_level_rows);
    spyro3_areawarp_surface(sink);
    game_level_surface(&spyro3_levels, sink);
    spyro3_cutscene_surface(sink);
    spyro3_area_surface(sink);
}

/** @brief Arm a --skip-to-cutscene target; a row starting from a level rides a --skip-to-level there, and a contradicting --skip-to-level refuses it.
 *
 * @param cutscene_id The cutscene id; 0 or less disables it. */
static void spyro3_set_skip_to_cutscene(int cutscene_id) {
    if (cutscene_id > 0) spyro3_cutscene_arm(cutscene_id, spyro3_skip_target());
}

/** @brief Classify a data read as part of a level load: a read the loadStage state machine issues in loadStage 2..7.
 *
 * The load-read classifier note in the file header gives the reasons.
 * @param cpu Guest CPU state at the CdRead call (ra is the call site).
 * @return 1 when the read is part of a level load, else 0. */
static int spyro3_cd_read_in_load(CPUState* cpu) {
    if (cpu->ra != 0x80050758u) return 0;
    const uint32_t state = cpu->read_word(0x8006C518u);
    if (state < 2u || state > 7u) return 0;
    const uint32_t issuer = cpu->read_word(cpu->sp + 0x28u);
    return issuer >= 0x8007494Cu && issuer < 0x80075114u;
}

/* The level sound tables, loaded by func_8002C9F4 (loaders.c):
 * g_SoundTablePtr (0x8006C654) and g_SpuDefinitionsPtr (0x8006C6A0).
 * g_ActiveSounds[24] at 0x8006FCE4, 0x2C bytes each, voice i at entry i;
 * the pitch word unk18 (func_8003C0B0) is the voice's SpuVoiceAttr
 * pitch. */
static const psyq_sfx_slots_t k_sfx_slots = {
    0x8006C654u, 0x8006C6A0u, 0x8006FCE4u, 0x2Cu, 0x18u,
    0u, /* the level bank uploads in chunks: every SpuWrite is a partial write */
    spyro3_sfx_slot_names,
};

/** @brief Log whether the level-load checksum walk passed, once per load, at entry to func_8002CA50.
 *
 * Loading state 8 in func_8002CA50 (0x8002CD24) takes the walk's result
 * once per level load: when the cursor 0x8006C6B8 has not reached
 * 0x8007B6E4 or the CRC 0x8006C6B4 is not 0, a rand()-picked word of the
 * code of func_8002AB38 is zeroed (guest RAM is never executed here, so
 * the store changes no behaviour); otherwise both words are cleared. Both conditions are readable
 * at the function's entry on the first call with the loading state word
 * 0x8006C518 at 8, so the result is logged there before the game acts on
 * it: a warning on failure, an info line on pass. A failure on the retail
 * disc means the walk ran over the wrong bytes or too few main-loop passes
 * (a load path the classifier above does not cover, or a load delay below
 * the floor). Each verdict is counted for the exit report, and a failure
 * names the enabled mods.
 * @param cpu Guest CPU state. */
static void checksum_verdict(CPUState* cpu) {
    static uint32_t s_last_state = 0;
    uint32_t state = cpu->read_word(0x8006C518u);
    if (state == 8u && s_last_state != 8u) {
        uint32_t cursor = cpu->read_word(0x8006C6B8u);
        uint32_t crc    = cpu->read_word(0x8006C6B4u);
        const int passed = cursor == 0x8007B6E4u && crc == 0u;
        if (!passed)
            OPL_WARN_F("cd", "checksum walk failed: cursor 0x%08X (end 0x8007B6E4), "
                       "crc 0x%08X (want 0) at vbl=%d; the game will zero a word "
                       "of func_8002AB38",
                       cursor, crc, SDL_GetAtomicInt(&openpete_vbl));
        else
            OPL_INFO_F("cd", "checksum walk passed (vbl=%d)",
                       SDL_GetAtomicInt(&openpete_vbl));
        spyro3_scan_watch_verdict(passed);
    }
    s_last_state = state;
}

/** @brief Run Spyro 3's verdict hooks at a function entry: the level-load checksum log and the level overlays' scan observer.
 *
 * @param cpu Guest CPU state.
 * @param addr The entered function's guest vaddr. */
static void spyro3_entry_verdicts(CPUState* cpu, uint32_t addr) {
    if (addr == 0x8002CA50u) checksum_verdict(cpu);
    spyro3_antipiracy_scan_on_entry(cpu, addr);
}

/** @brief Runs the Spyro 3 intercepts at the entry of every recompiled function.
 *
 * In order, through game_dispatch_preamble: the free camera's freeze test,
 * which ends the function at once while it drives the camera in place of
 * func_8001204C or suppresses the player update func_8003E83C; the
 * level-load checksum log at func_8002CA50 and the level overlays' scan
 * observer; the write census's entry note;
 * the mod override chains. Then the stub guard (code_patch_guard.h), which
 * ends a call of func_8005663C or func_800512E4 whose entry words in guest
 * RAM are `jr ra; nop`; it follows the mod chains, so a mod's base() into a
 * stubbed function reaches the stub. Then the OPENPETE_D1 gem mark
 * (moby_update_all_note_entry), then the feature modules' dispatch tables. Called from
 * psx_override_dispatch, which the generated bodies call before their own code.
 *
 * @param cpu The guest CPU state.
 * @param addr The entry address of the function being dispatched.
 * @return 1 when the recompiled body must return at once, else 0. */
static int spyro3_override_dispatch(CPUState* cpu, uint32_t addr) {
    if (game_dispatch_preamble(cpu, addr, spyro3_entry_verdicts)) return 1;
    if (spyro3_code_patch_guard(cpu, addr)) return 1;
    moby_update_all_note_entry(cpu, addr);
    return feature_registry_dispatch(cpu, addr);
}



/* Stream reads. The disc reader in str.c has two entries over the same
 * request words: func_80050680 issues a read (CdRead returns to 0x80050758)
 * and returns, its callers polling the busy check func_800503F8 from their
 * main loop; func_80050578 issues one and spins on func_800503F8 before and
 * after it (calls returning to 0x800505B0 and 0x80050658) without calling
 * VSync. func_800503F8 reports busy while D_8006E480 is set (the read
 * callback func_80050504 clears it) and, once clear, until CdSync(1)
 * reports the closing CdlPause complete. A cutscene plays with game.state
 * 0x8006E344 at 6, and each cutscene overlay streams through
 * func_80050680. */
/** @brief Classify a read as a cutscene stream read: the asynchronous reader's CdRead while game.state is cutscene playback (6).
 *
 * @param cpu Guest CPU state at the CdRead call (ra is the call site).
 * @return 1 for a stream read, else 0. */
static int spyro3_cd_read_streamed(CPUState* cpu) {
    return cpu->ra == 0x80050758u && cpu->read_word(0x8006E344u) == 6u;
}

/* The tables of the game's other TUs the descriptor carries. */
extern const openpete_game_identity_t spyro3_game_identity;   /* gen/game_identity.c */
extern const game_layout_t            spyro3_game_layout;     /* renderer/layout.c */
extern const ui_game_t                spyro3_ui_game;         /* renderer/layout.c */
extern const terrain_game_t           spyro3_terrain_game;    /* renderer/terrain_game.c */
extern const moby_game_s23_consts_t   spyro3_moby_game_s23;   /* renderer/moby_game.c */
extern const player_game_s23_consts_t spyro3_player_game_s23; /* renderer/player_game.c */
extern const portal_game_s23_consts_t spyro3_portal_game_s23; /* renderer/portal_game.c */
extern const freecam_game_desc        spyro3_freecam_desc;    /* features/freecam.c */
extern const game_orchestrator_t      spyro23_orchestrator;   /* common renderer/reextract_spyro23.c */
extern const openpete_feature_list_t  spyro3_feature_list;    /* feature_list.c */
void spyro23_interp_register(void);                            /* common renderer/interp_regions_spyro23.c */
extern const psyq_game_conf_t         spyro3_psyq_conf;       /* gen/game_identity.c */
extern const psyq_stub_table_t        spyro3_psyq_stub_table; /* gen/psyq_stub_table.c */

/* The shared PSYQ thunk layer's hooks. load_window_active is NULL: no
 * loader-state value opens a window the verifier skips. */
static const psyq_game_hooks_t k_psyq = {
    .conf               = &spyro3_psyq_conf,
    .stubs              = &spyro3_psyq_stub_table,
    /* D_80073F3C (psyq .bss): rand (func_8005C644) steps it, srand
     * (func_8005C674) overwrites it. */
    .rand_seed_addr     = 0x80073F3Cu,
    /* The reference boot's `lhu` at 0x8004F6A4 (func_8004F6A0, stdutil.s)
     * read this; it seeds srand. */
    .root_counter2      = 0x58C7u,
    .cd_read_in_load    = spyro3_cd_read_in_load,
    .cd_read_delay_cap  = 100u,
    .cd_read_streamed   = spyro3_cd_read_streamed,
    /* loadStage: 2..7 the checksum walk, one step per iteration, 8..12 the
     * level reads and parses, then -1. */
    .load_window_word   = 0x8006C518u,
    /* The overlay base: the title overlay's disc check writes CD-ROM
     * commands from 0x8007BB3C, libcd below it. */
    .cd_raw_pc_min      = 0x800742D0u,
    /* func_800503F8's CdSync(1) at 0x800504E0, taken when D_8006E480
     * (cleared by callback func_80050504) is 0. */
    .cd_done_witness_ra = 0x800504E8u,
    .sfx_slots          = &k_sfx_slots,
};

GAME_MOD_TABLES_DECLARE(spyro3);
const openpete_moby_ops_t* spyro3_moby_ops(void);

extern const openpete_save_desc_t spyro3_save_desc; /* mods/mod_save_desc.c */

/** @brief Return Spyro 3's mod-layer descriptor: its generated SDK symbol table and overlay entry arrays, and the scanned-range guard.
 *
 * Built on the first call into static storage. Overlay-window overrides
 * bind through override_ovl or a name scoped to one overlay only
 * (legacy_window_links 0). The moby ops are mods/mod_moby_ops.c's, every
 * render channel is a material channel, and mod text draws through the
 * text-moby rasterizer. Level ops, the sfx canonicaliser and the
 * render-function list stay NULL, so the loader treats those surfaces as
 * unsupported on this game; the arena keeps the core default base. The guest-byte guard is
 * scan_watch.h's spyro3_scan_guard.
 * @return The descriptor, never NULL. */
static const game_mod_desc_t* spyro3_mod_desc(void) {
    static game_mod_desc_t s_desc;
    static int             s_built = 0;
    if (!s_built) {
        GAME_MOD_TABLES_FILL(&s_desc, spyro3);
        s_desc.moby_ops     = spyro3_moby_ops();
        s_desc.channel_mask = OPENPETE_MODS_CHANNELS_ALL;
        s_desc.text_emit    = text_moby_native_mod_emit;
        s_desc.save_desc    = &spyro3_save_desc;
        s_desc.guest_guard  = &spyro3_scan_guard;
        s_built = 1;
    }
    return &s_desc;
}

static const game_host_desc_t k_host = {
    .ui              = &spyro3_ui_game,
    .terrain         = &spyro3_terrain_game,
    .moby            = &moby_game_spyro23,
    .player          = &player_game_spyro23,
    .moby_s23        = &spyro3_moby_game_s23,
    .player_s23      = &spyro3_player_game_s23,
    .portal_s23      = &spyro3_portal_game_s23,
    .freecam         = &spyro3_freecam_desc,
    .orchestrator    = &spyro23_orchestrator,
    .interp_register = spyro23_interp_register,
    .frame_pre       = &k_frame_pre,
};

/* The disc image the User docs pin, labelled by name in the library. */
static const openpete_launcher_known_disc k_known_discs[] = {
    {"a313c9b1306a08e950edddc152280a443796e68052c8e0454b5d76d69354d157",
     "Spyro: Year of the Dragon (NTSC-U, v1.0)"},
};

/* Unset: the pad-read site is not localised for this game yet, so the
 * pacing census reports the pad-read phase as unmeasured rather than as
 * zero reads; no WAD disc layout; no cheats, so --cheats is refused; no
 * sony_image_base, since recomp.conf names no g_SonyImage window and the
 * recompiled code never reads the base.
 *
 * load_delay_default: the measured floor for the checksum walk (the
 * level-load delay note in the file header).
 *
 * feed_key: every frame trigger a run is given (--frame-limit, the
 * screenshot and state pins) counts VBLs (openpete_vbl): the game waits with
 * VSync(n > 1) and its boot runs VBLs without a yield, so a yield count is
 * not a VBL count.
 *
 * consume_pc: func_8003A584, called by Update; it consumes the pad ring the VSync
 * callback fills once per VBL (count D_8006C7CC, clamped to 4). Its entries
 * key a consume-keyed bk2's rows.
 *
 * is_gameplay_live: a level overlay resident (loadStage -1) with game.state
 * in gameplay. Title, loading, fade-in and the exit transition report "not
 * live", which keeps the in-memory snapshot users (rewind keyframes,
 * runahead) out of them. The extra savestate payload is the shared thunk
 * layer's latch pack. */
const openpete_game_runtime_t spyro3_game_runtime = {
    .identity             = &spyro3_game_identity,
    .layout               = &spyro3_game_layout,
    .load_delay_default   = 66u,
    .loop_anchor_pc       = 0x80055400u,
    .known_discs          = k_known_discs,
    .known_disc_count     = (int)(sizeof k_known_discs / sizeof k_known_discs[0]),
    .consume_pc           = 0x8003A584u,
    .feed_key             = OPENPETE_FEED_KEY_VBL,
    .on_boot              = spyro3_on_boot,
    .on_frame_pre         = game_frame_pre_on_tick,
    .on_state_loaded      = spyro3_on_state_loaded,
    .state_extra_size     = psyq_thunk_state_size,
    .state_extra_save     = psyq_thunk_state_save,
    .state_extra_load     = psyq_thunk_state_extra_load,
    .is_gameplay_live     = spyro3_skip_gameplay_live,
    .set_skip_to_level    = spyro3_skip_set,
    .resolve_level_arg    = spyro3_level_resolve,
    .set_skip_to_cutscene = spyro3_set_skip_to_cutscene,
    .resolve_cutscene_arg = spyro3_cutscene_resolve,
    .set_skip_to_area     = spyro3_area_set,
    .resolve_area_arg     = spyro3_area_resolve,
    .surface              = spyro3_surface,
    .features             = &spyro3_feature_list,
    .host                 = &k_host,
    .psyq                 = &k_psyq,
    .override_dispatch    = spyro3_override_dispatch,
    .mod_desc             = spyro3_mod_desc,
};
