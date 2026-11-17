// games/spyro-2/game_extras_impl.c, Spyro 2: Ripto's Rage!'s per-game contract.
//
// Defines the runtime descriptor spyro2_game_runtime (openpete_game_runtime.h)
// over the hooks below and the tables of the game's other TUs. Identity is
// generated from recomp.conf (game_identity.c). The PSYQ descriptor below
// carries the hand-written PSYQ addresses (rand seed slot, root counter 2),
// the sound tables and the stream-read classifiers. Bring-up
// state: the overlay hooks, --skip-to-level and the freecam wiring carry
// real values; the descriptor leaves the hooks the game does not implement
// NULL.

#include "openpete_disc_build.h"
#include "game_extras.h"
#include "game_host_desc.h"
#include "launcher_assets.h"
#include "renderer/portal_sky_s23.h"   /* portal_sky_s23_walk */
#include "game_mod_desc.h"
#include "openpete_modsave.h"
#include "openpete_launcher.h"
#include "psyq/psyq_thunk_state.h"
#include "openpete_fiber.h"
#include "openpete_psx.h"
#include "spyro_overlay_table.h"
#include "psyq/psyq_game_hooks.h"
#include "skip_to_level.h"
#include "features/levelwarp.h"
#include "features/cutscene_skip.h"
#include "dispatch_preamble.h"
#include "freecam_game.h"
#include "feature_registry.h"
#include "features/levelwarp_ui.h"
#include "features/level_skip.h"
#include "features/moby_update_all.h"
#include "renderer/text_moby.h"
#include "spyro-2/gen/spyro2_sfx_names.h" /* generated: the 256 SoundTable slot names */

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The boot and savestate-restore hooks are the shared bodies in
 * games/common/game_extras_overlay_resync.c. */
void game_overlay_resync_on_boot(CPUState* cpu);
void game_overlay_resync_on_state_loaded(CPUState* cpu);

/* The pre-tick sequence is the shared body in games/common/game_frame_pre.c:
 * the level skip (drives the title's demo-timeout transition toward the
 * target and holds the demo flag clear; inert without the flag), then the
 * dev-overlay level warp (drains a queued Jump into the game's own level
 * transition; inert until one is clicked). */
static const game_frame_pre_t k_frame_pre = {
    .skip_on_tick      = spyro2_skip_on_tick,
    .levelwarp_on_tick = spyro2_levelwarp_on_tick,
};

/** @brief Runs this game's intercepts at a function entry: the shared preamble, then the feature modules.
 *
 * The preamble (game_dispatch_preamble) runs the debug free camera (F),
 * which while engaged and gameplay is live drives the camera in place of
 * the camera follow (0x8001D87C) and suppresses it and the player update
 * (0x8002A07C), the write census's entry note, and the mod override
 * chains. An address none of them claims gets the OPENPETE_D1 gem mark
 * (moby_update_all_note_entry), then goes to the registered feature
 * modules (feature_list.c), which is how the shared presentation feeders
 * reach their hook addresses.
 * @param cpu The guest CPU state.
 * @param addr The guest address of the function being entered.
 * @return 1 when the recompiled body must return at once, else 0. */
static int spyro2_override_dispatch(CPUState* cpu, uint32_t addr) {
    if (game_dispatch_preamble(cpu, addr, NULL)) return 1;
    moby_update_all_note_entry(cpu, addr);
    return feature_registry_dispatch(cpu, addr);
}

/** @brief Report Spyro 2's feature rows and its level and cutscene vocabularies to the surface dump.
 *
 * @param sink The dump's collector. */
static void spyro2_surface(struct openpete_surface_sink* sink) {
    levelwarp_surface(sink, levelwarp_level_rows);
    game_level_surface(&spyro2_levels, sink);
    spyro2_cutscene_surface(sink);
}

/** @brief Arm a --skip-to-cutscene target through the shared level skip, riding a --skip-to-level to its source level.
 *
 * An exit row rides no --skip-to-level of its own (its source level is
 * reached by play or by an explicit one); a different explicit
 * --skip-to-level refuses the cutscene.
 * @param cutscene_id The cutscene id; 0 or less disables it. */
