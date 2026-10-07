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

/** @brief Gate, latch, and run the guest draw once, recording the polybuf interval it allocated.
 *
 * The gate passes when the descriptor's verify latch, the live native scene,
 * --render-paths (unless HB_NO_RENDER_PATHS) or the extra gate asks for the
 * bracket. Past the gate and the re-entrancy latch, `pre` runs, the polybuf
 * cursor is read, the guest draw runs under the descriptor's profile bucket,
 * the cursor is read again, and the interval is recorded under the
 * descriptor's render path. The latch stays set until hook_bracket_end.
 * @param cpu The guest CPU state.
 * @param addr The hooked entry address, passed to the guest draw.
 * @param d The feeder's descriptor.
 * @param start Receives the polybuf cursor before the draw.
 * @param end Receives the polybuf cursor after the draw.
 * @param feed Receives whether the native scene is live.
 * @param verify Receives the verify latch's verdict.
 * @return 0 when the hook declines (inert, or re-entered) and the caller must
 *         return 0 so the ordinary dispatch runs the draw; 1 having run it,
 *         with every out-parameter written. */
int hook_bracket_run(CPUState* cpu, uint32_t addr, const hook_bracket_desc* d,
                     uint32_t* start, uint32_t* end, int* feed, int* verify);

/** @brief Release the re-entrancy latch hook_bracket_run set.
 *
 * @param d The descriptor passed to hook_bracket_run.
 * @return 1, the "took over, body already ran" verdict a feature dispatch
 *         hook returns. */
int hook_bracket_end(const hook_bracket_desc* d);

/** @brief The gate every sub-tick re-extract shares.
 *
 * Feeds the native scene only while the scene is live, this path is
 * selected, and the most recent canonical tick drew it. Without the last term
 * a paused canonical renderer (a sim stall, a level transition) leaves the
 * intermediates replaying the last geometry as a ghost over a scene that has
 * stopped.
 * @param path The feeder's render path.
 * @return 1 when the re-extract may feed, else 0. */
int hook_reextract_ready(render_path_t path);

/** @brief Whether a re-extract may feed, on a held camera cut by the producer's own tick N-1 set.
 *
 * Outside a hold, hook_reextract_ready. On a held cut the producer's set
 * choice (cut_held_set_pick) stands in for the canon-drawn term, since whether
 * tick N drew the path does not decide what tick N-1 drew: no feed for
 * CUT_HELD_EMPTY, else the scene is live and the path selected.
 * @param path The feeder's render path.
 * @param held -1 outside a hold, else the producer's cut_held_pick_t.
 * @return 1 when the re-extract may feed, else 0. */
int hook_reextract_ready_held(render_path_t path, int held);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_HOOK_BRACKET_H */
