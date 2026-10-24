/* games/common/renderer/interp_regions_spyro23.c - the render-side
 * interpolation region table Spyro 2 and Spyro 3 share, captured per tick into
 * the openpete_interp ring so intermediate presents can lerp the camera
 * transform and the avatar.
 *
 * The rows are registered through the shared generator
 * (renderer/interp_regions_gen.h), which assigns each one its ring region
 * index; consumers resolve a row by name. Region 0 is the camera's draw-time
 * transform [cam.base, cam.xform_end): the sky matrix at +0x00, the world
 * matrix at +0x14 and the position at +0x28 (Spyro 2's span also holds the
 * Draw-time position copy at +0x30), all rewritten once per tick before the
 * draws read them, and on a game whose glow routine is located the row runs on
 * to the camera yaw halfword that routine reads (glow.cam_yaw_off). Every
 * address and length is the game's layout descriptor's.
 *
 * The per-object capture is the shared moby-array walk
 * (game_interp_capture_moby_array) plus the particle, gem-glint, glow, zap and
 * tracer pools (game_interp_capture_feeder_pools), whose records those
 * feeders' sub-tick replays rebase their captured primitives onto; all of them
 * are installed by the same registration call. The scrolling-texture
 * descriptors the same call captures are located by
 * game_layout.env_anim.scroll, so the UV-scroll mirror has its pairs.
 *
 * The game-state row is the simulation regime word, so a pair whose two ticks
 * came from different regimes is marked as a discontinuity and the camera cut
 * guard in renderer/reextract_spyro23.c holds instead of lerping across it. */
#include "renderer/interp_regions_gen.h"
#include "renderer/orchestrator.h"
#include "game_layout.h"
#include <stdint.h>

/** @brief Register the running game's interp region table (camera, player avatar, fire breath, game-state regime word) from its layout descriptor; Spyro 2's and Spyro 3's interp_register. */
void spyro23_interp_register(void) {
    const game_interp_row_t rows[] = {
        /* g_Spyro (the whole struct): the player avatar. It is not in the moby
         * array, so the shared moby-array capture never reaches it; position
         * +0x00, the body and head rotations +0x0C/+0x10 and the two animation
         * words +0x14/+0x18 are what the player draw reads per tick. The length
         * is the descriptor's, so the captured span and the record store's
         * per-call copy are the same bytes. The ground shadow's state block is
         * not a row: the player shadow hook copies it into the player record
         * store. */
        { "g_Spyro", game_layout.player.base, game_layout.player.rec_len, 0 },
        /* g_SpyroFlame: the fire-breath state block. Its origin and attach
         * matrix both advance once per tick, so without the pair the ribbon
         * holds its tick pose under an avatar that moves between ticks; the
         * per-strand keyframe counters in the same span are what the sub-tick
         * slide advances. */
        { "g_SpyroFlame", game_layout.player.flame_base, game_layout.player.flame_len, 0 },
        /* g_GameState (4 bytes): which simulation regime produced the tick. A
         * pair whose two ticks straddle one of its edges spans a level load or
         * a cutscene boundary rather than one motion. Committed timeline, like
         * the camera it gates: a led copy could flip a tick before the camera
         * pair it is compared against. */
        { "g_GameState", game_layout.game_state, 4u, 1 },
    };
    /* The camera row runs on to the yaw halfword the glow routine reads, so a
     * sub-tick present can blend the yaw that turns every ring. */
    uint32_t cam_len = game_layout.cam.xform_end - game_layout.cam.base;
    if (game_layout.glow.cam_yaw_off && game_layout.glow.cam_yaw_off + 2u > cam_len)
        cam_len = game_layout.glow.cam_yaw_off + 2u;
    game_interp_register_common(rows, (unsigned)(sizeof(rows) / sizeof(rows[0])),
                                cam_len, game_interp_capture_feeder_pools);
}
