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
 * (games/common/renderer/terrain.c) does the projection-space pick and
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

/** @brief Report whether the inspector is engaged.
 *
 * Engaged, the terrain feeder runs the crosshair pick, the verdict tint
 * and (with preview) the candidate draw; disengaged none of them run.
 * Set by the P key, the overlay knob, and OPENPETE_FILLPICK=1 once a
 * Spyro 1 level is live.
 * @return 1 when engaged, else 0. */
int  openpete_fillpick_active(void);

/** @brief Flip the engaged state of the inspector.
 *
 * Equivalent to openpete_fillpick_set_active on the negated state. */
void openpete_fillpick_toggle(void);

/** @brief Engage or disengage the inspector.
 *
 * A change clears the stored hit, logs the new state and bumps the
 * generation counter (preview draws only while engaged); setting the
 * state it already has does nothing. Held session verdicts survive a
 * disengage.
 * @param on Nonzero to engage. */
void openpete_fillpick_set_active(int on);

/** @brief Report whether preview is in effect: the preview mode is on and the inspector is engaged.
 *
 * Preview draws every LP face, not only the tagged ones, and tints the
 * untagged ones separately; an under-fill site is found this way, since
 * the face that would cover the hole is not drawn until it is tagged.
 * @return 1 when preview is on and the inspector is engaged, else 0. */
int  openpete_fillpick_preview(void);

/** @brief Turn the preview mode on or off.
 *
 * The mode is kept while the inspector is disengaged and takes effect on
 * the next engage. A change bumps the generation counter and logs; setting
 * the mode it already has does nothing.
 * @param on Nonzero for preview. */
void openpete_fillpick_set_preview(int on);

/** @brief Return the generation counter the terrain feeder's cached sector fill tags key on.
 *
 * Bumped by openpete_fillpick_mark, openpete_fillpick_undo, a change of the
 * preview mode, and a change of the engaged state (preview draws only while
 * engaged), so the cached tags re-derive on the next walk.
 * @return The current generation; starts at 1. */
unsigned openpete_fillpick_generation(void);

/** @brief Store the crosshair pick for the overlay and the verdict keys.
 *
 * The terrain feeder calls it once per walk while the inspector is
 * engaged. A NULL hit, or one with valid 0, means nothing is under the
 * crosshair. Engaging or disengaging the inspector clears the stored hit.
 * @param h Pick to copy, or NULL. */
void openpete_fillpick_publish(const openpete_fillpick_hit* h);

/** @brief Copy out the stored crosshair pick.
 * @param out Receives the pick (all zero when none is stored); may be
 *        NULL.
 * @return 1 when a face is picked (the stored hit's valid field), else
 *         0. */
int  openpete_fillpick_get(openpete_fillpick_hit* out);

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

/** @brief Step back the most recent record and rewrite the session file.
 *
 * Pops the tail of the record order. A record that replaced a verdict
 * gets that verdict back, returns to the tail, and loses its undo target,
 * so a second undo on it removes it; a record that replaced nothing is
 * removed. Warns and does nothing when there is no record. */
void openpete_fillpick_undo(void);

/** @brief Return the number of session records, cleared ones included.
 *
 * The overlay lists them through openpete_fillpick_entry, indices 0 to
 * count - 1 in record order.
 * @return Record count, at most 1024. */
int  openpete_fillpick_count(void);

/** @brief Read one session record by position in the record order.
 *
 * Any output pointer may be NULL. The name pointer refers to the record's
 * storage and is valid until the next mark or undo.
 * @param idx Position, 0 for the oldest record.
 * @param level Receives g_LevelId.
 * @param level_name Receives the verdicts-file spelling of the level.
 * @param sec Receives the LP sector index.
 * @param face Receives the LP face index.
 * @param verdict Receives OPENPETE_FILLPICK_FILL, OPENPETE_FILLPICK_NOFILL
 *        or OPENPETE_FILLPICK_NONE for a cleared record.
 * @return 1 when @p idx is in range, else 0 with no output written. */
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
