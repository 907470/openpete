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

/* Allocate the rings. Called once at startup, before any other thread
 * exists, so the note paths below never have to test for initialization
 * under a lock. Depth comes from OPENPETE_GLITCH_MARK_N (default 600, about
 * 1.25 s of presents at 480 Hz), clamped to [16, 65536]. */
void openpete_glitch_mark_init(void);

/* Ring depth actually allocated; 0 when the module is off. */
int  openpete_glitch_mark_depth(void);

/* Push one walk census. Called by the terrain walk on the guest thread. The
 * record is also held as "the walk most recently completed", which the scene
 * publish copies into its slot so the present that composes that scene
 * carries the right census under eager production. */
void openpete_glitch_mark_note_walk(const openpete_glitch_walk_t *w);

/* The walk most recently completed, for the scene publish to snapshot. */
void openpete_glitch_mark_last_walk(openpete_glitch_walk_t *out);

/* Push one present record. Called by the present census on the display
 * owner's thread. */
void openpete_glitch_mark_note_present(const openpete_glitch_present_t *p);

/* Raise the mark. `reason` names what raised it ("key", "env") and appears
 * in the file header. Safe from any thread: it sets a flag. */
void openpete_glitch_mark_request(const char *reason);

/* Write a pending mark out, and evaluate OPENPETE_GLITCH_MARK_AT. Called
 * once per host-loop iteration, on the host loop, because it blocks on file
 * IO. Returns 1 when a file was written and fills `path_out` with its path.
 *
 * OPENPETE_GLITCH_MARK_AT fires the mark automatically:
 *   <n> or p<n>   at present serial n
 *   w<n>          at walk ordinal n (the only spelling headless can reach,
 *                 since a headless run composes no presents)
 *   <n>ms         n milliseconds after the module was initialized
 */
int  openpete_glitch_mark_service(char *path_out, size_t path_cap);

#ifdef __cplusplus
}
#endif

#endif
