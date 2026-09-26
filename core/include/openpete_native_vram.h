/* openpete_native_vram.h — the natively-owned PSX VRAM model.
 *
 * A 1024x512 uint16 surface written by native implementations of the PSYQ
 * image ops (LoadImage / MoveImage / ClearImage, including the OT-embedded
 * DR_LOAD/DR_MOVE and the LoadTPage/LoadClut wrappers, all forwarded from
 * PsyCross's LIBGPU adapters via PsyX_SetVRAMOpCallbacks). The ops replicate
 * the PSX GPU coordinate masking/clamping semantics of PsyCross's
 * GR_CopyVRAM / GR_ClearVRAM to the byte, so while PsyCross is still alive
 * the model can be memcmp-verified against its mirror per written rect
 * (OPENPETE_VRAM_VERIFY, native_render.c).
 *
 * While PsyCross exists, framebuffer-copyback content (GL FBO -> vram) is
 * NOT produced natively — those regions are shadow-copied from PsyCross's
 * mirror at present time (the dirty-rect path), which also
 * re-shadows op-written rects (byte-identical when the ops are correct).
 * StoreImage (VRAM -> gameplay RAM, canonical) keeps reading PsyCross's
 * mirror until the native renderer owns framebuffer rendering.
 */

#ifndef OPENPETE_NATIVE_VRAM_H
#define OPENPETE_NATIVE_VRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_VRAM_W 1024
#define OPENPETE_VRAM_H 512

/** @brief Return the VRAM model's storage.
 *
 * A static OPENPETE_VRAM_W x OPENPETE_VRAM_H array of 16-bit texels, row
 * stride OPENPETE_VRAM_W. The native renderer uses it as its CPU VRAM
 * mirror; the texture-pack registry and the terrain decoder read it.
 * @return Pointer to texel (0, 0). */
uint16_t* openpete_vram_data(void);

/** @brief Install the native image ops into PsyCross and initialise the texture-pack registry.
 *
 * Registers openpete_vram_load, openpete_vram_move and openpete_vram_clear
 * as PsyCross's VRAM op callbacks, then calls texture_pack_init. Called at
 * native renderer bring-up. Idempotent: a repeat registers the same table
 * and texture_pack_init returns at once. */
void openpete_vram_install_ops(void);

/* Native op implementations (also callable directly once PsyCross goes). */
/** @brief Copy a tightly packed image into the VRAM model (the PSYQ LoadImage op).
 *
 * Replicates PsyCross's GR_CopyVRAM(src, 0, 0, w, h, x, y): the destination
 * origin is masked to the VRAM size and the width and height are clamped at
 * its right and bottom edges, while the source keeps the original @p w as
 * its row stride. The written rect is logged for the verify census and
 * passed to the texture-pack registry; OPENPETE_TEXPACK_CENSUS logs it with
 * a content hash. A non-positive size or NULL @p src is ignored.
 * @param x Destination X in texels.
 * @param y Destination Y in texels.
 * @param w Width in texels.
 * @param h Height in rows.
 * @param src Source texels, @p w per row. */
void openpete_vram_load (int x, int y, int w, int h, const uint16_t* src);
void openpete_vram_move (int src_x, int src_y, int w, int h, int dst_x, int dst_y);
void openpete_vram_clear(int x, int y, int w, int h,
                         uint8_t r, uint8_t g, uint8_t b);

/* Verify-census support: effective (post-mask/clamp) rects written by the
 * ops since the last drain. Returns the count copied to out (up to max);
 * *overflowed is set if the internal list overflowed since the last drain
 * (census must then skip exact-match accounting for the window). */
typedef struct { int x, y, w, h; } openpete_vram_op_rect;
int openpete_vram_drain_op_rects(openpete_vram_op_rect* out, int max, int* overflowed);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_VRAM_H */
