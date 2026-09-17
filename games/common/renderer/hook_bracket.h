/* hook_bracket.h: the one bracket every native feeder wraps its guest draw in.
 *
 * A feeder hooks the game's own draw entry so it can re-derive the same
 * geometry natively. Every such hook needs the same five things around the
 * guest call, in the same order:
 *
 *   1. a gate, so a run that neither feeds the native scene nor verifies nor
 *      isolates render paths falls straight through to the ordinary dispatch
 *      at one branch of cost;
 *   2. a re-entrancy latch, because the dispatch below re-enters the hook;
 *   3. the polybuf cursor read before the guest draw;
 *   4. the guest draw itself, once, under a profile scope;
 *   5. the cursor read after it, and render_path_record of the interval the
 *      draw allocated, which drives the ordering-table filter and the
 *      canon-drawn liveness gate the sub-tick re-extract reads.
 *
 * The guest draw runs exactly once on every path that takes the hook over: it
 * writes m_WasDrawn and the sector visibility flags that the next tick's
 * gameplay reads, so a second call or a skipped call is a gameplay-state
 * change, not a rendering one.
 *
 * Usage: the caller keeps its own post-draw body and closes the latch:
 *
 *     static hook_bracket_state s_hb;
 *     static const hook_bracket_desc k_desc = {
 *         .path = RP_FLAME, .prof_tag = "flame.srend",
 *         .run = run_flame, .verify_enabled = flame_verify_enabled,
 *         .st = &s_hb,
 *     };
 *     uint32_t startp, endp; int feed, verify;
 *     if (!hook_bracket_run(cpu, addr, &k_desc, &startp, &endp, &feed, &verify))
 *         return 0;
 *     ... feed / verify from [startp, endp) ...
 *     return hook_bracket_end(&k_desc);
 */

#ifndef OPENPETE_HOOK_BRACKET_H
#define OPENPETE_HOOK_BRACKET_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/render_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Do not take the hook over for --render-paths alone. A feeder whose prims
 * are already attributed elsewhere, or which has nothing to record for the
 * ordering-table filter, sets this and brackets only to feed or verify. */
#define HB_NO_RENDER_PATHS 0x1u

/* The bracket's own mutable slots, zero-initialized: the re-entrancy latch and
 * a cache of the profile bucket's interned id (stored plus one, so zero reads
 * as "not yet interned"). One instance per latch, which is per hooked entry,
 * several descriptors that must not nest share one. */
typedef struct {
    int in;
    int prof_id_plus1;
} hook_bracket_state;

typedef struct {
    render_path_t path;        /* the path the drawn interval is recorded under */
    const char*   prof_tag;    /* profile bucket for the guest draw's time */

    /* The guest draw. NULL runs the recomp module's dispatch on `addr`, which
     * is what a hook on the game's own entry wants; a feeder that reaches its
     * draw another way (a direct recompiled call, a provider pass) supplies
     * its own. */
    void (*run)(CPUState* cpu, uint32_t addr);

    /* The feeder's verify latch, reported back as `verify`. NULL never
     * verifies. */
    int (*verify_enabled)(void);

    /* A further reason to bracket, ORed into the gate but NOT reported as
     * `verify`: a second verify mode with its own report path. NULL = none. */
    int (*extra_gate)(void);

    /* Run after the gate and the latch, before the cursor read: state the
     * feeder must capture while the guest draw has not yet clobbered it
     * (argument registers, a pool pointer, a list base). NULL = none. */
    void (*pre)(CPUState* cpu, int feed, int verify);

    hook_bracket_state* st;    /* latch + profile-id cache; see above */
    unsigned flags;            /* HB_* above */
} hook_bracket_desc;

/* Gate, latch, and run the guest draw once. Returns 0 when the hook declines
 * (inert, or re-entered) and the caller must return 0 so the ordinary
 * dispatch runs the draw; returns 1 having run it, with the polybuf interval
 * in *start/*end and the gate's verdicts in *feed/*verify. */
int hook_bracket_run(CPUState* cpu, uint32_t addr, const hook_bracket_desc* d,
                     uint32_t* start, uint32_t* end, int* feed, int* verify);

/* Release the latch. Returns 1, the "took over, body already ran" verdict a
 * feature dispatch hook returns. */
int hook_bracket_end(const hook_bracket_desc* d);

/* The gate a sub-tick re-extract shares: feed the native scene only while the
 * scene is live, this path is selected, and the most recent canonical tick
 * actually drew it. Without the last term a paused canonical renderer (a sim
 * stall, a level transition) leaves the intermediates replaying the last
 * geometry as a ghost over a scene that has stopped. */
int hook_reextract_ready(render_path_t path);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOOK_BRACKET_H */
