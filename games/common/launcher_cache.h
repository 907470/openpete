/* games/common/launcher_cache.h - one game's launcher asset cache: the
 * asset extraction job's outputs (launcher_assets.h) in the game's
 * directory under the "cache" state leaf, with the manifest that says
 * whether they are current.
 *
 * Layout, in openpete_paths_game_state_dir("cache", game):
 *
 *   domes.bin       "OPDOMES1", u32 count, then per dome: u8 name length,
 *                   the level id, u32 vertex count, u32 triangle count,
 *                   u32 sector count, the clear colour r, g, b, the
 *                   vertices (9 bytes each, game_asset_dome_t) and the
 *                   triangles (three u16 each); little-endian
 *   glyphs.txt      the glyph models (text_glyph_set_write)
 *   logo.rgba       "OPLOGO01", u32 width, u32 height, width * height RGBA
 *   preview.rgba    "OPPREV01", u32 width, u32 height, width * height RGBA:
 *                   the box picture (launcher_preview.h)
 *   manifest.toml   the format version, the content hash of the library
 *                   disc the outputs were read from, the build id that
 *                   wrote them, each output's name and size, and a failed
 *                   marker: the build id and disc hash of a failed run and
 *                   its reason; for the preview, the disc hash and the
 *                   "level|pose|WxH form" it was drawn from and its own failed
 *                   marker
 *
 * Every file is replaced atomically (op_atomic_file), the manifest last,
 * so a stopped or killed job leaves the previous outputs whole. The RHI
 * pipeline cache shares the leaf (cache/<file> beside cache/<game>/). */
#ifndef OPENPETE_COMMON_LAUNCHER_CACHE_H
#define OPENPETE_COMMON_LAUNCHER_CACHE_H

#include <stddef.h>

#include "openpete_thread_roles.h"

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_game_build;

/* The manifest's format version this build writes. A manifest of an older
 * version reruns the job; one of a newer version is treated as missing. */
#define LAUNCHER_CACHE_FORMAT 1

/* launcher_cache_update results. */
enum {
    LAUNCHER_CACHE_READY = 0,     /* current, or rewritten now */
    LAUNCHER_CACHE_SKIPPED = 1,   /* a failed marker of this build and disc: not rerun */
    LAUNCHER_CACHE_FAILED = -1,   /* the extraction failed; the failed marker is written */
    LAUNCHER_CACHE_STOPPED = -2,  /* stopped (openpete_launcher_job_cancelled); nothing written */
};

/** @brief Brings one game's launcher asset cache up to date for a library disc, rerunning the asset extraction when the cache is not current.
 *
 * The cache is current when its manifest exists in this build's format,
 * names the disc hash @p disc_sha256 and this build's id
 * (openpete_launcher_build_id), and every output it lists exists at its
 * recorded size. A failed marker naming this build and this disc hash skips
 * the job; one naming another build or disc reruns it. A rerun logs one
 * line naming the game, the job and the trigger, which starts with
 * "launch-time rerun" when @p context is "launch-time". The outputs are
 * written atomically and the manifest last; a deterministic extraction
 * failure (a decode, a hash mismatch, OPENPETE_ASSET_FAIL naming the game
 * or set to 1) writes the failed marker into the manifest and keeps the
 * previous outputs; a stop writes nothing. The harness variable
 * OPENPETE_ASSET_JOB_DELAY_MS holds the job that long before its writes,
 * stopping early when the job is asked to stop.
 * Then, on the launcher's device only (a headless run neither checks nor
 * logs it), the box picture: current when its file exists at its recorded
 * size and its disc hash and "level|pose|WxH form" match; a preview failed marker of
 * this build and disc skips it; otherwise it is rendered
 * (launcher_preview_render) and a rerun logs one line, "launch-time rerun:
 * preview render of <game>" at launch time. A deterministic failure writes
 * the preview's failed marker; a stop, the time bound or a device failure
 * writes nothing. The extraction outputs' build id does not key the preview.
 * The game must be installed.
 * @param b The installed game's build record.
 * @param disc_path The library entry's disc.
 * @param disc_sha256 The disc's content hash from the entry's meta.toml.
 * @param context What started the job: "launch-time", "ingest" or "pick".
 * @return A LAUNCHER_CACHE_* value. */
int launcher_cache_update(const struct openpete_game_build* b, const char* disc_path,
                          const char* disc_sha256, const char* context) OP_REQUIRES(ingest);

/** @brief Reads a game's title logo from its launcher asset cache (logo.rgba).
 *
 * The file's magic, width and height (each 1 to 1024) and its size are
 * checked; a missing or malformed file reads as none. Reads only, so it runs
 * on any thread; the job replaces the file atomically.
 * @param game The game id.
 * @param w Receives the width.
 * @param h Receives the height.
 * @return width * height RGBA texels, rows top to bottom, allocated with
 *         malloc for the caller to free; NULL when the cache holds none. */
unsigned char* launcher_cache_read_logo(const char* game, int* w, int* h);

/** @brief Reads a game's box picture from its launcher asset cache (preview.rgba) when the manifest lists it.
 *
 * None is read while the manifest holds a preview failed marker or lists a
 * size the file does not have; the file's magic, width and height (each 1
 * to 1024) and its size are checked. Reads only, so it runs on any thread.
 * @param game The game id.
 * @param w Receives the width.
 * @param h Receives the height.
 * @return width * height RGBA texels, rows top to bottom, allocated with
 *         malloc for the caller to free; NULL when the cache holds none. */
unsigned char* launcher_cache_read_preview(const char* game, int* w, int* h);

/** @brief Reports whether a game declares launcher assets (an asset descriptor in its host tables).
 *
 * @param b The game's build record.
 * @return 1 when it does, else 0. */
int launcher_cache_game_has_assets(const struct openpete_game_build* b);

/** @brief Reads one game's glyph models from its cache's glyphs.txt for the launcher's title, replacing the ones read before.
 *
 * Needs no install: the path names the game by its build record. Logs the
 * file read and its glyph count, or why nothing was read.
 * @param b The game's build record.
 * @return 0 when the models are ready, -1 when the file is missing or does not read. */
int launcher_cache_title_load(const struct openpete_game_build* b);

struct openpete_launcher_title;
struct openpete_launcher_title_glyph;

/** @brief Emits a string as one game's 3D title from the glyph models launcher_cache_title_load read (text_glyph_set_emit_title).
 *
 * The font's light terms and model height come from the game's own layout,
 * not the installed game's, so no install is needed.
 * @param b The game's build record.
 * @param text The string.
 * @param t The placement, in window pixels.
 * @param out The openpete_hud_vert buffer.
 * @param cap Its capacity, in vertices.
 * @param cap_h Receives the cap height.
 * @param glyphs Receives the posed glyphs, or NULL.
 * @param glyph_cap The records @p glyphs holds.
 * @return The vertex count, 0 when no models are loaded for the game. */
int launcher_cache_title_emit(const struct openpete_game_build* b, const char* text,
                              const struct openpete_launcher_title* t, void* out, int cap, float* cap_h,
                              struct openpete_launcher_title_glyph* glyphs, int glyph_cap);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_LAUNCHER_CACHE_H */
