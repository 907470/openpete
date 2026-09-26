#ifndef OPENPETE_GLITCH_MARK_H
#define OPENPETE_GLITCH_MARK_H

/*
 * The glitch mark — a ring of the last N presents and the last N terrain
 * walks, dumped to a text file on demand.
 *
 * A one-present artifact lasts 2 to 20 ms on a 480 Hz display and is gone
 * before anything can be started to catch it. The ring runs always, costs
 * one struct store per present and per walk, and the mark writes out what
 * is already in it, so the capture is taken AFTER the artifact was seen.
 *
 * Two rings, because the two clocks differ. A present record is one trip
 * through the native compose: it carries the scene the compose put on
 * screen and the walk census of the scene it composed, so a present whose
 * terrain differs from its neighbours is readable as one row. A walk record
 * is one terrain walk, canonical or sub-tick, whether or not a present ever
 * composed it; headless runs never compose, so the walk ring is the only
 * one that fills there.
 *
 * Threading. The present ring has exactly one writer, whichever thread owns
 * the display (the present thread while eager production is active, the
 * host loop otherwise), because the compose it is fed from runs under that
 * ownership. The walk ring has exactly one writer, the thread executing the
 * guest. Neither writer takes a lock: each record is published with a seq
 * counter raised before the fields are written and again after, so the
 * reader detects a record written under it and retries or skips that one
 * record. The mark itself is a request flag; the file is written by the
 * host loop, which is the thread allowed to block for file IO.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Terrain-walk census. Every field is read from a counter the walk already
 * maintains; nothing here is computed for the ring. */
typedef struct {
    uint32_t ord;          /* walk ordinal, 1-based; 0 = no walk recorded    */
    uint32_t tick;         /* openpete_frame at the walk                     */
    int32_t  sectors;      /* sectors the walk emitted geometry for          */
    int32_t  tris;         /* terrain triangles the walk produced            */
    int32_t  near_faces;   /* faces routed to the near/HQ tier               */
    int32_t  near_tris;    /* triangles those faces produced                 */
    int32_t  occl_culled;  /* sectors the occlusion grid rejected this walk  */
    int32_t  sub;          /* 1 = sub-tick re-extract, 0 = canonical walk    */
} openpete_glitch_walk_t;

/* One present. `kind` and `shows` use the present census's own vocabulary. */
typedef struct {
    uint32_t serial;       /* present ordinal, 1-based                       */
    uint32_t tick;         /* openpete_frame the composed scene was built at */
    uint32_t stamp;        /* terrain-cache build stamp of that scene        */
    float    alpha;        /* sub-tick alpha the scene was composed for      */
    int32_t  kind;         /* 0 canonical, 1 intermediate, 2 inline          */
    int32_t  paired;
    int32_t  shows;        /* 0 scene, 1 vram-fb, 2 psycross-ref, 3 clear    */
    int32_t  grid_age;     /* presents since the occlusion grid was captured */
    int32_t  tris;         /* triangles the compose drew                     */
    int32_t  tercache;     /* retained terrain sectors drawn                 */
    int32_t  cam_valid;
    float    cam_pos[3];   /* camera position, full-world units              */
    float    cam_M[9];     /* camera matrix, rows unit-normalized, row-major */
    openpete_glitch_walk_t walk;   /* census of the walk this scene came from */
} openpete_glitch_present_t;

/** @brief Allocate both rings and parse OPENPETE_GLITCH_MARK_AT.
 *
 * Called from openpete_run before any other thread exists, so the note
 * paths never test for initialization under a lock; later calls do
 * nothing. The depth comes from OPENPETE_GLITCH_MARK_N (default 600, about
 * 1.25 s of presents at 480 Hz), clamped to [16, 65536]; a value of 0 or
 * less allocates nothing and leaves the module off, as does an allocation
 * failure. The wall-clock origin for the milliseconds trigger is taken
 * here. An unparsable trigger is logged and ignored. */
void openpete_glitch_mark_init(void);

/** @brief Return the ring depth allocated.
 *
 * @return Records per ring; 0 when the module is off. */
int  openpete_glitch_mark_depth(void);

/** @brief Push one walk census into the walk ring and hold it as the most recent walk.
 *
 * Called by the terrain walk on the guest thread, the ring's only
 * writer. The stored record's ord is overwritten with the walk ordinal,
 * 1-based. The held copy is what openpete_glitch_mark_last_walk returns,
 * which the scene publish copies into its slot so the present that
 * composes that scene carries its census under eager production. No
 * effect when the module is off or @p w is NULL.
 * @param w Walk census. */
void openpete_glitch_mark_note_walk(const openpete_glitch_walk_t *w);

/** @brief Copy the most recently completed walk census.
 *
 * Retries up to four times while the guest thread is writing it; when
 * every try collides, or the module is off, @p out is left zeroed (ord 0
 * means no walk). Called by the scene publish and by the present census
 * when no scene slot is current. No effect when @p out is NULL.
 * @param out Receives the census. */
void openpete_glitch_mark_last_walk(openpete_glitch_walk_t *out);

/** @brief Push one present record into the present ring.
 *
 * Called by the native renderer's present census on the display owner's
 * thread, the ring's only writer. The record is stored as given; its
 * serial is the caller's. No effect when the module is off or @p p is
 * NULL.
 * @param p Present record. */
void openpete_glitch_mark_note_present(const openpete_glitch_present_t *p);

/** @brief Raise the mark, to be written by the next openpete_glitch_mark_service call.
 *
 * Copies @p reason into the name the file header carries (truncated to 23
 * characters; a NULL or empty reason keeps the previous name, initially
 * "key"), then sets the request flag with a release store. The flag is
 * atomic; the name copy is not, so two concurrent callers may leave either
 * name. No effect when the module is off.
 * @param reason What raised the mark: "key" from the host key binding, "env"
 *        from the OPENPETE_GLITCH_MARK_AT trigger. */
void openpete_glitch_mark_request(const char *reason);

/** @brief Evaluate OPENPETE_GLITCH_MARK_AT and write a pending mark to a text file.
 *
 * Called once per host-loop iteration on the host loop, the thread allowed
 * to block on file IO. The trigger fires once, raising the mark with reason
 * "env", when its counter reaches the value:
 *   N or pN   present serial N
 *   wN        walk ordinal N (a headless run composes no presents)
 *   Nms       N wall-clock milliseconds after openpete_glitch_mark_init
 * A pending mark is consumed and both rings are copied, then written to
 * glitch-mark-S.txt in the log file's directory (else the working
 * directory), S being the present count, or the walk count when no present
 * was recorded. A record recycled before the copy is written as a lost row.
 * The file name is also posted as an OSD notification.
 * @param path_out Receives the written file's path.
 * @param path_cap Capacity of @p path_out.
 * @return 1 when a file was written; 0 when the module is off, an argument is
 *         NULL or zero, no mark is pending, or the snapshot or file could not
 *         be allocated or opened. */
int  openpete_glitch_mark_service(char *path_out, size_t path_cap);

#ifdef __cplusplus
}
#endif

#endif
