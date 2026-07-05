/* texture_pack — VRAM-region registry for HD texture replacement.
 *
 * The identity core of the texture-replacement feature
 * (notes/plans/openpete-texture-replacement.md, Phase 1): every VRAM
 * upload is recorded at the W2.4 native-VRAM op seam with its destination
 * rect and an XXH3-64 content hash; CLUT uploads are recorded the same way
 * and paired at draw time, hashed over the used index range only. Draw-time
 * material keys ({tpage,clut,bpp} + UV box) resolve through the registry to
 * a stable identity that survives per-level VRAM repacking.
 *
 * Game-agnostic: fed by core/runtime/native_vram.c; optional symbolic asset
 * names arrive from the game layer (games/spyro-1/) as (rect -> name)
 * annotations. Renderer-agnostic: this module never touches the RHI — the
 * native renderer's bind path queries it (Phase 4).
 *
 * Determinism: read-only observer of upload traffic. Writes nothing to PSX
 * RAM, VRAM, or any gameplay-readable byte. */
#ifndef OPENPETE_TEXTURE_PACK_H
#define OPENPETE_TEXTURE_PACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- registry feed (called from the native_vram op seam) ---------------- */

/* Record a LoadImage-family upload: dest rect (VRAM 16-bit-pixel units,
 * post-mask/clamp) + the tightly-packed source rows (stride = pre-clamp
 * width, as native_vram receives them). */
void texture_pack_on_load(int x, int y, int w, int h,
                          const uint16_t *src, int src_stride);

/* Record a MoveImage: identity travels with the pixels (a VRAM->VRAM copy
 * has no source bytes to hash). */
void texture_pack_on_move(int src_x, int src_y, int w, int h,
                          int dst_x, int dst_y);

/* Record a ClearImage: entries covered by the rect are invalidated. */
void texture_pack_on_clear(int x, int y, int w, int h);

/* ---- draw-time query (the material seam, Phase 4) ----------------------- */

/* Resolved identity for one draw-time material + UV box. `thash` is the
 * covering upload's content hash; `phash` the paired CLUT hash over the
 * used index range (0 for 16bpp); `box_*` the upload-relative texel box the
 * UVs address (sub-rect identity within packed pages). */
typedef struct {
    uint64_t thash;
    uint64_t phash;
    int      bpp;
    int      box_x, box_y, box_w, box_h;   /* relative to the upload rect */
    int      up_w, up_h;                   /* full upload dims, texels of this bpp */
    const char *asset;                     /* symbolic name, or NULL */
} texture_pack_id;

/* Resolve a draw-time key. tpage/clut in VRAM pixel units (as carried by
 * openpete_scene_tri); u0/v0/u1/v1 the texel bounding box the draw samples
 * within the page. Returns 1 and fills `out` if the region maps to a live
 * registry entry; 0 if unknown (never uploaded / cleared / partially
 * overwritten). */
int texture_pack_resolve(int tpage_x, int tpage_y, int clut_x, int clut_y,
                         int bpp, int u0, int v0, int u1, int v1,
                         texture_pack_id *out);

/* ---- symbolic annotation (game layer, Phase 7) --------------------------- */

/* Name the next upload(s) that land exactly on `rect` (x,y,w,h). The game
 * layer calls this from load-site knowledge; the registry attaches the name
 * to matching entries as they arrive. `name` must be a string literal or
 * otherwise outlive the registry. */
void texture_pack_annotate(int x, int y, int w, int h, const char *name);

/* ---- replacement packs (Phase 3) ----------------------------------------- */

/* True if a replacement directory exists and holds at least one PNG.
 * Dir: $OPENPETE_TEXPACK_DIR, else <exe>/assets/textures/replacements/.
 * Cheap flag read — callable per drawn run. */
int texture_pack_replacements_active(void);

/* A matched replacement: the image plus the FILE's texel box (upload-
 * relative). Matching is by CONTAINMENT: a pack file covers every query box
 * inside its own box (same thash/bpp/phash), so one 64x64 tile file serves
 * both the LP full-tile draw and the HP 2x2 sub-quad draws, and per-frame
 * clipped boxes still resolve. Smallest containing file wins. */
typedef struct {
    const unsigned char *pixels;   /* RGBA8, borrowed from the cache */
    int w, h;                      /* replacement image dimensions */
    int box_x, box_y, box_w, box_h; /* the FILE's texel box, upload-relative */
} texture_pack_repl;

/* Match a resolved identity against the pack index (filenames parsed at
 * init). Lazy-loads the image on first match; memoized per identity. */
int texture_pack_match(const texture_pack_id *id, texture_pack_repl *out);

/* Stable 64-bit key over the full identity (thash+phash+box+bpp) — for
 * caller-side caches (e.g. the renderer's identity->GPU-texture map). */
uint64_t texture_pack_id_hashkey(const texture_pack_id *id);

/* ---- dump tool (Phase 2) ------------------------------------------------- */

/* If OPENPETE_TEXPACK_DUMP_DIR is set, decode the draw's texel box from the
 * live VRAM model and write it as a baked RGBA PNG named by its identity:
 *   <thash>-<bx>x<by>+<bw>x<bh>-<phash>-<bpp>bpp.png
 * (box coords are upload-relative, so the name is stable across per-level
 * VRAM repacks). Deduped per identity; call freely per drawn run. */
void texture_pack_dump_draw(const texture_pack_id *id,
                            int tpage_x, int tpage_y, int clut_x, int clut_y,
                            int u0, int v0);

/* ---- lifecycle / diagnostics -------------------------------------------- */

void texture_pack_init(void);      /* idempotent; reads env gates */
void texture_pack_shutdown(void);

/* One line per live entry to stderr (OPENPETE_TEXPACK_DUMP_REGISTRY, or
 * called explicitly from debug paths). */
void texture_pack_debug_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TEXTURE_PACK_H */
