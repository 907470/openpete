/* games/common/launcher_assets.h - the launcher's asset extraction job: every
 * level's sky dome, the 3D glyph models and the title logo, read from a disc
 * the caller names, never from guest RAM and never from the booted disc.
 *
 * The job runs with its game installed (openpete_game_build_install), so the
 * shared decoders read that game's game_layout and portal_game_s23 as they
 * do in game; it writes only the result it is handed and calls no function
 * that requires `sim`. Each decoder's input bytes are hashed and checked
 * against a known disc's hashes (the one the image's content hash names,
 * else the game's first); a hash that differs fails the job, since a decode
 * at wrong offsets returns geometry, not a failure. An image of another
 * revision fails; an image whose asset inputs equal a known disc's passes,
 * whatever its other bytes. */
#ifndef OPENPETE_COMMON_LAUNCHER_ASSETS_H
#define OPENPETE_COMMON_LAUNCHER_ASSETS_H

#include <stddef.h>
#include <stdint.h>

#include "openpete_thread_roles.h"
#include "features/level_table.h"

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_game_build;
struct text_glyph_set;

/* How a level's sky block is found on the disc. */
enum {
    GAME_SKY_NONE = 0,
    /* Spyro 1: WAD entry sky_wad_base + 2 * index holds the level, index
     * (world - 1) * 6 + (number % 10) for level number world * 10 + n; its
     * first sector is the level header, whose words +0x08/+0x0C give the
     * level data's offset and size in the entry; the data opens with
     * sky_components size-prefixed components and the next is the sky
     * block {size, payload}. */
    GAME_SKY_LEVEL_HEADER = 1,
    /* Spyro 2/3: the level's slot is the byte at the level number of the
     * slot table portal_game_s23.level_slots, read from the boot executable
     * on the disc; sub-file 1 of WAD entry portal_game_s23.level_wad_base +
     * 2 * slot holds the block chain sky_walk follows to the sky payload. */
    GAME_SKY_SLOT_CHAIN = 2,
};

/* The hashes of one known disc's asset inputs: FNV-1a 64 of each level's
 * sky payload in level-table order, of the glyph font bytes read, and of
 * the logo's texel rows followed by its palette. */
typedef struct game_asset_hashes {
    const char*     disc_sha256;   /* the known disc's content hash, 64 lowercase hex digits */
    const uint64_t* sky;
    int             sky_count;
    uint64_t        glyphs;
    uint64_t        logo;
} game_asset_hashes_t;

/* Where one game's launcher assets lie on its disc. */
typedef struct game_assets_desc {
    const game_level_table_t* levels;   /* one dome per row */
    int      sky_form;                  /* GAME_SKY_* */
    int      sky_wad_base;              /* GAME_SKY_LEVEL_HEADER */
    int      sky_components;            /* GAME_SKY_LEVEL_HEADER */
    /* GAME_SKY_SLOT_CHAIN: the loader's block walk over sub-file 1
     * (portal_sky_s23_walk); returns the payload's offset, 0 on failure. */
    uint32_t (*sky_walk)(const uint8_t* b, uint32_t n, uint32_t* len);
    /* The title logo: an 8-bit image with 1024-byte rows in WAD entry
     * `entry` (its sub-file `subfile`, or the entry itself when -1), the
     * drawn region's first texel at `texels`, its 256-colour 15-bit palette
     * at `palette`. */
    struct {
        int      entry, subfile;
        uint32_t texels, palette;
        uint32_t w, h;
    } logo;
    const game_asset_hashes_t* discs;
    int                        disc_count;
} game_assets_desc_t;

/* One level's sky dome. Vertices are 9 bytes each: the unit direction of
 * the decoded vertex (x, -y, z: y up) as three int16 scaled by 32767, then
 * its colour as r, g, b bytes; a vertex is one (position, colour) pair, in
 * order of first use. Triangles are three uint16 vertex indices. */
