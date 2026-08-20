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

/* Guest-state discontinuity (savestate load): VRAM was wholesale-replaced
 * WITHOUT flowing through the op seam above, so live entries may describe
 * pre-load uploads. Re-hashes every live entry's rect against the restored
 * VRAM: matching content keeps its identity (same-level load = everything
 * survives), mismatches are killed (never serve a wrong replacement; the
 * asset returns on the next real upload). Registered with the
 * openpete_discontinuity broadcast by texture_pack_init(). */
void texture_pack_on_restore(void);

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

/* Override the replacement-pack directory (from the --texpack-dir CLI flag).
 * Takes precedence over $OPENPETE_TEXPACK_DIR and the built-in default. Call
 * BEFORE texture_pack_init() (the scan reads it). NULL / "" clears the
 * override (fall back to env, then default). */
void texture_pack_set_dir_override(const char *dir);

/* True if a replacement directory exists and holds at least one PNG.
 * Dir: --texpack-dir, else $OPENPETE_TEXPACK_DIR, else
 * <exe>/assets/textures/replacements/. Cheap flag read — callable per drawn run. */
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

/* Claim-scoping query: does ANY enabled pack file touch this identity's
 * texel box? texture_pack_match tests containment because draws query
 * per-tri boxes; a caller holding a whole FACE's box needs the conservative
 * cover of every possible sub-box query, which is intersection — a file
 * contained anywhere inside the face box intersects it. Index-only (never
 * loads the image); symbolic entries match by annotated asset name (they
 * cover the full upload, so any overlap counts). */
int texture_pack_overlaps(const texture_pack_id *id);

/* Stable 64-bit key over the full identity (thash+phash+box+bpp) — for
 * caller-side caches (e.g. the renderer's identity->GPU-texture map). */
uint64_t texture_pack_id_hashkey(const texture_pack_id *id);

/* ---- dump tool (Phase 2) ------------------------------------------------- */

/* If OPENPETE_TEXPACK_DUMP_DIR is set, decode the draw's texel box from the
 * live VRAM model and write it as a baked RGBA PNG named by its identity:
 *   <label>-<thash>-<bx>x<by>+<bw>x<bh>-<phash>-<bpp>bpp.png
 * (box coords are upload-relative, so the name is stable across per-level
 * VRAM repacks). `label` is the draw's scene category, lowercased — purely
 * organisational, so a pack author can filter a dump to just the terrain or
 * just Spyro; identity is the hash fields, and the parser skips the label.
 *
 * `sx`/`sy` replicate each texel into an sx-by-sy block. Screen-space 2D draws
 * map 1 texel to 1 framebuffer pixel, and Spyro's framebuffer has non-square
 * pixels — 512x240 with 224 visible shown at 4:3 — so their texels are ~1.71x
 * taller than wide and the raw decode reads horizontally stretched in any
 * image viewer. Correcting that makes dumps editable at square pixels. World
 * textures are mapped onto geometry the projection already aspect-corrects,
 * so they pass 1,1 — stretching those would INTRODUCE distortion.
 *
 * The factors must be INTEGERS (the caller reduces the aspect to lowest
 * terms) so that block boundaries land exactly on texel boundaries. That is
 * what lets a dump be dropped straight back in as a replacement and render
 * bit-identically: an integer-multiple image is sampled NEAREST, and each
 * screen pixel then resolves to the very texel stock would have fetched. A
 * non-integer stretch breaks that alignment and cannot round-trip exactly.
 *
 * Deduped per identity; call freely per drawn run. */
void texture_pack_dump_draw(const texture_pack_id *id,
                            int tpage_x, int tpage_y, int clut_x, int clut_y,
                            int u0, int v0, int category, int sx, int sy);

/* ---- identity/provenance census (temporary instrument) ------------------ */

/* OPENPETE_TEXPACK_CATCENSUS=1: record which scene categories and provenance
 * tags each resolved identity is drawn under, and how many distinct CLUTs
 * (fog ladder rows) share one texel box. Answers two design questions:
 * whether an identity is ever drawn by more than one category (i.e. whether
 * the renderer path belongs in the match key), and how far the fog ladder
 * fans one texture out. Reports to stderr at exit. */
