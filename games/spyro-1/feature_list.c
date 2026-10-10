// games/spyro-1/feature_list.c - Spyro 1's registered feature modules.

#include "openpete_game_runtime.h"

#include <stddef.h>

extern const feature_module_t render_paths_module;
extern const feature_module_t cyclorama_module;
extern const feature_module_t moby_module;
extern const feature_module_t player_module;
extern const feature_module_t flame_module;
extern const feature_module_t shadow_module;
extern const feature_module_t shadow_spyro_module;
extern const feature_module_t particle_module;
extern const feature_module_t tracer_module;
extern const feature_module_t terrain_module;
extern const feature_module_t ui_sprite_module;
extern const feature_module_t portal_module;
extern const feature_module_t transition_module;
extern const feature_module_t cheats_module;
extern const feature_module_t credits_skip_module;
extern const feature_module_t spyro1_levelwarp_module;
extern const feature_module_t text_moby_id_module;
extern const feature_module_t crash_demo_woah_module;
extern const feature_module_t polybuf_guard_module;

static const feature_module_t* const k_modules[] = {
    /* --render-paths: parse the path-isolation selection at startup (apply_options
     * only; the per-path polybuf brackets live in the native feature modules and
     * the OT-walk filter in psyq_DrawOTag). */
    &render_paths_module,
    /* Native-renderer cyclorama feeder + A/B verify harness.
     * Hooks 0x8004EBA8 only when OPENPETE_CYC_VERIFY is set; otherwise an
     * inert fall-through. */
    &cyclorama_module,
    /* Native-renderer moby feeder + A/B verify harness. Hooks the
     * REGULAR material renderer 0x8001F798 only when OPENPETE_MOBY_VERIFY or
     * a native renderer mode (--renderer vk) is set; inert otherwise. */
    &moby_module,
    /* Native-renderer Spyro feeder + A/B verify harness. Hooks the
     * player-avatar renderer 0x80023AC4 only when OPENPETE_SPYRO_VERIFY or
     * a native renderer mode (--renderer vk) is set; inert otherwise. */
    &player_module,
    /* Native-renderer flame feeder + A/B harness, an ADDITIVE
     * (ABR mode 1) consumer. Hooks Spyro's flame renderer 0x80058D64 only when
     * OPENPETE_FLAME_VERIFY or a native renderer mode (--renderer vk) is
     * set; inert otherwise. Records the RP_FLAME polybuf range itself (so the
     * render_paths thin bracket for flame is retired - see render_paths.c). */
    &flame_module,
    /* Native-renderer SHADOWS: moby shadows (0x80059F8C, textured
     * FT3 fan, ABR mode 0) + Spyro silhouette shadow (0x80059A48, untextured
     * G3 fan, ABR mode 0, an untextured-alpha consumer). Hooks both
     * only under OPENPETE_SHADOW_VERIFY or a native renderer mode; inert
     * otherwise. Records the RP_MOBY_SHADOW and RP_SHADOW polybuf ranges
     * itself (so the
     * render_paths thin brackets for shadow are retired - see render_paths.c). */
    &shadow_module,
    &shadow_spyro_module,
    /* Native-renderer PARTICLES: the particle family - particles
     * (0x800573C8, 7 sprite classes), glows (0x800580F4, additive rings) and
     * sparkles (0x800584C4, twinkle quads). VERDICT-CRITICAL: the .s runs
     * headless (writes m_03 + Sparkle m_Life) and we only mirror geometry.
     * Hooks all three only under OPENPETE_PARTICLE_VERIFY or a native renderer
     * mode; inert otherwise. Records the RP_PARTICLES polybuf range itself (so
     * the render_paths thin bracket on func_800573C8 is retired). */
    &particle_module,
    /* Native-renderer TRACER/beam: the additive gouraud quad-strip
     * tracer (func_800189F0 -> func_800189F8), e.g. the dream-weavers fireball's
     * glowing streak. Capture-and-feed: runs the real renderer (verdict) then
     * feeds the additive strip it emitted into the native scene's additive
     * layer. */
    &tracer_module,
    /* Native-renderer TERRAIN/world: hooks the env-render dispatch
     * func_8002B9CC (-> func_800258F0 world renderer) only under
     * OPENPETE_TERRAIN_VERIFY or a native renderer mode; inert otherwise.
     * Records the RP_TERRAIN polybuf range itself (so the render_paths thin
     * bracket on func_8002B9CC is retired - see render_paths.c). */
    &terrain_module,
    /* 2D-from-OT native UI: hooks func_800168DC (the 2D addPrim chokepoint) to
     * feed the title logo / "PRESS START" / HUD text sprites into the native
     * scene's HUD overlay layer. Inert unless a native renderer mode is
     * active. */
    &ui_sprite_module,
    /* Native PORTAL skybox previews: re-derives the destination-level dome
     * shown inside homeworld portal arches (driven by RenderPortals
     * func_80050BD0 -> func_8004FEA0/80050240/8004F4BC, into a per-portal clip
     * rect). The main-sky feeder (cyclorama_native) only hooks func_8004EBA8,
     * so portal previews were missing. Capture/dump harness under
     * OPENPETE_PORTAL_DUMP now; float extraction + clip + feed are the next
     * increment. Inert unless a native renderer mode or the dump is set. */
    &portal_module,
    /* Native feeder for the level-transition / balloonist cross-fade backdrop
     * (func_8004F000): captures the screen-space gouraud mesh it emits into the
     * shared polybuf and feeds it as the backmost layer, so the soft gradient
     * shows during the balloonist screen / level exit instead of a flat clear.
     * Also keeps a thin red probe for the still-unfed GS_ExitLevel grayscale
     * screen (func_8001C694), opt-in via OPENPETE_TRANSITION_PROBE=1. Chains a
     * present-commit (after portal's) for the sub-tick staleness latch. */
    &transition_module,
    /* --cheats: parse the selection at startup (apply_options only; the
     * effects are written from game_on_frame_pre once a level is live -
     * see features/cheats.c). Inert without the flag. */
    &cheats_module,
    /* --skip-to-cutscene: intro (New Game's branch from a GamestateDraw
     * entry hook at the Press-Start wait), ending / ending_loot (the boss
     * branch's GS_Cutscene stores from the same entry on the boss level's
     * first live tick), credits / credits_100 (the "CREDITS" pad code's
     * InitCreditsSequence from a GamestateUpdate entry hook). Inert unless
     * armed by game_set_skip_to_cutscene. */
    &credits_skip_module,
    /* Dev-overlay level warp (M menu): a level dropdown + Warp button that
     * rides the game's own exit-level -> load sequence to any level, plus
     * the shared level id/name table --skip-to-level resolves against.
     * Writes nothing until the button is clicked. */
    &spyro1_levelwarp_module,
    /* Records the emitter-call provenance of every HUD text glyph
     * (func_80017FE4 / func_800181AC entry hooks, read-only, always fall
     * through) so the sub-tick interpolator can tell one tick's letters from
     * the next's. See renderer/text_moby_id.c. */
    &text_moby_id_module,
    /* L1+Triangle on the title screen: the original boots the disc's Crash 3
     * demo via LoadExec, which openpete cannot host. Swallows CrashDemoLaunch
     * before its engine teardown and plays Crash's "woah!" (extracted from
     * the player's own disc image on first use) as an easter egg. */
    &crash_demo_woah_module,
    /* Read-only detector for the unchecked polygon-buffer overflow: samples
     * the prim cursor (D_800757B0) against the buffer end (D_800756FC) once
     * per frame at BuildOrderTable, when the cursor is at its frame peak.
     * Overrunning the buffer walks primitives into g_WorldOT, which sits
     * immediately after it, and the OT tail-insert then dereferences primitive
     * bytes as a node pointer - arbitrary 3-byte writes. Always falls through;
     * Off by default; OPENPETE_POLYBUF_GUARD=1 enables it, =verbose also logs
     * per-frame headroom. */
    &polybuf_guard_module,
};

/* The list spyro1_game_runtime carries (game_extras_impl.c). */
const openpete_feature_list_t spyro1_feature_list = {
    k_modules, sizeof(k_modules) / sizeof(k_modules[0])
};
