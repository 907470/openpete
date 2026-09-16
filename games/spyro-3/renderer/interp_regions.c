/* games/spyro-3/renderer/interp_regions.c — Spyro 3's render-side
 * interpolation region table, captured per tick into the openpete_interp ring
 * so intermediate presents can lerp the camera transform.
 *
 * The rows are registered through the shared generator
 * (renderer/interp_regions_gen.h), which assigns each one its ring region
 * index. Region 0 is the camera's draw-time transform [cam.base,
 * cam.xform_end): the sky matrix at +0x00, the world matrix at +0x14 and the
 * position at +0x28, all rewritten once per tick before the draws read them.
 *
 * The per-object capture walks the moby array and snapshots one whole record
 * per live moby, keyed by its address: the array is fixed and terminated by a
 * live sentinel, so a moby's address IS its slot and stays the same identity
 * across ticks. The sub-tick moby walk pairs its records against these. */
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "renderer/interp_regions_gen.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "renderer/orchestrator.h"
#include "game_layout.h"
#include "spyro3_symbols.gen.h"
#include "openpete_game_structs.h"   /* sizeof(Spyro) */
#include <stdint.h>
#include <string.h>

/* The plain-global rows: an address, a length and which timeline the region is
 * presented from. The camera's span is computed at registration below. */
static const game_interp_row_t s_rows[] = {
    /* g_Spyro (whole struct, 0x2E8): the player avatar. It is not in the moby
     * array, so the per-object capture below never reaches it; position +0x00,
     * the body and head rotations +0x0C/+0x10 and the two animation words
     * +0x14/+0x18 are what the player draw reads per tick. The drop-shadow
     * scratch Spyro 1 captures separately has no counterpart here — the shadow
     * derives from this record. */
    { "g_Spyro", G_SPYRO, (uint32_t)sizeof(Spyro), 0 },
    /* game.state (4 bytes): which simulation regime produced the tick. A pair
     * whose two ticks came from different regimes is a discontinuity for the
     * presentation bracket, so the re-extract snaps instead of lerping across
     * the edge. Committed timeline, like the camera it gates: a led copy could
     * flip a tick before the camera pair it is compared against. */
    { "g_GameState", G_GAMESTATE, 4u, 1 },
};

static void capture_objs(void) {
    openpete_interp_objs_begin_tick();
    const uint32_t head = game_layout.moby.array_head;
    uint32_t arr = 0;
    if (head) {
        if (game_layout.moby.array_direct) arr = head;
        else {
            const uint8_t* h = (const uint8_t*)openpete_psx_to_host_ptr(head);
            if (h) memcpy(&arr, h, 4);
        }
    }
    if (arr >= 0x80000000u) {
        const uint32_t stride = game_layout.moby.array_stride;
        const uint32_t blob   = openpete_interp_objs_blob_bytes();
        uint32_t m = arr;
        /* Bound the walk below the store capacity so a full array can never
         * overflow it; the sentinel stops it after the live prefix anyway. */
        for (uint32_t guard = 0; guard < openpete_interp_objs_slots() - 512u; ++guard) {
            const void* host = openpete_psx_to_host_ptr(m);
            if (!host) break;
            uint8_t st = ((const uint8_t*)host)[game_layout.moby.state_off];
            if (st == (uint8_t)game_layout.moby.state_term) break;   /* end of the array */
            if (st < 0x80) openpete_interp_objs_capture(m, host, blob);  /* the rest are dormant */
            m += stride;
        }
    }
    openpete_interp_objs_end_tick();
}

void game_interp_register(void) {
    /* The camera first, so it is region 0. Its span is the draw-time transform
     * [cam.base, cam.xform_end), computed from the layout descriptor rather
     * than stated. ra_commit = 1: the camera presents from the committed
     * timeline under runahead, so a misprediction never shows as a pan that
     * un-shows. */
    game_interp_rows_add_one("camera", game_layout.cam.base,
                             game_layout.cam.xform_end - game_layout.cam.base, 1);
    game_interp_rows_add(s_rows, (unsigned)(sizeof(s_rows) / sizeof(s_rows[0])));
    game_interp_rows_commit();
    /* One whole Moby record per object, so every consumer reads its fields at
     * their natural offsets, and the store's default capacity. */
    game_interp_rows_obj_capture(capture_objs,
                                 game_layout.moby.array_stride,
                                 OPENPETE_INTERP_OBJ_SLOTS);
}