void texture_pack_cat_census(const texture_pack_id *id, int category, unsigned tag);

/* ---- pack fog shift (mod-supplied scene transform) ----------------------- */

/* A full-coverage replacement pack that re-grades the scene (v' = s*v + o
 * per channel) leaves the CLUT-ladder fog affine pointing at the STOCK
 * ambience: replaced texels are fogged toward colours the pack no longer
 * matches (dark near terrain blending to bright pastel at distance). With a
 * shift registered, every fog affine's offset is corrected to the pack's
 * ambience — b' = s*b + (1-a)*o, slope unchanged — which is exactly
 * M(a*x + b) for pack pixels p ~= M(x). scale NULL clears. Display-only. */
void texture_pack_fog_shift_set(const float scale_rgb[3], const float offset_rgb[3]);
/* Returns 1 (and fills s/o) when a shift is active. */
int  texture_pack_fog_shift_get(float scale_rgb[3], float offset_rgb[3]);

/* ---- terrain fog-ladder ramp (continuous pack fog) ----------------------- */

/* Spyro's terrain selects one pre-fogged CLUT row PER FACE (row Δ from the
 * face-mean fog key). Replaced draws re-create the row as an affine on the
 * pack texels — but quantized per face, the affine's rung steps show on art
 * whose grade differs from the stock rows. The HD pipes therefore evaluate
 * the ladder CONTINUOUSLY: t = clamp(w*t_scale + t_bias, 0, 1) per fragment
 * (w = true view-Z), interpolating the identity->deepest-row affine — the
 * same fade og's rows quantize, minus the per-face steps. Display-only, HD
 * replaced draws only; stock draws keep the authentic per-face rows.
 *
 * The terrain feeder publishes the ramp each walk from the level's fog
 * reference and the --fog-scale curve (t = (viewZ*fogscale - fogRef + 4096)
 * / (16 rows * 256 key units)); (0,0) = no ladder this frame (ladder off /
 * fog saturated away), which drops the HD pipes back to per-face rungs. */
void texture_pack_fog_ladder_set(float t_scale, float t_bias);
/* Returns 1 (and fills out) when a ramp is published this frame. */
int  texture_pack_fog_ladder_get(float *t_scale, float *t_bias);

/* Flags a feeder may OR above the row byte (bits 7:0) of the value it passes
 * to openpete_native_scene_set_fog_delta(): CONT marks a face that went
 * through the ladder select (fades on the published ramp); FOGBIAS marks a
 * face whose fog reference carries the authored +0x2000 bias. */
#define OPENPETE_TP_FOG_CONT    0x100
#define OPENPETE_TP_FOG_FOGBIAS 0x200

/* Registry generation counter: bumps on ANY mutation that can change a
 * resolve/match/overlap answer — VRAM uploads, MoveImage, ClearImage, pack
 * index rescans, and enabled-modset changes (polled here, so latched callers
 * see toggles without any query traffic). For caller-side caches keyed on
 * resolutions or on borrowed `texture_pack_repl.pixels` pointers. */
unsigned texture_pack_generation(void);

/* Per-texture stock->pack grade: least-squares affine fit `pack ~= p*stock+q`
 * per channel (0..1 units) over this file's own (stock texel, pack pixel)
 * pairs — the pack's regrade of THIS texture, exact where the pack is an
 * affine of stock and the best local approximation elsewhere. Consumers:
 * fog-offset retarget (fogged = a*pack + (p*b + (1-a)*q)) and pack-aware
 * recolouring of untextured geometry authored against the stock texels.
 * tpage/clut in VRAM pixel units, clut_y the BASE (unfogged) row; pu0/pv0 =
 * the FILE box origin in page texel coords. Memoized per (file, palette);
 * transparent/cutout texels excluded; returns 1 on signal (>= 64 pairs),
 * 0 = caller falls back (e.g. the global fog shift). */
int texture_pack_grade_fit(const texture_pack_id *id, const texture_pack_repl *repl,
                           int tpage_x, int tpage_y, int clut_x, int clut_y,
                           int bpp, int pu0, int pv0, float p[3], float q[3]);

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