static void spyro2_set_skip_to_cutscene(int cutscene_id) {
    level_skip_arm_cutscene(&spyro2_level_skip, cutscene_id);
}
/* The level sound tables, loaded by func_8001D5FC (loaders.s): the
 * SoundTable pointer D_80067020 and the SoundDefinition pointer D_80067068.
 * ActiveSound[24] at D_80069A10, 0x2C bytes each, voice i at entry i
 * (func_80050FDC keys voice 1 << i from entry i); the pitch word at +0x18
 * (D_80069A28) is the voice's SpuVoiceAttr pitch. */
static const psyq_sfx_slots_t k_sfx_slots = {
    0x80067020u, 0x80067068u, 0x80069A10u, 0x2Cu, 0x18u,
    0u, /* the level bank uploads in chunks: every SpuWrite is a partial write */
    spyro2_sfx_slot_names,
};

/* Stream reads. The disc reader in str.c has two entries over the same
 * request words: func_80013918 issues a read (CdRead returns to 0x800139F0)
 * and returns, its callers polling the busy check func_80013690 from their
 * main loop; func_80013810 issues one and spins on func_80013690 before and
 * after it (calls returning to 0x80013848 and 0x800138F0) without calling
 * VSync. func_80013690 reports busy while D_800682E8 is set (the read
 * callback func_8001379C clears it) and, once clear, until CdSync(1)
 * reports the closing CdlPause complete. A cutscene plays with the top
 * state D_800681C8 at 6, and its loop func_titlescreen_and_loading_800742F8
 * streams through func_80013918 (0x800748D4). */
/** @brief Classify a read as a cutscene stream read: the asynchronous reader's CdRead while the top state is cutscene playback (6).
 *
 * @param cpu Guest CPU state at the CdRead call (ra is the call site).
 * @return 1 for a stream read, else 0. */
static int spyro2_cd_read_streamed(CPUState* cpu) {
    return cpu->ra == 0x800139F0u && cpu->read_word(0x800681C8u) == 6u;
}

/* The tables of the game's other TUs the descriptor carries. */
extern const openpete_game_identity_t spyro2_game_identity;   /* gen/game_identity.c */
extern const game_layout_t            spyro2_game_layout;     /* renderer/layout.c */
extern const ui_game_t                spyro2_ui_game;         /* renderer/layout.c */
extern const terrain_game_t           spyro2_terrain_game;    /* renderer/terrain_game.c */
extern const moby_game_s23_consts_t   spyro2_moby_game_s23;   /* renderer/moby_game.c */
extern const player_game_s23_consts_t spyro2_player_game_s23; /* renderer/player_game.c */
extern const portal_game_s23_consts_t spyro2_portal_game_s23; /* renderer/portal_game.c */
extern const freecam_game_desc        spyro2_freecam_desc;    /* features/freecam.c */
extern const game_orchestrator_t      spyro23_orchestrator;   /* common renderer/reextract_spyro23.c */
extern const openpete_feature_list_t  spyro2_feature_list;    /* feature_list.c */
void spyro23_interp_register(void);                            /* common renderer/interp_regions_spyro23.c */
extern const psyq_game_conf_t         spyro2_psyq_conf;       /* gen/game_identity.c */
extern const psyq_stub_table_t        spyro2_psyq_stub_table; /* gen/psyq_stub_table.c */

/* The shared PSYQ thunk layer's hooks. */
static const psyq_game_hooks_t k_psyq = {
    .conf             = &spyro2_psyq_conf,
    .stubs            = &spyro2_psyq_stub_table,
    /* D_8006D144 (psyq .bss): rand (func_80059D2C) steps it, srand
     * (func_80059D5C) overwrites it. */
    .rand_seed_addr   = 0x8006D144u,
    /* The reference boot's `lhu` at 0x8001C2DC (func_8001C2D8, stdutil.s)
     * read this; it seeds srand. */
    .root_counter2    = 0x4876u,
    .cd_read_streamed = spyro2_cd_read_streamed,
    .sfx_slots        = &k_sfx_slots,
};

GAME_MOD_TABLES_DECLARE(spyro2);
const openpete_moby_ops_t* spyro2_moby_ops(void);

extern const openpete_save_desc_t spyro2_save_desc; /* mods/mod_save_desc.c */

