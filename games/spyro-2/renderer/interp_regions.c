/* games/spyro-2/renderer/interp_regions.c - Spyro 2's render-side
 * interpolation region table, captured per tick into the openpete_interp ring
 * so intermediate presents can lerp the camera transform and the avatar.
 *
 * The rows are registered through the shared generator
 * (renderer/interp_regions_gen.h), which assigns each one its ring region
 * index. Region 0 is the camera's draw-time transform [cam.base,
 * cam.xform_end): the sky matrix at +0x00, the world matrix at +0x14, the
 * position at +0x28 and the Draw-time position copy at +0x30, all rewritten
 * once per tick before the draws read them.
 *
 * The per-object capture is the shared moby-array walk
 * (game_interp_capture_moby_array) plus the particle, gem-glint, glow and zap
 * pools (game_interp_capture_feeder_pools), whose records those feeders'
 * sub-tick replays rebase their captured primitives onto; all of them are
 * installed by the same registration call.
 * The scrolling-texture descriptors the same call captures are located
 * (game_layout.env_anim.scroll), so the UV-scroll mirror has its pairs.
 *
 * The game-state row is the loader/simulation regime word, so a pair whose two
 * ticks came from different regimes is marked as a discontinuity and the camera
 * cut guard in renderer/reextract_spyro23.c holds instead of lerping across it. */
#include "renderer/interp_regions_gen.h"
#include "renderer/orchestrator.h"
#include "game_layout.h"
#include "spyro-2/gen/spyro2_symbols.gen.h"
#include <stddef.h>
#include <stdint.h>

/** @brief Register Spyro 2's interp region table (the player avatar, its fire breath and the game-state regime word) with the interp ring; the descriptor's interp_register. */
void spyro2_interp_register(void) {
    const game_interp_row_t rows[] = {
        /* g_Spyro (the whole struct): the player avatar. It is not in the moby
         * array, so the shared moby-array capture never reaches it; position
         * +0x00, the body and head rotations +0x0C/+0x10 and the two animation
         * words +0x14/+0x18 are what the player draw reads per tick. The length
         * is the descriptor's, so the captured span and the record store's
         * per-call copy are the same bytes. */
        { "g_Spyro", game_layout.player.base, game_layout.player.rec_len, 0 },
        /* g_SpyroFlame: the fire-breath state block. Its origin and attach
         * matrix both advance once per tick, so without the pair the ribbon
         * holds its tick pose under an avatar that moves between ticks; the
         * per-strand keyframe counters in the same span are what the sub-tick
         * slide advances. The length is the descriptor's, as for g_Spyro. */
        { "g_SpyroFlame", game_layout.player.flame_base, game_layout.player.flame_len, 0 },
        /* g_GameState (4 bytes): which simulation regime produced the tick. The
         * tick dispatcher branches on this word (update/func_8001A79C.s:26-31),
         * so a pair whose two ticks straddle one of its edges spans a level
         * load or a cutscene boundary rather than one motion. Committed
         * timeline, like the camera it gates: a led copy could flip a tick
         * before the camera pair it is compared against. */
        { "g_GameState", G_GAMESTATE, 4u, 1 },
    };
    /* The camera's span is the draw-time transform [cam.base, cam.xform_end),
     * computed from the layout descriptor rather than stated. */
    game_interp_register_common(rows, (unsigned)(sizeof(rows) / sizeof(rows[0])),
                                game_layout.cam.xform_end - game_layout.cam.base,
                                game_interp_capture_feeder_pools);
}
