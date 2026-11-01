/* games/common/launcher_preview.h - the launcher's box picture: one image of
 * a chosen level at a chosen free-camera pose, read from a disc the caller
 * names (WAD.WAD alone, never guest RAM, never the running game) and drawn
 * once through the terrain cache's pipelines on the launcher's device.
 *
 * The job runs inside the `ingest` role with its game installed, as the
 * asset extraction does (launcher_assets.h); the draw itself is handed to
 * the launcher's display owner (openpete_native_launcher_preview_render). */
#ifndef OPENPETE_COMMON_LAUNCHER_PREVIEW_H
#define OPENPETE_COMMON_LAUNCHER_PREVIEW_H

#include <stddef.h>
#include <stdint.h>

#include "openpete_thread_roles.h"

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_game_build;
struct disc_image;
struct terrain_host_level;

/* The preview's target width: the game's 512-wide screen on a 4:3 display
 * 640 wide. The height is the game's (launcher_preview_size). */
#define LAUNCHER_PREVIEW_W 640u
/* The rendering form, part of the cache's view key, so a build that draws
 * the picture differently redraws a cached one: extended LOD at the game's
 * displayed aspect. */
#define LAUNCHER_PREVIEW_FORM "extended-lod"

/* The time bound of one preview render, from its start: a render past it is
 * a transient job failure (no failed marker, rerun at the next launch). The
 * render measured at most 1.8 s on a software device, the terrain
 * pipelines' first compile included. */
#define LAUNCHER_PREVIEW_BOUND_MS 10000

/* Which level and pose a game's box picture shows. */
typedef struct launcher_preview_spec {
    const char* game;    /* the game id */
    const char* level;   /* the level table's name of the level */
    const char* pose;    /* the --freecam-pose text */
} launcher_preview_spec;

/* A host-loaded level: the terrain bytes the walk's decoders read, the
 * sector records' offsets and the texture tables, and the level's VRAM
 * image placed where the game's level load puts it. */
typedef struct launcher_preview_level {
    uint8_t*  bytes;
    uint32_t  len;
    uint32_t* sector_off;
    int       sector_count;
    uint32_t  lq_off, hq_off, tex_count;
    uint16_t* vram;      /* 1024 x 512 PSX pixels */
    uint8_t*  exe;       /* the boot executable's text */
    uint32_t  exe_addr, exe_len;
    int       entry;     /* the WAD entry read */
} launcher_preview_level;

/* A level's sky dome as the asset extraction stores it (domes.bin,
 * game_asset_dome_t): 9-byte vertices (the unit direction x, -y, z of the
 * decoded sky vertex as int16 scaled by 32767, then r, g, b), uint16
 * triangle indices, and the level's clear colour. */
typedef struct launcher_preview_dome {
    uint8_t         bg[3];
    uint32_t        nv, ntri;
    const uint8_t*  verts;
    const uint16_t* tris;
} launcher_preview_dome;

/** @brief Returns a game's box picture level and pose, or NULL for a game that has none.
 *
 * The harness variable OPENPETE_PREVIEW_VIEW ("<game>=<level>|<pose>",
 * rows separated by ';') replaces a game's row, so a pose is tried without
 * a rebuild; the cache keys the preview on the level, the pose, the size and the form, so a changed
 * row redraws it.
 * @param game The game id.
 * @return The row of the one per-game table, or the variable's row. */
const launcher_preview_spec* launcher_preview_spec_of(const char* game);

/** @brief Returns a game's box picture size: LAUNCHER_PREVIEW_W wide and two lines per displayed row high, the game's displayed frame at its own aspect.
 *
 * A 4:3 display shows the game's 240-row frame as 480 lines, two per row;
 * the game displays fewer rows (224 on Spyro 1, 216 on Spyro 2 and 3),
 * which a display shows at 448 and 432 lines inside its 480.
 * @param game The game id.
 * @param w Receives the width.
 * @param h Receives the height.
 * @return 0, or -1 for a game without a box picture. */
int launcher_preview_size(const char* game, uint32_t* w, uint32_t* h);

/** @brief Reads a level's terrain bytes, sector table, texture tables and VRAM image from a disc into host buffers.
 *
 * Spyro 1: the level's WAD entry (level header: VRAM image at +0x00, level
 * data at +0x08) holds the texture component {size, count, LQ entries, HQ
 * records} and then the environment component {size, count, offsets
 * relative to its second word}. Spyro 2/3: sub-file 1 of the slot table's
 * WAD entry holds the texture block {size, count, 48-byte entries} and the
 * sector block {size, word, count, offsets relative to its second word};
 * sub-file 0 opens with the VRAM image. The VRAM image is 512 x 512 pixels
 * placed at x 512, rows 0-511, as the game's level load places it. The boot
 * executable's text is read too: the walk reads static tables at guest
 * addresses. The game must be installed.
 * @param img The disc.
 * @param b The installed game's build record.
 * @param number The level number.
 * @param out Receives the level; released with launcher_preview_level_free.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1 when a read or a layout check failed. */
int launcher_preview_level_load(struct disc_image* img, const struct openpete_game_build* b, int number,
                                launcher_preview_level* out, char* why, size_t cap);

/** @brief Releases a level launcher_preview_level_load read; a zeroed level is a no-op.
 *
 * @param L The level, zeroed after. */
void launcher_preview_level_free(launcher_preview_level* L);

/** @brief Renders a game's box picture from a disc: reads its level, builds the preview with the walk's decoders and chunk packer and has the launcher's display owner draw it.
 *
 * The game must be installed. The harness variables OPENPETE_PREVIEW_FAIL
 * (1 or the game id: a deterministic failure), OPENPETE_PREVIEW_DELAY_MS
 * (holds the job before its draw, stopping early on a stop) and
 * OPENPETE_PREVIEW_BOUND_MS (replaces LAUNCHER_PREVIEW_BOUND_MS) drive the
 * flow's cases.
 * @param b The installed game's build record.
 * @param disc_path The disc.
 * @param dome The level's sky dome, drawn behind the terrain; its clear colour fills the rest.
 * @param rgba Receives the launcher_preview_size image, 4 bytes a pixel, which the caller frees.
 * @param ms Receives the render's milliseconds.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return An OPENPETE_PREVIEW_* value. */
int launcher_preview_render(const struct openpete_game_build* b, const char* disc_path,
                            const launcher_preview_dome* dome,
                            uint8_t** rgba, double* ms, char* why, size_t cap) OP_REQUIRES(ingest);

/** @brief Builds a game's box picture without drawing it: the level read and the preview built and freed, the harness form OPENPETE_PREVIEW_BUILD_ONLY runs on a headless launch.
 *
 * Proves the build leaves no state the game reads: a run with it and one
 * without capture the same images.
 * @param b The installed game's build record.
 * @param disc_path The disc.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1. */
int launcher_preview_build_only(const struct openpete_game_build* b, const char* disc_path, char* why,
                                size_t cap) OP_REQUIRES(ingest);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_LAUNCHER_PREVIEW_H */
