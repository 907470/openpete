/* openpete_tile_census.h - per-tile engagement counters of a presentation capture.
 *
 * A tally counts the triangles of one presented scene by provenance-tag
 * family and kind, category, blend mode, triangle flag, class bit
 * (openpete_scene_tri.cbits) and moby class, plus the retained
 * terrain-cache sectors the scene's state draws. The renderer
 * fills tallies from its private scene records; this module turns a tally
 * into named counters (core/diag/tile_census.def lists every name and what
 * it counts) and writes them to the sidecar file beside a capture.
 *
 * Counting selects no code path: the tally reads a scene the compose draws
 * anyway. OPENPETE_TILE_CENSUS gates only whether a capture records its
 * tally and writes the sidecar.
 *
 * Host-side only; nothing here reads or writes PSX RAM.
 */
#ifndef OPENPETE_TILE_CENSUS_H
#define OPENPETE_TILE_CENSUS_H

#include <stdint.h>
#include "openpete_native_render.h"   /* OPENPETE_TAG_KINDS */

#ifdef __cplusplus
extern "C" {
#endif

#define TC_FAMILIES   16   /* tag bits 31:28 */
#define TC_CATS       16   /* OPENPETE_SCENE_* 0..14; 15 holds any other value */
#define TC_BLENDS     8    /* blend 0..6; 7 holds any other value */
#define TC_MOBY_TYPES 512  /* distinct moby classes one tally holds */
#define TC_CBITS      32   /* class bits of openpete_scene_tri.cbits */

/* Per-triangle flags the tally counts in triangles. */
enum {
    TC_FLAG_SHELL, TC_FLAG_KEY_ORDER, TC_FLAG_KFOLD, TC_FLAG_PORTAL_RIM,
    TC_FLAG_HOST_ADDED, TC_FLAG_FOG_DELTA, TC_FLAGS
};
/* 2D flags the tally counts in primitives (runs of consecutive flagged
 * triangles sharing one primitive address). */
enum { TC_PRIM_OVER_FADE, TC_PRIM_UNDER_PLAYER, TC_PRIM_CARRY, TC_PRIMS };

/* One scene triangle as the tally reads it. */
typedef struct {
    uint32_t tag;       /* provenance tag */
    uint32_t prim;      /* PSX primitive address, 0 for none */
    int      category;  /* OPENPETE_SCENE_* */
    int      blend;     /* blend mode */
    unsigned flags;     /* bit TC_FLAG_* */
    unsigned prims;     /* bit TC_PRIM_* */
    uint32_t cbits;     /* class bits (OPENPETE_CB_*) */
} tc_tri_t;

/* The counts of one scene. Plain data: copied by assignment. */
typedef struct {
    uint32_t fk[TC_FAMILIES][OPENPETE_TAG_KINDS];
    uint32_t cat[TC_CATS];
    uint32_t blend[TC_BLENDS];
    uint32_t flag[TC_FLAGS];
    uint32_t prim[TC_PRIMS];
    uint32_t cbit[TC_CBITS];       /* triangles carrying each class bit */
    uint32_t prim_last[TC_PRIMS];  /* run state: primitive of the previous triangle */
    uint8_t  prim_open[TC_PRIMS];  /* run state: the previous triangle carried the flag */
    uint16_t mcls[TC_MOBY_TYPES];  /* moby class + 1 per hash slot, 0 empty */
    uint32_t mcnt[TC_MOBY_TYPES];
    int      sec[4];               /* retained sectors: drawn, hp, lp, near-variant */
    int      vram;                 /* the compose shows the VRAM framebuffer */
    int      overflow;             /* more moby classes than TC_MOBY_TYPES */
} tc_tally_t;

/** @brief Clear a tally to zero counts.
 * @param t The tally. */
void tc_tally_clear(tc_tally_t *t);
/** @brief Add one triangle to a tally.
 *
 * Counts it under its tag family and kind, category, blend, flags and each
 * class bit it carries; a moby-family triangle also under its class (tag
 * bits 15:0). A 2D flag adds
 * a primitive when the previous triangle did not carry the flag or names
 * another primitive address; a flagged triangle with address 0 counts alone.
 * @param t The tally.
 * @param tri The triangle. */
void tc_tally_tri(tc_tally_t *t, const tc_tri_t *tri);

/** @brief Register the render-path mapping of tag families, which the game layer owns.
 *
 * @param path_of Returns the path index of a family and kind, or of the
 *        family itself for kind -1; -1 when none.
 * @param names Path names indexed by path.
 * @param count Number of names. */
void openpete_native_register_tag_paths(int (*path_of)(int family, int kind),
                                        const char *const *names, int count);

/** @brief Whether this run records tile tallies and writes sidecars (OPENPETE_TILE_CENSUS).
 *
 * Boot-latched: the variable is read on the first call only. Called only by
 * the renderer's two capture helpers (scripts/fences/tile_census_accessor.py).
 * @return 1 when the variable is set to anything but "0". */
int tile_census_on(void);

/** @brief Begin a sidecar: forget every counter of the previous one. */
void tc_sidecar_begin(void);
/** @brief Add a tally's counters to the sidecar, each name under a prefix.
 * @param t The tally.
 * @param prefix "" for the tile's own scene, "backdrop." for its frozen backdrop. */
void tc_sidecar_add_tally(const tc_tally_t *t, const char *prefix);
/** @brief Add one value to a named counter of the sidecar.
 *
 * A name that does not fit the counter table marks the sidecar failed.
 * @param name Counter name.
 * @param v Value to add. */
void tc_sidecar_add(const char *name, uint32_t v);
/** @brief Mark the sidecar failed with a reason; tc_sidecar_write then writes nothing.
 * @param why The reason, logged at the write. */
void tc_sidecar_fail(const char *why);
/** @brief Write the sidecar beside a capture as `# gen G tree T` then sorted nonzero `name<TAB>value` lines.
 *
 * Writes through a temporary file and a rename; writes nothing when the
 * sidecar was marked failed, and logs the reason.
 * @param path The sidecar path.
 * @return 0 when written, -1 when refused or the write failed. */
int tc_sidecar_write(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_TILE_CENSUS_H */
