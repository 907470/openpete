/* texture_pack — VRAM-region registry for HD texture replacement.
 *
 * The identity core of the texture-replacement feature: every VRAM
 * upload is recorded at the native-VRAM op seam with its destination
 * rect and an XXH3-64 content hash; CLUT uploads are recorded the same way
 * and paired at draw time, hashed over the used index range only. Draw-time
 * material keys ({tpage,clut,bpp} + UV box) resolve through the registry to
 * a stable identity that survives per-level VRAM repacking.
 *
 * Game-agnostic: fed by core/renderer/native_vram.c; optional symbolic asset
 * names arrive from the game layer (games/spyro-1/) as (rect -> name)
 * annotations. Renderer-agnostic: this module never touches the RHI — the
 * native renderer's bind path queries it.
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

/** @brief Record a LoadImage upload as a registry entry keyed by the XXH3-64 hash of its source rows.
 *
 * The rect is in VRAM halfword units after native_vram's mask and clamp;
 * the hash covers w halfwords of each of h rows read at src_stride, the
 * pre-clamp width. A live entry on exactly the same rect is reused in
 * place; every other overlapping entry is killed, since a partial
 * overwrite leaves no stable identity. An annotation on the same rect
 * names the entry when its bound hash matches, and an armed annotation
 * binds to this upload's hash first. Bumps the registry generation.
 * Ignored for an empty rect or a NULL src.
 * @param x Destination X in VRAM halfwords.
 * @param y Destination row.
 * @param w Width in halfwords.
 * @param h Height in rows.
 * @param src Source rows.
 * @param src_stride Source row stride in halfwords. */
void texture_pack_on_load(int x, int y, int w, int h,
                          const uint16_t *src, int src_stride);

/** @brief Record a MoveImage: identity travels with the pixels, since a VRAM-to-VRAM copy has no source bytes to hash.
 *
 * Entries the destination rect overlaps are killed first; then each live
 * entry wholly inside the source rect is copied to the destination,
 * translated. An entry only partly inside the source rect is not copied,
 * and a source entry that also overlaps the destination was killed before
 * the copy. Bumps the registry generation; ignored for an empty rect.
 * @param src_x Source X in VRAM halfwords.
 * @param src_y Source row.
 * @param w Width in halfwords.
 * @param h Height in rows.
 * @param dst_x Destination X in VRAM halfwords.
 * @param dst_y Destination row. */
void texture_pack_on_move(int src_x, int src_y, int w, int h,
                          int dst_x, int dst_y);

/** @brief Record a ClearImage: every entry the rect overlaps is killed.
 *
 * Bumps the registry generation; ignored for an empty rect.
 * @param x X in VRAM halfwords.
 * @param y Row.
 * @param w Width in halfwords.
 * @param h Height in rows. */
void texture_pack_on_clear(int x, int y, int w, int h);

/** @brief Re-verify every live entry against VRAM after a guest-state discontinuity such as a savestate load.
 *
 * A restore replaces VRAM without passing through the op seam above, so
 * live entries may describe pre-restore uploads. Each live entry's rect is
 * re-hashed from a PsyX VRAM snapshot: matching content keeps its
 * identity, and a mismatch or a rect outside VRAM is killed, so no wrong
 * replacement is served; the asset returns on its next upload. The hash
 * of an upload whose rect was clamped covers the pre-clamp source, so
 * such an entry fails the re-hash and is killed. When the snapshot cannot
 * be taken every live entry is killed. Bumps the registry generation when
 * any entry dies; a no-op before texture_pack_init, which registers it
 * with the openpete_discontinuity broadcast. */
void texture_pack_on_restore(void);

/* ---- draw-time query (the material seam) -------------------------------- */

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

