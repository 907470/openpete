/* games/common/renderer/cut_guard.h — the sub-tick discontinuity guard the
 * re-extract bodies share: the camera-cut predicate, the hold verdict, the
 * census, and the whole camera bracket for a body that needs no more than the
 * linear lerp.
 *
 * A guard fires when the two ticks of the pair are not one motion: the camera
 * jumped or spun, or the two ticks came from different simulation regimes.
 * The interval between the two ticks shows tick N-1 and reaches tick N at
 * alpha 1, so a guard holds N-1 for every present below alpha 1 and lets N
 * show at alpha 1 — the discontinuity lands on the present where every
 * interpolated channel reaches its canonical N value. A guard with no previous
 * state to hold (no open interval, no staging arena, no prev snapshot) snaps
 * to N instead and the census counts the reason. */
#ifndef OPENPETE_GAME_CUT_GUARD_H
#define OPENPETE_GAME_CUT_GUARD_H

#include <stdint.h>

#include "openpete_interp.h"
#include "openpete_interp_objs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guard kinds, in census-tally order. */
enum { CUT_CAM = 0, CUT_CLASS = 1, CUT_TELEPORT = 2, CUT_KINDS = 3 };

/** @brief Whether the cut census is armed: OPENPETE_CUT_CENSUS set to a nonempty value not starting with '0', read once.
 *
 * It arms every stderr print below; the callers also gate
 * cut_locate_camera on it.
 * @return 1 when armed, else 0. */
int cut_census_on(void);

/** @brief Count one guard decision in the per-kind hold/snap tallies and, with the census armed, print it on one line.
 *
 * The line carries the kind, the object, the verdict and its reason, both
 * positions and their distance D, and the running tallies, which are the
 * guards' engagement counters. The tallies count whether or not the census
 * is armed, so they are complete from the first present. A kind outside
 * 0..CUT_KINDS-1 is ignored.
 * @param kind CUT_CAM, CUT_CLASS or CUT_TELEPORT.
 * @param addr The object's address; the camera guard passes its regime-edge
 *        flag instead.
 * @param held Nonzero when the guard held tick N-1, zero when it snapped.
 * @param why The verdict's reason.
 * @param prev_pos World position of tick N-1; NULL reads as the origin.
 * @param shown_pos World position shown; NULL reads as the origin. */
void cut_census_note(int kind, uint32_t addr, int held, const char* why,
                     const int32_t* prev_pos, const int32_t* shown_pos);

/** @brief Print the camera a present showed, at the alpha it showed it, when the census is armed.
 *
 * A snapped present writes no bracket trace, so this line is the only record
 * of what the interval drew. Nothing is printed when `pos` or `mat` is NULL.
 * @param pos The shown world position, int32 x/y/z.
 * @param mat The shown rotation; its first row is printed.
 * @param alpha The alpha the present drew at.
 * @param cam_cut Nonzero when the pair is a camera cut. */
void cut_census_camera(const int32_t* pos, const int16_t* mat,
                       double alpha, int cam_cut);

/* Where the camera's fields sit inside a game's camera transform block, and
 * which ring rows carry the transform and the simulation-regime word. A game
 * that registers no regime row passes gs_region < 0 and the regime test is
 * skipped. */
typedef struct cut_cam_desc {
    int      cam_region;     /* ring row index of the camera transform */
    int      gs_region;      /* ring row index of the regime word, or -1 */
    uint32_t pos_off;        /* int32 x/y/z world position */
    uint32_t mat_off;        /* the 3x3 int16 rotation the cut test reads */
} cut_cam_desc_t;

/* The measured inputs of one cut decision, for the callers' diagnostics. */
typedef struct cut_cam_metrics {
    double   d2;        /* squared position step, world units */
    double   frob;      /* row-dot sum of the two rotation matrices */
    uint32_t pgs, cgs;  /* the two regime words (0 when the game has no row) */
    int      gs_edge;   /* the pair crossed a regime edge */
} cut_cam_metrics_t;

/** @brief Decide whether two camera snapshots are a discontinuity rather than one motion.
 *
 * The pair is a cut when the position steps more than CUT_POS_LIMIT (5000
 * world units) in one tick ("pos"), when the row-dot sum of the two
 * rotations falls below one unit of 4096^2, a turn of more than 90 degrees
 * ("rot"), or when the two regime words differ ("gs-edge", reported only
 * when neither of the others fires). The position and rotation tests run
 * only when both camera pointers are non-NULL, the regime test only when
 * both regime pointers are.
 * @param pcam The previous tick's camera block.
 * @param ccam The current tick's camera block.
 * @param pos_off Offset of the int32 x/y/z position in the block.
 * @param mat_off Offset of the 3x3 int16 rotation in the block.
 * @param pgs The previous tick's regime word, or NULL.
 * @param cgs The current tick's regime word, or NULL.
 * @param out Receives the measured inputs when non-NULL, whatever the verdict.
 * @return "pos", "rot" or "gs-edge", or NULL when the pair is one motion. */