/** @brief Return Spyro 2's mod-layer descriptor: its generated SDK symbol table and overlay entry arrays, and its moby ops.
 *
 * Built on the first call into static storage. Overlay-window overrides
 * bind through override_ovl or a name scoped to one overlay only
 * (legacy_window_links 0). The moby ops are mods/mod_moby_ops.c's, every
 * render channel is a material channel, and mod text draws through the
 * text-moby rasterizer. Level ops, the sfx canonicaliser and the
 * render-function list stay NULL, so the loader treats those surfaces as
 * unsupported on this game; the arena keeps the core default base.
 * @return The descriptor, never NULL. */
static const game_mod_desc_t* spyro2_mod_desc(void) {
    static game_mod_desc_t s_desc;
    static int             s_built = 0;
    if (!s_built) {
        GAME_MOD_TABLES_FILL(&s_desc, spyro2);
        s_desc.moby_ops     = spyro2_moby_ops();
        s_desc.channel_mask = OPENPETE_MODS_CHANNELS_ALL;
        s_desc.text_emit    = text_moby_native_mod_emit;
        s_desc.save_desc    = &spyro2_save_desc;
        s_built = 1;
    }
    return &s_desc;
}

/* The known disc's asset input hashes (FNV-1a 64): each level's sky
 * payload in level-table order, the glyph font bytes, and the logo's
 * texel rows followed by its palette. */
static const uint64_t k_sky_hashes[] = {
    0x975649137a167798u, /* summer_forest */
    0x2240b3412e7a4138u, /* glimmer */
    0xb142439202182194u, /* idol_springs */
    0x51f926ca47ff8661u, /* colossus */
    0x491db8fafafcef47u, /* hurricos */
    0x86446ca1f48a7751u, /* aquaria_towers */
    0x57095ee4ae87224au, /* sunny_beach */
    0x1b6b14eee5b80550u, /* ocean_speedway */
    0x031290db065e2b7au, /* crushs_dungeon */
    0x3612935481e6e12du, /* autumn_plains */
    0x344d171043b88457u, /* skelos_badlands */
    0xd3e349b91e05d631u, /* crystal_glacier */
    0x740e6ce4c6d0fc2du, /* breeze_harbor */
    0x948c372c95c9a351u, /* zephyr */
    0xebf6acd9e60bdd7bu, /* metro_speedway */
    0x5dc3fd50145a1d63u, /* scorch */
    0xb5c092a6c8068b13u, /* shady_oasis */
    0x4f28d639bd749a0du, /* magma_cone */
    0xbe564f9572a78347u, /* fracture_hills */
    0xc439096e8626530au, /* icy_speedway */
    0xc00ec2c3d97266adu, /* gulps_overlook */
    0xee3ed563c23d529du, /* winter_tundra */
    0x1434b38d8ee9a690u, /* mystic_marsh */
    0x81a7eb16bd00f0f8u, /* cloud_temples */
    0x9c5ff6d7c6585fdeu, /* canyon_speedway */
    0x948c372c95c9a351u, /* robotica_farms */
    0x08f1a849a6820e59u, /* metropolis */
    0xbe5676ff368d5fe7u, /* dragon_shores */
    0xee3ed563c23d529du, /* riptos_arena */
};
static const game_asset_hashes_t k_asset_hashes[] = {
    {"b3fd9fdb24b115fee4583dbe3c36ab788fd8679fb4592dbec8736392dec64551",
     k_sky_hashes, (int)(sizeof k_sky_hashes / sizeof k_sky_hashes[0]),
     0x041727bb43eeda96u, 0xa98faa35bd859ac0u},
};
static const game_assets_desc_t k_assets = {
    .levels         = &spyro2_levels,
    .sky_form       = GAME_SKY_SLOT_CHAIN,
    .sky_walk       = portal_sky_s23_walk,
    /* WAD entry 95 sub-file 0: VRAM (512,0)-(1023,511); texels at
     * (512,256), palette at (512,511), drawn by a SPRT 256x96. */
    .logo           = { 95, 0, 262144u, 523264u, 256u, 96u },
    .discs          = k_asset_hashes,
    .disc_count     = (int)(sizeof k_asset_hashes / sizeof k_asset_hashes[0]),
};