/** @brief Resolve a draw-time material and texel box to the live registry entry that covers it.
 *
 * The box's VRAM columns follow the bpp packing (u, u/2 or u/4 halfwords
 * from tpage_x). The first live entry whose rect contains the whole box
 * supplies thash, the asset name, the full upload size in this bpp's
 * texels, and the box relative to the upload origin. phash is the XXH3 of
 * the full 16- or 256-entry CLUT row read from the native VRAM model, 0
 * for 16bpp. Hits and misses are cached per material and box until the
 * registry generation changes.
 * @param tpage_x Texture page X in VRAM halfwords.
 * @param tpage_y Texture page Y in VRAM rows.
 * @param clut_x CLUT X in VRAM halfwords.
 * @param clut_y CLUT row.
 * @param bpp 4, 8 or 16.
 * @param u0 Box left texel within the page.
 * @param v0 Box top texel within the page.
 * @param u1 Box right texel, inclusive.
 * @param v1 Box bottom texel, inclusive.
 * @param out Receives the identity on a hit.
 * @return 1 on a hit; 0 when no live entry covers the box (never
 *         uploaded, cleared or partly overwritten), the box is inverted,
 *         or the registry is not initialized. */
int texture_pack_resolve(int tpage_x, int tpage_y, int clut_x, int clut_y,
                         int bpp, int u0, int v0, int u1, int v1,
                         texture_pack_id *out);

/* ---- symbolic annotation (game layer) ------------------------------------ */

/** @brief Name one texture by content: the upload live on exactly (x, y, w, h) now, or the next upload landing there when none is live.
 *
 * The name binds to that upload's content hash; later uploads on the same
 * rect carry it only when their bytes match, since VRAM blocks are shared
 * across screens and levels. At most 128 annotations are held and further
 * calls are ignored. The binding is logged to stderr.
 * @param x X in VRAM halfwords.
 * @param y Row.
 * @param w Width in halfwords.
 * @param h Height in rows.
 * @param name Asset name, matched against a `<name>.png` pack file; a
 *        string literal or otherwise outliving the registry. */
void texture_pack_annotate(int x, int y, int w, int h, const char *name);

/* ---- replacement packs -------------------------------------------------- */

/** @brief Set the replacement-pack directory from the --texpack-dir flag.
 *
 * The override takes precedence over $OPENPETE_TEXPACK_DIR and the
 * built-in <exe>/assets/textures, and while either of the first two is set
 * the user pack directory is not scanned; mod asset roots are scanned in
 * every case. The scan runs in texture_pack_init, so the call comes before
 * it.
 * @param dir Directory; NULL or "" clears the override. */
void texture_pack_set_dir_override(const char *dir);

/** @brief Return 1 when the pack index holds at least one replacement file from any source.
 *
 * Sources: the pack directory (--texpack-dir, else $OPENPETE_TEXPACK_DIR,
 * else <exe>/assets/textures/ joined by the user pack directory), then
 * every mod asset root; files of a disabled mod's source count too. After
 * texture_pack_init the call first folds in mod toggles and newly
 * registered asset roots; otherwise it is a flag read, callable per drawn
 * run.
 * @return 1 when the index is nonempty. */
int texture_pack_replacements_active(void);

/* A matched replacement: the image plus the FILE's texel box (upload-
 * relative). Matching is by CONTAINMENT: a pack file covers every query box
 * inside its own box (same thash/bpp/phash), so one 64x64 tile file serves
 * both the LP full-tile draw and the HP 2x2 sub-quad draws, and per-triangle
 * clipped boxes still resolve. Smallest containing file wins. */
typedef struct {
    const unsigned char *pixels;   /* RGBA8, borrowed from the cache */
    int w, h;                      /* replacement image dimensions */
    int box_x, box_y, box_w, box_h; /* the FILE's texel box, upload-relative */
} texture_pack_repl;

/** @brief Find the pack file that serves a resolved identity and return its decoded image.
 *
 * A symbolic file whose name equals the identity's asset name wins, the
 * last such file in source order. Otherwise the hash files with equal
 * thash, bpp and phash whose box contains the query box compete: the
 * smallest area wins, the later source on a tie. Only sources whose owning
 * mod is enabled serve, so toggling a later source off falls back to an
 * earlier one. The chosen file is memoized per identity key until the
 * registry generation changes. Images decode on a worker thread: a file
 * not yet resident is queued at the front of the decode queue and the call
 * returns 0, so the draw uses the base texture until the decode lands.
 * @param id Identity from texture_pack_resolve.
 * @param out Receives the borrowed RGBA8 pixels, their dimensions, and the
 *        file's box (the full upload for a symbolic file).
 * @return 1 when out was filled; 0 when no file serves, the decode is not
 *         ready or failed, or id is NULL. */
