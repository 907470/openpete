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

/* OPENPETE_CUT_CENSUS=1 arms every print below. */
int cut_census_on(void);

/* One line per guard decision, with the running hold/snap tallies whose zero is
 * the engagement counter. `prev_pos` and `shown_pos` are world positions; NULL
 * reads as the origin and prints D = 0. Counted whether or not the census is
 * armed, so the tallies are complete from the first present. */
void cut_census_note(int kind, uint32_t addr, int held, const char* why,
                     const int32_t* prev_pos, const int32_t* shown_pos);

/* The camera each present showed, at the alpha it showed it: a snapped present
 * writes no bracket trace, so this is the only record of what the interval
 * drew. */
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

/* The cut predicate over two camera snapshots. Returns the reason
 * ("pos", "rot", "gs-edge") or NULL when the pair is one motion; `out` is
 * filled either way. `pgs`/`cgs` are the two regime words, or NULL. */
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

/* Whether the interval this cut ends still has a tick N-1 to show: true while
 * its alpha is below 1 and the scene can hold with the camera. A pair whose
 * forward endpoint stopped advancing has no open interval and shows the head.
 * Notes the verdict, with its reason, in the census. */
int cut_camera_hold(double period_ns, op_time_t curr_tick_ns, int gs_edge);

/* The ticks at which the camera guard fires, evaluated against the same ring
 * pair once per canonical pass. The guard itself runs on intermediate presents,
 * which a headless run produces only where a capture asks for one, so this is
 * what names the ticks a sub-tick capture must sample. Prints `locate=camcut`
 * and reports nothing about what was presented. */
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

/* Interpolate the pair into `cam` and report what the present shows. `cam` is
 * the staged camera copy; the sky matrix at `mat_off`, the world matrix at
 * `view_off` and the position at `pos_off` are written. On a cut with an open
 * interval the bracket still runs, with alpha pinned to 0, where the lerp is
 * the previous state byte for byte; on a cut with no open interval nothing is
 * written and the caller presents the live tick-N camera. Guest memory is never
 * written — `cam` is the view's copy. */
void cam_bracket_run(uint8_t* cam, const openpete_interp_snapshot_t* prev,
                     const openpete_interp_snapshot_t* curr,
                     const cut_cam_desc_t* desc, uint32_t view_off,
                     cam_bracket_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_CUT_GUARD_H */