static const game_host_desc_t k_host = {
    .ui              = &spyro2_ui_game,
    .terrain         = &spyro2_terrain_game,
    .moby            = &moby_game_spyro23,
    .player          = &player_game_spyro23,
    .moby_s23        = &spyro2_moby_game_s23,
    .player_s23      = &spyro2_player_game_s23,
    .portal_s23      = &spyro2_portal_game_s23,
    .freecam         = &spyro2_freecam_desc,
    .orchestrator    = &spyro23_orchestrator,
    .interp_register = spyro23_interp_register,
    .frame_pre       = &k_frame_pre,
    .assets          = &k_assets,
};

/* The builds a disc of this game can hold (disc_builds.c). */
extern const openpete_disc_build_row_t spyro2_disc_builds[];

/* The disc image the User docs pin, labelled by name in the library. */
static const openpete_launcher_known_disc k_known_discs[] = {
    {"b3fd9fdb24b115fee4583dbe3c36ab788fd8679fb4592dbec8736392dec64551",
     "Spyro 2: Ripto's Rage! (NTSC-U)"},
};

/* The bug-report replay site: the main loop's one VSync wait, every state's. */
static const openpete_park_site_t k_replay_sites[] = {
    { 0x80015944u, 0x801FFFC8u },
};

/* Unset: the pad-read site is not localised for this game yet, so the
 * pacing census reports the pad-read phase as unmeasured rather than as
 * zero reads; no load-read classifier, so loads are instant; no WAD disc
 * layout; no cheats and no sub-area targets, so --cheats and
 * --skip-to-area are refused; no sony_image_base, since recomp.conf names no
 * g_SonyImage window and the recompiled code never reads the base.
 *
 * feed_key: every frame trigger a run is given (--frame-limit, the
 * screenshot and state pins) counts VBLs (openpete_vbl): the game waits with
 * VSync(n > 1) and its boot runs VBLs without a yield, so a yield count is
 * not a VBL count.
 *
 * consume_pc: func_8001271C, called by func_8001B140 (the update), the guidebook and the cheat loop; it consumes the pad ring the VSync
 * callback fills once per VBL (count D_80067160, clamped to 4). Its entries
 * key a consume-keyed bk2's rows.
 *
 * is_gameplay_live: a level overlay resident with its state machine running
 * (loader state -1). Title, loading and the exit transition report "not
 * live", which keeps the in-memory snapshot users (rewind keyframes,
 * runahead) out of them. The extra savestate payload is the shared thunk
 * layer's latch pack. */
const openpete_game_runtime_t spyro2_game_runtime = {
    .identity             = &spyro2_game_identity,
    .layout               = &spyro2_game_layout,
    .loop_anchor_pc       = 0x8001B140u,
    .known_discs          = k_known_discs,
    .known_disc_count     = (int)(sizeof k_known_discs / sizeof k_known_discs[0]),
    .disc_builds          = spyro2_disc_builds,
    .consume_pc           = 0x8001271Cu,
    .replay_sites         = k_replay_sites,
    .replay_site_count    = (int)(sizeof k_replay_sites / sizeof k_replay_sites[0]),
    .feed_key             = OPENPETE_FEED_KEY_VBL,
    .on_boot              = game_overlay_resync_on_boot,
    .on_frame_pre         = game_frame_pre_on_tick,
    .on_state_loaded      = game_overlay_resync_on_state_loaded,
    .state_extra_size     = psyq_thunk_state_size,
    .state_extra_save     = psyq_thunk_state_save,
    .state_extra_load     = psyq_thunk_state_extra_load,
    .is_gameplay_live     = spyro2_skip_gameplay_live,
    .set_skip_to_level    = spyro2_skip_set,
    .resolve_level_arg    = spyro2_level_resolve,
    .set_skip_to_cutscene = spyro2_set_skip_to_cutscene,
    .resolve_cutscene_arg = spyro2_cutscene_resolve,
    .surface              = spyro2_surface,
    .features             = &spyro2_feature_list,
    .host                 = &k_host,
    .psyq                 = &k_psyq,
    .override_dispatch    = spyro2_override_dispatch,
    .mod_desc             = spyro2_mod_desc,
};