int texture_pack_match(const texture_pack_id *id, texture_pack_repl *out);

/** @brief Return 1 when any enabled pack file touches the identity's texel box.
 *
 * texture_pack_match tests containment because draws query per-triangle
 * boxes; a caller holding a whole face's box needs the conservative cover
 * of every sub-box query, which is intersection: a file contained anywhere
 * inside the face box intersects it. A hash file counts on equal thash,
 * bpp and phash and an intersecting box; a symbolic file counts when its
 * name equals the identity's asset name, since it covers the full upload.
 * Reads the index only and never decodes an image.
 * @param id Resolved identity; NULL returns 0.
 * @return 1 on any overlap, 0 otherwise or when no pack is active. */
int texture_pack_overlaps(const texture_pack_id *id);

/** @brief Return a nonzero 64-bit key mixed from thash, phash, box and bpp, for caller-side caches keyed on an identity.
 *
 * The same mix keys the match memo. It is not collision-free.
 * @param id Resolved identity.
 * @return The key. */
uint64_t texture_pack_id_hashkey(const texture_pack_id *id);

/* ---- dump tool ----------------------------------------------------------- */

/** @brief Write the draw's texel cluster as an RGBA PNG named by its identity, when OPENPETE_TEXPACK_DUMP_DIR is set.
 *
 * Boxes accumulate per (thash, phash, bpp) into clusters: a query box that
 * overlaps one cluster grows it, one that overlaps several merges them,
 * and one that overlaps none starts a new cluster, so the clipped
 * fragments of one tile coalesce while adjacent disjoint tiles stay
 * separate. A cluster that grew is decoded from the VRAM model and
 * rewritten, and its superseded file is removed; a draw that grows nothing
 * writes nothing. The file is named
 *   <label>-<thash>-<bx>x<by>+<bw>x<bh>-<phash>-<bpp>bpp.png
 * with the cluster box upload-relative, so the name is stable across
 * per-level VRAM repacks. `label` is the lowercase slug of the scene
 * category of the draw that last grew the cluster; it is organisational
 * and the parser skips it. Alpha: a 0x0000 texel writes 0, an STP-set
 * texel 128, any other 255. A key whose 32-slot probe window is full
 * writes nothing.
 *
 * `sx`/`sy` replicate each texel into an sx-by-sy block. Screen-space 2D
 * draws map 1 texel to 1 framebuffer pixel, and the framebuffer's pixels
 * are not square (512 wide, 224 visible rows shown at 4:3, each pixel
 * about 1.71 times taller than wide), so the raw decode reads horizontally
 * stretched in an image viewer. The native renderer passes 4*clip_h :
 * 3*disp_w reduced to lowest terms (7:12 at 512x224) for HUD and overlay
 * draws and 1,1 for every other category, since the projection already
 * aspect-corrects world textures. The factors are integers so block
 * boundaries land on texel boundaries: a dump dropped back in as a
 * replacement is sampled nearest and each screen pixel resolves to the
 * texel stock would have fetched.
 * @param id Resolved identity; NULL is ignored.
 * @param tpage_x Texture page X in VRAM halfwords.
 * @param tpage_y Texture page Y in VRAM rows.
 * @param clut_x CLUT X in VRAM halfwords.
 * @param clut_y CLUT row the draw samples.
 * @param u0 Page U of the query box origin; anchors page to upload coordinates.
 * @param v0 Page V of the query box origin.
 * @param category OPENPETE_SCENE_* category of the draw.
 * @param sx Horizontal replication factor.
 * @param sy Vertical replication factor. */
void texture_pack_dump_draw(const texture_pack_id *id,
                            int tpage_x, int tpage_y, int clut_x, int clut_y,
                            int u0, int v0, int category, int sx, int sy);

/* ---- identity/provenance census (temporary instrument) ------------------ */

