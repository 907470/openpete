#pragma once
/* openpete_fillpick.h — host-side state for the LP fill-tag inspector (P key).
 *
 * The frozen fill-face list (tests/freecam/fill-tags.verdicts ->
 * games/spyro-1/renderer/terrain_fill_tags.h) decides which LP faces may
 * back-fill sightlines no authored HP covers. Auditing it needs three things
 * the sweep that produced it cannot give: point at a face and learn its
 * identity, learn whether authored HP actually covers that sightline, and flip
 * its verdict and see the result immediately.
 *
 * This is the GENERIC half — the on/off + preview toggles, the hit published
 * by the renderer, the session verdict overrides and their file. It carries no
 * game addresses and no knowledge of the tag table; the terrain feeder
 * (games/spyro-1/renderer/terrain.c) does the projection-space pick and
 * consults openpete_fillpick_lookup ahead of the frozen table.
 *
 * Debug tool — inert unless engaged. Display-only: it changes which LP faces
 * the native renderer draws and how they are tinted, and writes no
 * gameplay-readable RAM.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Session verdicts. FILL/NOFILL mirror the verdicts file; NONE means "no
 * override — defer to the frozen table". */
#define OPENPETE_FILLPICK_NONE    (-1)
#define OPENPETE_FILLPICK_NOFILL  0
#define OPENPETE_FILLPICK_FILL    1

/* One present's crosshair pick, published by the terrain walk. The LP half is
 * the fill-tag candidate; the HP half answers the coverage question the FILL
 * verdict turns on ("does authored HP cover this sightline?"). */
typedef struct openpete_fillpick_hit {
    int      valid;        /* an LP face lies under the crosshair */
    int      level;        /* g_LevelId (the fill-tag table key) */
    char     level_name[24]; /* verdicts-file spelling, e.g. "artisans" */
    int      sec, face;    /* LP face identity — the rest of the table key */
    int      verdict;      /* effective verdict: FILL / NOFILL */
    int      overridden;   /* verdict comes from this session, not the frozen table */
    int      quad;
    double   vz;           /* LP surface view-Z at the crosshair (world/16) */
    int      hp_valid;     /* an HP face lies under the crosshair too */
    int      hp_sec, hp_face;
    double   hp_vz;        /* HP surface view-Z at the crosshair (world/16) */
} openpete_fillpick_hit;

/* Engaged state. The inspector arms the pick, the tint and (with preview) the
 * candidate draw; disengaged it costs nothing. */
int  openpete_fillpick_active(void);
void openpete_fillpick_toggle(void);
void openpete_fillpick_set_active(int on);

/* Preview: draw every LP face, not just the tagged ones, tinting the untagged
 * ones separately. This is how an under-fill site is found — the face that
 * *would* cover the hole is not drawn until it is tagged. */
int  openpete_fillpick_preview(void);
void openpete_fillpick_set_preview(int on);

/** @brief Return the generation counter the terrain feeder's cached sector fill tags key on.
 *
 * Bumped by openpete_fillpick_mark, openpete_fillpick_undo, a change of the
 * preview mode, and a change of the engaged state (preview draws only while
 * engaged), so the cached tags re-derive on the next walk.
 * @return The current generation; starts at 1. */
unsigned openpete_fillpick_generation(void);

/* Renderer -> UI. Publish once per present; pass NULL (or a hit with
 * valid == 0) when nothing is under the crosshair. */
void openpete_fillpick_publish(const openpete_fillpick_hit* h);
int  openpete_fillpick_get(openpete_fillpick_hit* out);   /* 1 = a face is picked */

/** @brief Return the in-memory verdict override recorded for an LP face.
 *
 * The terrain feeder's tag lookup consults it ahead of the frozen fill-tag
 * table. A record cleared with OPENPETE_FILLPICK_NONE is held in memory
 * and returns OPENPETE_FILLPICK_NONE, as does a face with no record.
 * @param level g_LevelId.
 * @param sec LP sector index.
 * @param face LP face index within the sector.
 * @return OPENPETE_FILLPICK_FILL, OPENPETE_FILLPICK_NOFILL or OPENPETE_FILLPICK_NONE. */
int  openpete_fillpick_lookup(int level, int sec, int face);

/** @brief Record or clear the verdict for the face under the crosshair and rewrite the session file.
 *
 * FILL or NOFILL creates the face's record or overwrites it, keeping the
 * replaced verdict as the undo target; OPENPETE_FILLPICK_NONE marks an
 * existing record cleared (it stays in memory and is omitted from the
 * file). The record moves to the tail of the record order that
 * openpete_fillpick_undo pops. Does nothing, with a warning, when no face
 * is picked or when a new record would exceed the 1024-entry table;
 * clearing a face with no record does nothing.
 * @param verdict OPENPETE_FILLPICK_FILL, OPENPETE_FILLPICK_NOFILL or OPENPETE_FILLPICK_NONE. */
void openpete_fillpick_mark(int verdict);

/* Drop the most recent record (restoring whatever it replaced). */
void openpete_fillpick_undo(void);

/* Session listing, for the overlay. */
int  openpete_fillpick_count(void);
int  openpete_fillpick_entry(int idx, int* level, const char** level_name,
                             int* sec, int* face, int* verdict);

/** @brief Return the path the session file is written to.
 *
 * OPENPETE_FILLPICK_OUT when set and non-empty, else "fill-tags.session"
 * in the working directory. The value is read on the first call and
 * latched for the process.
 * @return The session file path. */
const char* openpete_fillpick_session_path(void);

/** @brief Rewrite the session file from the in-memory records.
 *
 * Writes a header naming scripts/fill-tags-apply.py, then one
 * "<verdict> <level name> <sector> <face>" line per FILL or NOFILL record
 * in record order; cleared records are omitted. Called by
 * openpete_fillpick_mark and openpete_fillpick_undo; no other caller.
 * @return 1 on success, 0 when the file cannot be opened for writing. */
int  openpete_fillpick_save(void);

#ifdef __cplusplus
}
#endif
