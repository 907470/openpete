/* games/spyro-3/renderer/interp_regions.c - Spyro 3's render-side
 * interpolation region table, captured per tick into the openpete_interp ring
 * so intermediate presents can lerp the camera transform.
 *
 * The rows are registered through the shared generator
 * (renderer/interp_regions_gen.h), which assigns each one its ring region
 * index. Region 0 is the camera's draw-time transform [cam.base,
 * cam.xform_end): the sky matrix at +0x00, the world matrix at +0x14 and the
 * position at +0x28, all rewritten once per tick before the draws read them.
 *
 * The per-object capture is the shared moby-array walk
 * (game_interp_capture_moby_array) plus the particle, gem-glint, glow and zap
 * pools (game_interp_capture_feeder_pools), whose records those feeders'
 * sub-tick replays rebase their captured primitives onto; all of them are
 * installed by the same registration call. */
#include "renderer/interp_regions_gen.h"
#include "renderer/orchestrator.h"
#include "game_layout.h"
#include "spyro-3/gen/spyro3_symbols.gen.h"
#include "spyro-3/mod_sdk_gen/openpete_game_structs.h"   /* sizeof(Spyro) */
#include <stdint.h>

/* The plain-global rows: an address, a length and which timeline the region is
 * presented from. The camera's span is computed at registration below. */
static const game_interp_row_t s_rows[] = {
    /* g_Spyro (whole struct, 0x2E8): the player avatar. It is not in the moby
     * array, so the shared moby-array capture never reaches it; position +0x00,
     * the body and head rotations +0x0C/+0x10 and the two animation words
     * +0x14/+0x18 are what the player draw reads per tick. The ground
     * shadow's state block is not an interp row: the player shadow hook
     * copies it into the player record store. */
    { "g_Spyro", G_SPYRO, (uint32_t)sizeof(Spyro), 0 },
    /* game.state (4 bytes): which simulation regime produced the tick. A pair
     * whose two ticks came from different regimes is a discontinuity for the
     * presentation bracket, so the re-extract snaps instead of lerping across
     * the edge. Committed timeline, like the camera it gates: a led copy could
     * flip a tick before the camera pair it is compared against. */
    { "g_GameState", G_GAMESTATE, 4u, 1 },
};

/** @brief Register Spyro 3's interp region table and per-object capture with the interp ring; the descriptor's interp_register. */
void spyro3_interp_register(void) {
    /* g_SpyroFlame: the fire-breath state block, whose base and length come
     * from the layout descriptor rather than a symbol, so the row is built
     * here instead of in the static table. Its origin and attach matrix both
     * advance once per tick, so without the pair the ribbon holds its tick
     * pose under an avatar that moves between ticks; the per-strand keyframe
     * counters in the same span are what the sub-tick slide advances. */
    enum { NROWS = (unsigned)(sizeof(s_rows) / sizeof(s_rows[0])) + 1u };
    game_interp_row_t rows[NROWS];
    for (unsigned i = 0; i + 1u < (unsigned)NROWS; i++) rows[i] = s_rows[i];
    rows[NROWS - 1].name = "g_SpyroFlame";
    rows[NROWS - 1].addr = game_layout.player.flame_base;
    rows[NROWS - 1].length = game_layout.player.flame_len;
    rows[NROWS - 1].ra_commit = 0;

    /* The camera's span is the draw-time transform [cam.base, cam.xform_end),
     * computed from the layout descriptor rather than stated. */
    game_interp_register_common(rows, (unsigned)NROWS,
                                game_layout.cam.xform_end - game_layout.cam.base,
                                game_interp_capture_feeder_pools);
}