/** @brief Record the scene category and provenance tag a resolved identity is drawn under, when OPENPETE_TEXPACK_CATCENSUS is set and not "0".
 *
 * At exit the census prints to stderr the identities and draws per
 * category, the identities drawn under more than one category or tag, and
 * per texel box the count of distinct CLUTs (fog ladder rows) sharing it.
 * The first two show whether the renderer path belongs in the match key;
 * the last measures how far the fog ladder fans one texture out. The table
 * holds 16384 identities; an identity whose 32-slot probe window is full
 * is dropped and the report is marked truncated.
 * @param id Resolved identity; NULL is ignored.
 * @param category OPENPETE_SCENE_* category; outside 0..31 it counts the
 *        draw without setting a category bit.
 * @param tag Provenance tag; the first seen is kept and a later different
 *        one marks the identity as mixed. */
void texture_pack_cat_census(const texture_pack_id *id, int category, unsigned tag);

/* ---- pack fog shift (mod-supplied scene transform) ----------------------- */

/** @brief Register or clear a pack's scene regrade v' = s*v + o per channel, retargeting the CLUT-ladder fog to the pack's colours.
 *
 * A full-coverage replacement pack that regrades the scene leaves the
 * CLUT-ladder fog affine pointing at the stock ambience: replaced texels
 * are fogged toward colours the pack no longer matches (dark near terrain
 * blending to bright pastel at distance). With a shift registered, the fog
 * affine's offset becomes b' = s*b + (1-a)*o with the slope unchanged,
 * which equals M(a*x + b) for pack pixels p = M(x), M(v) = s*v + o. A
 * per-texture fit from texture_pack_grade_fit takes precedence where one
 * exists. Set by mods through the mod API; display-only.
 * @param scale_rgb Per-channel s; NULL clears the shift.
 * @param offset_rgb Per-channel o in 0..255 units; NULL clears the shift. */
void texture_pack_fog_shift_set(const float scale_rgb[3], const float offset_rgb[3]);
/** @brief Copy the registered shift into scale_rgb and offset_rgb and return 1, or return 0 when none is set.
 *
 * @param scale_rgb Receives the per-channel scale.
 * @param offset_rgb Receives the per-channel offset, 0..255 units.
 * @return 1 when a shift is registered. */
int  texture_pack_fog_shift_get(float scale_rgb[3], float offset_rgb[3]);

/* ---- terrain fog-ladder ramp (continuous pack fog) ----------------------- */

/** @brief Publish the terrain walk's fog-ladder ramp for continuous fog on HD-replaced draws.
 *
 * Spyro's terrain selects one pre-fogged CLUT row per face (row Δ from the
 * face-mean fog key). Replaced draws re-create the row as an affine on the
 * pack texels, and quantized per face the affine's rung steps show on art
 * whose grade differs from the stock rows. The HD pipes therefore evaluate
 * the ladder per fragment, t = clamp(w*t_scale + t_bias, 0, 1) with w the
 * view-Z, interpolating from the identity affine to the deepest row's
 * affine: the same fade the stock rows quantize, without the per-face
 * steps. Only faces flagged OPENPETE_TP_FOG_CONT use it; stock draws keep
 * the per-face rows. Display-only.
 *
 * The terrain feeder publishes the ramp each walk from the level's fog
 * reference and the --fog-scale value: the row select is
 * Δ = (key - fogRef + 4096) >> 8 over 16 rows, so t = Δ/15 =
 * (vz*fogscale/16 - fogRef + 4096) / 3840 with vz the full-world view-Z.
 * (0, 0) publishes no ramp (ladder off), which returns the HD pipes to
 * per-face rows.
 * @param t_scale Slope of t in view-Z.
 * @param t_bias Offset of t. */
void texture_pack_fog_ladder_set(float t_scale, float t_bias);
/** @brief Copy the published ramp into t_scale and t_bias and return 1, or return 0 when the last published ramp was (0, 0).
 *
 * @param t_scale Receives the slope.
 * @param t_bias Receives the offset.
 * @return 1 when a ramp is published. */
int  texture_pack_fog_ladder_get(float *t_scale, float *t_bias);

