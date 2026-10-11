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

#include "game_extras.h"
#include "game_host_desc.h"
#include "game_mod_desc.h"
#include "psyq/psyq_thunk_state.h"
#include "openpete_psx.h"
#include "spyro_overlay_table.h"
#include "psyq/psyq_game_hooks.h"
#include "skip_to_level.h"
#include "features/levelwarp.h"
#include "features/cutscene_skip.h"
#include "write_census.h"
#include "dispatch_preamble.h"
#include "freecam_game.h"
#include "feature_registry.h"
#include "features/levelwarp_ui.h"
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

/** @brief Run Spyro 2's per-tick host hooks before the game fiber runs a tick: the level skip and warp, the scripted free camera, the write census phase and the host text layers.
 *
 * @param cpu Guest CPU state.
 * @param frame_counter The openpete_frame counter before the tick. */
static void spyro2_on_frame_pre(CPUState* cpu, uint32_t frame_counter) {
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

/** @brief Runs this game's intercepts at a function entry: the shared preamble, then the feature modules.
 *
 * The preamble (game_dispatch_preamble) runs the debug free camera (F),
 * which while engaged and gameplay is live drives the camera in place of
 * the camera follow (0x8001D87C) and suppresses it and the player update
 * (0x8002A07C), the write census's entry note, and the mod override
 * chains. An address none of them claims goes to the registered feature
 * modules (feature_list.c), which is how the shared presentation feeders
 * reach their hook addresses.
 * @param cpu The guest CPU state.
 * @param addr The guest address of the function being entered.
 * @return 1 when the recompiled body must return at once, else 0. */
static int spyro2_override_dispatch(CPUState* cpu, uint32_t addr) {
    if (game_dispatch_preamble(cpu, addr, NULL)) return 1;
    return feature_registry_dispatch(cpu, addr);
}

/** @brief Report Spyro 2's feature rows and its level and cutscene vocabularies to the surface dump.
 *
 * @param sink The dump's collector. */
static void spyro2_surface(struct openpete_surface_sink* sink) {
    levelwarp_ui_surface(sink);
    spyro2_level_surface(sink);
    spyro2_cutscene_surface(sink);
}

/** @brief Arm a --skip-to-cutscene target, riding a --skip-to-level to its source level; a different explicit --skip-to-level refuses it.
 *
 * @param cutscene_id The cutscene id; 0 or less disables it. */
static void spyro2_set_skip_to_cutscene(int cutscene_id) {
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
/** @brief Whether the entry is inside func_80013690 called from func_80013810's spins.
 *
 * The recompiled body enters through more than one block (0x80013690,
 * 0x80013698), so any address of the function counts.
 * @param cpu Guest CPU state at the entry (ra is the caller's return address).
 * @param addr Guest address of the function being entered.
 * @return 1 for the spin's poll, else 0. */
static int spyro2_cd_spin_wait(CPUState* cpu, uint32_t addr) {
    return addr >= 0x80013690u && addr < 0x8001379Cu && (cpu->ra == 0x80013848u || cpu->ra == 0x800138F0u);
}

/* The tables of the game's other TUs the descriptor carries. */
extern const openpete_game_identity_t spyro2_game_identity;   /* gen/game_identity.c */
extern const openpete_game_icon_t     spyro2_game_icon;       /* gen/openpete_spyro2_icon.c */
extern const game_layout_t            spyro2_game_layout;     /* renderer/layout.c */
extern const ui_game_t                spyro2_ui_game;         /* renderer/layout.c */
extern const terrain_game_t           spyro2_terrain_game;    /* renderer/terrain_game.c */
extern const moby_game_s23_consts_t   spyro2_moby_game_s23;   /* renderer/moby_game.c */
extern const player_game_s23_consts_t spyro2_player_game_s23; /* renderer/player_game.c */
extern const portal_game_s23_consts_t spyro2_portal_game_s23; /* renderer/portal_game.c */
extern const freecam_game_desc        spyro2_freecam_desc;    /* features/freecam.c */
extern const game_orchestrator_t      spyro2_orchestrator;    /* renderer/reextract.c */
extern const openpete_feature_list_t  spyro2_feature_list;    /* feature_list.c */
void spyro2_interp_register(void);                             /* renderer/interp_regions.c */
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
    .cd_spin_wait     = spyro2_cd_spin_wait,
    .sfx_slots        = &k_sfx_slots,
};

GAME_MOD_TABLES_DECLARE(spyro2);

/** @brief Return Spyro 2's mod-layer descriptor: its generated SDK symbol table and overlay entry arrays, and no game-specific surface.
 *
 * Built on the first call into static storage. Overlay-window overrides
 * bind through override_ovl or a name scoped to one overlay only
 * (legacy_window_links 0). Moby ops, level ops, the text rasterizer, the sfx
 * canonicaliser and the render-function list stay NULL and the material
 * channel mask stays 0, so the loader treats those surfaces as unsupported
 * on this game; the arena keeps the core default base.
 * @return The descriptor, never NULL. */
static const game_mod_desc_t* spyro2_mod_desc(void) {
    static game_mod_desc_t s_desc;
    static int             s_built = 0;
    if (!s_built) {
        GAME_MOD_TABLES_FILL(&s_desc, spyro2);
        s_built = 1;
    }
    return &s_desc;
}

static const game_host_desc_t k_host = {
    .ui              = &spyro2_ui_game,
    .terrain         = &spyro2_terrain_game,
    .moby            = &moby_game_spyro23,
    .player          = &player_game_spyro23,
    .moby_s23        = &spyro2_moby_game_s23,
    .player_s23      = &spyro2_player_game_s23,
    .portal_s23      = &spyro2_portal_game_s23,
    .freecam         = &spyro2_freecam_desc,
    .orchestrator    = &spyro2_orchestrator,
    .interp_register = spyro2_interp_register,
};

/* Unset: the pad-read site is not localised for this game yet, so the
 * pacing census reports the pad-read phase as unmeasured rather than as
 * zero reads; no load-read classifier, so loads are instant; no WAD disc
 * layout; no cheats and no sub-area targets, so --cheats and
 * --skip-to-area are refused.
 *
 * sony_image_base: the recompiler carries a compiled-in substitution for a
 * `lui 0x8007` + `addiu` pair composing into [0x8006FCF4, 0x80072E94),
 * rewriting it to `psx_runtime_g_sony_image_base + offset`. That window is
 * Spyro 1's g_SonyImage scratch pool, but the substitution is not
 * data-driven, so it fires on any game whose code composes an address in
 * that span. Holding the base at the canonical value makes the emitted
 * expression reduce to the original address, so the rewrite is identity
 * here. Making the window per-game belongs to the recompiler, not to this
 * file.
 *
 * is_gameplay_live: a level overlay resident with its state machine running
 * (loader state -1). Title, loading and the exit transition report "not
 * live", which keeps the in-memory snapshot users (rewind keyframes,
 * runahead) out of them. The extra savestate payload is the shared thunk
 * layer's latch pack. */
const openpete_game_runtime_t spyro2_game_runtime = {
    .identity             = &spyro2_game_identity,
    .icon                 = &spyro2_game_icon,
    .layout               = &spyro2_game_layout,
    .sony_image_base      = 0x8006FCF4u,
    .on_boot              = game_overlay_resync_on_boot,
    .on_frame_pre         = spyro2_on_frame_pre,
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
