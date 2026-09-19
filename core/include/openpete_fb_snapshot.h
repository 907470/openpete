/* openpete_fb_snapshot.h — framebuffer-window snapshots.
 *
 * A game screen can copy a rect of its draw buffer to a VRAM scratch page with
 * MoveImage and then re-display that copy every frame as a textured quad — a
 * window onto the frame as it stood when the copy was taken. The quad's texels
 * come from the emulated framebuffer, which the native renderer never writes:
 * under a native-only run nothing rasterizes into it, the copy is zeros, and a
 * 16-bit texel of zero is transparent even on an opaque primitive, so the
 * window contributes no pixels at all.
 *
 * This store closes that channel. The move records its destination rect; at the
 * next present the native renderer crops its own composed frame to the source
 * rect and stores it here as RGBA8 at native resolution. A fed textured quad
 * whose texture page and texel box land inside a stored rect is redirected to
 * the snapshot instead of the VRAM sampler. A quad that finds no stored
 * snapshot keeps the stock VRAM path.
 */
#ifndef OPENPETE_FB_SNAPSHOT_H
#define OPENPETE_FB_SNAPSHOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Material bpp sentinel for a scene triangle redirected to a snapshot. The
 * material fields are otherwise unused on such a triangle: tpage_x carries the
 * slot and the vertex u,v are texel coordinates in the snapshot image. A
 * sentinel rather than a reserved id keeps the snapshot slots out of the mod
 * texture-id namespace, and keeps the run-coalescing material compare from
 * merging a snapshot run with a VRAM or mod run. */
#define OPENPETE_FB_SNAPSHOT_BPP 33

/* Record a draw area the game put: the DRAWENV clip rect and drawing offset, in
 * VRAM texels. A primitive at screen (x, y) lands at VRAM (x + ofs_x, y +
 * ofs_y), so a copy out of VRAM (sx, sy) reads the pixels drawn at screen (sx -
 * ofs_x, sy - ofs_y); the area holding a copy's source supplies that offset. A
 * double-buffered game puts one area per draw buffer, and each distinct clip
 * rect keeps its latest offset. `isbg` is the DRAWENV's auto-clear: with it set
 * the area's next frame starts from the clear colour, so the native frame that
 * stood for the area's content is dropped; with it clear the game draws over
 * what the buffer already holds and the image is kept. */
void openpete_fb_snapshot_note_draw_env(int clip_x, int clip_y, int clip_w, int clip_h,
                                        int ofs_x, int ofs_y, int isbg);

/* Record a MoveImage. Two idioms are recognised, both out of a draw area:
 *
 *   draw area -> scratch page (destination beyond the display buffers): a
 *   framebuffer window. Returns 1 and a snapshot is now pending, unless the
 *   source area carries a content image, which is cropped at once.
 *
 *   draw area -> draw area covering the destination area whole: the game moving
 *   a finished frame into the buffer it is not drawing. The destination area
 *   takes the source area's content image, or waits for the next present when
 *   the source is the buffer being drawn. Returns 0.
 *
 * Anything else returns 0 and records nothing. Coordinates are VRAM texels,
 * post-clamp. */
int openpete_fb_snapshot_on_move(int src_x, int src_y, int w, int h,
                                 int dst_x, int dst_y);

/* 1 while any window slot or draw area is waiting for its pixels. */
int openpete_fb_snapshot_pending(void);

/* Drain one pending window slot: `idx` receives the slot, the source rect in
 * framebuffer screen pixels, and `frame` the frame the copy was taken on, whose
 * scene is the one the resolving present must compose — the copy reads what the
 * game drew on that frame, not what it draws on the next. Returns 0 when
 * nothing is pending. */
int openpete_fb_snapshot_take_pending(int *idx, int *src_x, int *src_y,
                                      int *w, int *h, unsigned *frame);

/* Drain one draw area waiting for a content image: `idx` receives the area, the
 * area's own rect in framebuffer screen pixels, and `frame` as above. Returns 0
 * when none waits. */
int openpete_fb_snapshot_take_pending_area(int *idx, int *src_x, int *src_y,
                                           int *w, int *h, unsigned *frame);

/* Hand a resolved slot its pixels (RGBA8, `pw` by `ph`, tightly packed). The
 * store takes ownership of `px`. A slot with no pixels stays invisible to
 * openpete_fb_snapshot_find, so the quad keeps the stock VRAM path. */
void openpete_fb_snapshot_store(int idx, unsigned char *px, int pw, int ph);

/* Hand a drained draw area the crop of the present composed into it, covering
 * the rect openpete_fb_snapshot_take_pending_area reported. The store takes
 * ownership of `px`. A later window copy out of that area crops this image
 * instead of waiting for a present that will never draw the area again. */
void openpete_fb_snapshot_store_area(int idx, unsigned char *px, int pw, int ph);

/* Resolve a fed textured quad against the store. `tpage_x`/`tpage_y` are the
 * VRAM texel origin of its texture page and u0..v1 its texel box, both as the
 * primitive spells them. The box, not the page origin, must lie inside a stored
 * rect: a window copied to a row below its page's top edge is sampled with v
 * offset by that row. A texel box inside a draw area that carries a content
 * image matches too, which is how a quad sampling the framebuffer itself — the
 * shrink of a frozen frame before it is copied out — is served. On a hit
 * returns the slot and fills `su`/`sv`, the
 * scale from page texels to snapshot pixels, and `ox`/`oy`, the snapshot pixel
 * of the page origin (negative when the page origin precedes the rect).
 * Returns -1 on a miss. Only 16-bit pages match: a copied
 * framebuffer is re-displayed at one texel per pixel. */
int openpete_fb_snapshot_find(int tpage_x, int tpage_y, int bpp,
                              int u0, int v0, int u1, int v1,
                              float *su, float *sv, float *ox, float *oy);

/* Pixels and upload key of a slot openpete_fb_snapshot_find returned, window
 * or draw area alike (NULL while unresolved). The key is
 * unique per stored image for the lifetime of the run, so a texture cache
 * keyed on it cannot serve an earlier snapshot whose buffer was freed and
 * whose address was reused. */
const unsigned char *openpete_fb_snapshot_pixels(int idx, int *pw, int *ph);
uint64_t openpete_fb_snapshot_key(int idx);

/* Engagement counters: rects recorded by a move, rects given pixels, and quads
 * redirected into a snapshot since boot. */
void openpete_fb_snapshot_counts(unsigned *taken, unsigned *resolved,
                                 unsigned *quads);

/* OPENPETE_FBSNAP=0 disables the whole mechanism (the negative control: every
 * quad keeps the stock VRAM path). */
int openpete_fb_snapshot_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_FB_SNAPSHOT_H */
