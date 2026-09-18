/* ui_game.h — the per-game half of the shared 2D UI feeder.
 *
 * The decode in renderer/ui_sprite.c owns everything the games share: the
 * primitive classification, the FT4/F4/LINE_G2/SPRT word layouts, the
 * DR_MODE ABR tracking, the deferred feed-previous, the widescreen widening
 * of a full-width bar and the fade drive. Those layouts are the psyq packet
 * shapes, identical in both games.
 *
 * What differs is where the game keeps the layer: the address of the routine
 * every 2D primitive is linked through, the address of the full-screen fade
 * emitter, the signature of the freeze-frame backdrop quad, the VRAM block a
 * texture pack may replace by name, and the addresses of a text chain that
 * links itself and so never reaches the chokepoint. A zero address makes the
 * feeder register no hook for that path; a game with no 2D feeder links
 * games/common/renderer/layout_none.c. */
#ifndef OPENPETE_COMMON_RENDERER_UI_GAME_H
#define OPENPETE_COMMON_RENDERER_UI_GAME_H

#include <stdint.h>
#include "psx_runtime_internal.h"   /* CPUState */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_game {
    /* The 2D ordering-table insert (a0 = the primitive being linked). Every
     * 2D emitter calls it and no 3D draw does — the 3D passes inline the
     * link — so it is the 2D-vs-3D discriminator. */
    uint32_t ot_insert;

    /* The full-screen fade emitter, called as (abr_mode, r, g, b): one
     * DR_MODE carrying the ABR alone, then a full-screen semi-transparent
     * POLY_F4. The feeder drives the native fade from the arguments. */
    uint32_t fade_emitter;

    /* The freeze-frame backdrop quad's signature: its setRGB0 tint as the
     * primitive packs it (0x00BBGGRR) and the VRAM position of the grayscale
     * CLUT it samples through. A quad matching both is reproduced from the
     * native renderer's own last world frame instead of being fed, so the
     * frozen frame shows the native render rather than the emulated VRAM
     * scratch. `freeze_tint` 0 = the game has no such quad. */
    uint32_t freeze_tint;
    int      freeze_clut_x, freeze_clut_y;

    /* Set when the game produces its frozen backdrop by suppressing the
     * framebuffer clear instead of emitting a quad: the menu screens write
     * DRAWENV.isbg = 0 on both draw buffers and draw no world, so the
     * framebuffer keeps the last rendered frame. The native then drives the
     * freeze backdrop from the draw buffer's own isbg byte and draws its last
     * world frame unaltered. 0 = the game has no such screen. */
    int freeze_retained;

    /* The UI texture block, named once at the first 2D primitive so a
     * texture pack can replace the whole block by file name. `atlas_w` 0 =
     * the game names no block. */
    int         atlas_x, atlas_y, atlas_w, atlas_h;
    const char* atlas_name;

    /* Emitters that build a DR_MODE + SPRT chain in the primitive buffer and
     * link it themselves, bypassing `ot_insert`: the glyph emitters and the
     * loading-screen image, which draws the uploaded picture as four
     * quarter-width sprites. Each is hooked separately and its chain read out
     * of the primitive buffer it just filled. 0 = unused slot. */
    uint32_t sprt_chain_emitter[3];

    /* The overlay each emitter above lives in, 0 for one in the base binary.
     * An emitter inside the overlay window shares its address with a different
     * function in every other overlay of the game, so its hook carries the id
     * and fires only while that overlay is resident. */
    uint32_t sprt_chain_overlay[3];
} ui_game_t;

extern const ui_game_t ui_game;

/* Feed one 0x14-byte SPRT (a screen-space textured sprite: colour|code at
 * +0x04, x0/y0 at +0x08/+0x0A, u/v at +0x0C/+0x0D, CLUT at +0x0E, and
 * (height << 16) | width at +0x10) into the native HUD layer. `tpage` is the
 * GPU texture-page word in effect, which the SPRT packet does not carry — it
 * comes from the DR_MODE that heads the chain. */
void ui_sprite_feed_sprt(CPUState* cpu, uint32_t prim, unsigned tpage);

/* Feed one 0x28-byte POLY_FT4 (a screen-space textured quad: colour|code at
 * +0x04, then four xy/uv word pairs) into the native HUD layer. Unlike an
 * SPRT the packet carries its own texture page and CLUT, so no draw mode is
 * passed. */
void ui_sprite_feed_ft4(CPUState* cpu, uint32_t prim);

/* Primitives fed by ui_sprite_feed_sprt since boot: the engagement counter a
 * harness reads to tell "no text drew" from "text drew identically". */
unsigned ui_sprite_sprt_count(void);

/* Arm the native freeze backdrop for a `freeze_retained` game, from the isbg
 * byte of the draw buffer whose ordering table is about to be presented. Called
 * once per canonical present, next to the clear-colour commit that reads the
 * same DRAWENV. */
void ui_freeze_retain_commit(void);
/* Presents this run on which ui_freeze_retain_commit armed the backdrop: the
 * engagement counter a capture reads to tell "retention was off" from
 * "retention was on and changed nothing". */
unsigned ui_freeze_retain_count(void);
/* Primitives fed through the chokepoint since boot, by class. */
unsigned ui_sprite_prim_count(void);

/* Feed the 2D primitives claimed at the chokepoint into the native scene.
 * Called at the start of the ordering-table walk: a 2D primitive's words are
 * not final when it is linked (a sprite mirrors its own U coords right after
 * the insert returns; the atlas page turn skews a whole page of already-linked
 * quads a tick later), and they are final once the table is handed to the GPU.
 * The primitives linked between two walks are exactly one table's worth, so
 * this is also where the batch is complete. Inert when no game registers the
 * chokepoint. */
void ui_sprite_ot_draw_begin(void);

/* Re-read the batch already released for the ordering table being walked and
 * count the primitives whose words changed since: the release happens inside
 * the frame that built them, and the walk is where the words are beyond doubt
 * final, so a nonzero count means the release point is too early. */
void ui_sprite_ot_verify(void);

/* One of the two most recent ordering-table bases handed to DrawOTag (0 =
 * newest). The 2D feeder walks these to rank its primitives by the traversal
 * the GPU will perform, rather than by the order it happened to be fed in. */
uint32_t psyq_ot_recent_base(int i);

/* 2D primitives the chokepoint's decode chain claims no encoding for, since
 * boot: the engagement counter for adding a decoder. */
unsigned ui_sprite_unclaimed_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_UI_GAME_H */