const char* cut_camera_reason(const uint8_t* pcam, const uint8_t* ccam,
                              uint32_t pos_off, uint32_t mat_off,
                              const void* pgs, const void* cgs,
                              cut_cam_metrics_t* out);

/* Whether an object's prev/curr pair IS two consecutive ticks: prev was last
 * observed by a capture within 1.5 ticks of curr's roll. A record that held
 * prev unchanged on every capture up to curr qualifies however long it held
 * it. An object whose prev was last observed earlier than that has no tick N-1
 * to show: a level load leaves pre-load records in the table, and a slot the
 * capture did not observe in between names a state the interval does not
 * span. */
int cut_pair_is_tick(const openpete_interp_obj_t* o);

/* Whether this present can show tick N-1 whole. A held cut shows the previous
 * tick on every channel, so every record the moby pass will draw needs a
 * previous tick of its own: a record that has none is drawn at tick N whatever
 * the camera does, and against a camera held one whole cut away it leaves the
 * frame. A record whose class changed is excluded — it names a reloaded model
 * table and carries no drawable previous state either way. */
int cut_scene_holdable(void);

/** @brief Whether the interval a camera cut ends still has a tick N-1 to show.
 *
 * The alpha is (openpete_interp_present_now() - curr_tick_ns) / period_ns,
 * or 1 when the period is not positive or the ring's head is stale (its
 * forward endpoint stopped advancing, so there is no open interval). The cut
 * holds while the alpha is below 1 and cut_scene_holdable() passes. The
 * verdict is noted in the census as kind CUT_CAM with reason "hold",
 * "scene" or "stale".
 * @param period_ns The pair's tick period.
 * @param curr_tick_ns The current snapshot's tick stamp.
 * @param gs_edge Nonzero when the pair crossed a regime edge; noted in the
 *        census.
 * @return 1 to hold tick N-1, 0 to show tick N. */
int cut_camera_hold(double period_ns, op_time_t curr_tick_ns, int gs_edge);

/** @brief Print a `locate=camcut` line when the ring pair at the present instant is a camera cut.
 *
 * The re-extract bodies call it once per canonical pass with the census
 * armed. The guard itself runs on intermediate presents, which a headless run
 * produces only where a capture asks for one, so this line names the ticks a
 * sub-tick capture must sample; it reports nothing about what was presented.
 * Nothing is printed when the ring has no pair or the game registered no
 * camera row.
 * @param desc The game's camera descriptor; NULL prints nothing. */
void cut_locate_camera(const cut_cam_desc_t* desc);

/* The result of one present's camera bracket. */
typedef struct cam_bracket {
    int      bracketed;        /* the staged camera carries an interpolated pair */
    int      cut;              /* the pair is a discontinuity */
    int      hold;             /* the interval is open and shows tick N-1 */
    double   alpha;            /* the alpha the present drew at (0 while held) */
    const uint8_t* tickcam;    /* curr's camera bytes, for a feeder that re-bases
                                * records composed against the tick's camera */
    double   fpos[3];          /* the un-rounded interpolated position */
} cam_bracket_t;

/** @brief Interpolate a snapshot pair's camera into `cam` and report what the present shows.
 *
 * `cam` is the staged camera copy; the sky matrix at `mat_off`, the world
 * matrix at `view_off` and the position at `pos_off` are written. The alpha
 * is the present instant's offset past curr's tick over the pair's cadence
 * (one nominal tick when the gap exceeds CUT_PAIR_STALE_TICKS ticks),
 * clamped to -0.35..1, and 1 when the ring's
 * head is stale. On a cut with an open interval (the pair at most
 * CUT_PAIR_STALE_TICKS old and cut_camera_hold passing) the bracket runs
 * with alpha pinned to 0, where the lerp is the previous state byte for
 * byte; on a cut with no open interval, or when `cam` or either snapshot's
 * camera row is missing, nothing is written and the caller presents the
 * live tick-N camera. Guest memory is never written.
 * @param cam The staged camera copy.
 * @param prev The previous snapshot.
 * @param curr The current snapshot.
 * @param desc The game's camera descriptor.
 * @param view_off Offset of the world matrix in the camera block.
 * @param out Receives the result when non-NULL. */
void cam_bracket_run(uint8_t* cam, const openpete_interp_snapshot_t* prev,
                     const openpete_interp_snapshot_t* curr,
                     const cut_cam_desc_t* desc, uint32_t view_off,
                     cam_bracket_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_CUT_GUARD_H */