/* Flags a feeder may OR above the row byte (bits 7:0) of the value it passes
 * to openpete_native_scene_set_fog_delta(): CONT marks a face that went
 * through the ladder select (fades on the published ramp); FOGBIAS marks a
 * face whose fog reference carries the authored +0x2000 bias. */
#define OPENPETE_TP_FOG_CONT    0x100
#define OPENPETE_TP_FOG_FOGBIAS 0x200

/** @brief Return the registry generation, which changes on every mutation that can change a resolve, match or overlap answer.
 *
 * Bumped by VRAM uploads, MoveImage, ClearImage, entries a restore kills,
 * mod sources added after the boot scan, mod toggles, and shutdown. The
 * call polls the mod set, so a caller keying a cache on the generation
 * sees a toggle without issuing any resolve or match. For caller-side
 * caches keyed on resolutions or on borrowed `texture_pack_repl.pixels`
 * pointers.
 * @return The generation. */
unsigned texture_pack_generation(void);

/** @brief Fit the per-channel affine pack = p*stock + q between a replacement's pixels and the stock texels it replaces.
 *
 * Least squares over this file's own (stock texel, pack pixel) pairs, in
 * 0..1 units (5-bit stock channels over 31, pack bytes over 255): the
 * pack's regrade of this texture, exact where the pack is an affine of
 * stock and the best local approximation elsewhere. Each texel of the file
 * box reads its stock colour from the VRAM model, through the CLUT for 4
 * and 8 bpp, and its pack colour from the replacement pixel nearest the
 * texel centre. A 0x0000 stock texel and a pack pixel with alpha below 128
 * are skipped. A channel whose stock values are constant gets p = 1 and
 * the mean offset. Consumers: the fog-offset retarget
 * (fogged = a*pack + (p*b + (1-a)*q)) and pack-aware recolouring of
 * untextured geometry authored against the stock texels. Memoized per
 * (pixel buffer, phash) until the registry generation changes.
 * @param id Resolved identity; its phash keys the memo.
 * @param repl Match result; supplies the pixels and the file box size.
 * @param tpage_x Texture page X in VRAM halfwords.
 * @param tpage_y Texture page Y in VRAM rows.
 * @param clut_x CLUT X in VRAM halfwords.
 * @param clut_y The base (unfogged) CLUT row.
 * @param bpp 4, 8 or 16.
 * @param pu0 File box origin U in page texels.
 * @param pv0 File box origin V in page texels.
 * @param p Receives the per-channel slope.
 * @param q Receives the per-channel offset.
 * @return 1 on a fit from at least 64 pairs; 0 on fewer, on no pixels or an
 *         empty box, or without a VRAM model, and the caller falls back
 *         (the global fog shift). */
int texture_pack_grade_fit(const texture_pack_id *id, const texture_pack_repl *repl,
                           int tpage_x, int tpage_y, int clut_x, int clut_y,
                           int bpp, int pu0, int pv0, float p[3], float q[3]);

/* ---- lifecycle / diagnostics -------------------------------------------- */

/** @brief Register the restore hook, scan the replacement-pack sources and start the decode prefetch; later calls return immediately.
 *
 * Registers texture_pack_on_restore with the guest-discontinuity
 * broadcast, then indexes the pack directory, the user pack directory
 * when neither --texpack-dir nor $OPENPETE_TEXPACK_DIR is set, and every
 * mod asset root registered so far. Called from
 * openpete_vram_install_ops. */
void texture_pack_init(void);
/** @brief Stop the decode worker, free decoded pixels and empty the registry.
 *
 * Dumps through texture_pack_debug_dump when OPENPETE_TEXPACK_DUMP_REGISTRY
 * is set. Joins the decode worker, frees every entry's pixels, clears the
 * pack table and the registry, and clears the initialized state so a later
 * texture_pack_init rescans. Takes no registry lock: the caller ensures no
 * thread is resolving or matching. */
void texture_pack_shutdown(void);

/** @brief Print one line per live registry entry, then a live/slot/generation summary, to stderr.
 *
 * texture_pack_shutdown calls it when OPENPETE_TEXPACK_DUMP_REGISTRY is
 * set. */
void texture_pack_debug_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TEXTURE_PACK_H */