typedef struct game_asset_dome {
    const char* level;      /* the level table row's id */
    uint32_t    nv, ntri;
    uint8_t*    verts;
    uint16_t*   tris;
    uint8_t     bg[3];      /* the level's clear colour, r, g, b */
    uint32_t    sectors;
} game_asset_dome_t;

/* The job's result: owned by the caller, released by game_assets_free. */
typedef struct game_assets {
    game_asset_dome_t*     domes;
    int                    dome_count;
    struct text_glyph_set* glyphs;
    uint8_t*               logo_rgba;   /* w * h * 4 bytes, index 0 and colour 0 transparent; cropped to the opaque texels */
    uint32_t               logo_w, logo_h;
} game_assets_t;

/** @brief Extracts one game's launcher assets from a disc: every level's dome, the glyph models and the title logo.
 *
 * The game must be installed. The disc is opened on @p disc_path; nothing is
 * read from guest RAM or from the booted disc. Each input's hash is checked
 * against the hashes of the known disc @p disc_sha256 names, else the
 * game's first known disc. Stops between
 * levels when the calling job is asked to stop
 * (openpete_launcher_job_cancelled).
 * @param b The installed game's build record.
 * @param disc_path The disc (a cue or the image).
 * @param disc_sha256 The disc's content hash, as the library recorded it.
 * @param out Receives the result; zeroed first, released with game_assets_free.
 * @param why Receives the failure's reason.
 * @param why_cap Its capacity.
 * @return 0; -1 on a deterministic failure (an unreadable disc, a failed
 *         decode, a game with no known disc or a hash mismatch); -2 when stopped. */
int game_assets_extract(const struct openpete_game_build* b, const char* disc_path,
                        const char* disc_sha256, game_assets_t* out, char* why,
                        size_t why_cap) OP_REQUIRES(ingest);

struct disc_image;

/** @brief Resolves the WAD entry that holds a level: Spyro 1's level entry by level number, Spyro 2/3's through the slot table read from the boot executable.
 *
 * The entry is the one the sky payload is read from (GAME_SKY_LEVEL_HEADER,
 * GAME_SKY_SLOT_CHAIN); the game must be installed.
 * @param img The disc.
 * @param A The game's asset descriptor.
 * @param number The level number.
 * @param entry Receives the WAD entry index.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1. */
int game_assets_level_entry(struct disc_image* img, const game_assets_desc_t* A, int number, int* entry,
                            char* why, size_t cap);

/** @brief Reads the boot executable's text (the image its PSX-EXE header loads, static data included) from a disc.
 *
 * @param img The disc.
 * @param addr Receives the load address.
 * @param len Receives the byte count.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return The bytes, which the caller frees, or NULL. */
uint8_t* game_assets_boot_exe(struct disc_image* img, uint32_t* addr, uint32_t* len, char* why, size_t cap);

/** @brief Releases a result of game_assets_extract; a zeroed result is a no-op.
 *
 * @param a The result, zeroed after. */
void game_assets_free(game_assets_t* a);

/** @brief Runs the extraction job on the calling thread inside the `ingest` role, the headless runner's form.
 *
 * The game must be installed. Logs the outcome with the counts; when
 * @p dump_dir is non-NULL, writes the result there (game_assets_write).
 * @param b The installed game's build record.
 * @param disc_path The disc.
 * @param disc_sha256 Its content hash.
 * @param dump_dir A directory for the result, or NULL.
 * @return game_assets_extract's status. */
int game_assets_run_inline(const struct openpete_game_build* b, const char* disc_path,
                           const char* disc_sha256, const char* dump_dir);

/** @brief Writes a result into a directory: `<game>.dome.<level>.v`, `.i` and `.bg` per dome, `<game>.glyphs.txt`, and `<game>.logo.rgba` with `<game>.logo.txt` holding its width and height.
 *
 * @param game The game id naming the files.
 * @param a The result.
 * @param dir The directory, which must exist.
 * @return 0, or -1 when a file could not be written. */
int game_assets_write(const char* game, const game_assets_t* a, const char* dir);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_LAUNCHER_ASSETS_H */
