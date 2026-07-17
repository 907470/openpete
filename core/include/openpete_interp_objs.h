/* core/include/openpete_interp_objs.h — per-object prev-state snapshot table.
 *
 * Step 1 of the native-renderer high-FPS plan
 * (notes/plans/openpete-native-renderer.md §"Movement"). The singleton
 * interp ring (openpete_interp.h) snapshots fixed DIRECT regions (g_Camera,
 * g_Spyro). This table is its INDIRECT counterpart: a per-object,
 * double-buffered snapshot keyed by a stable object identity (for Spyro 1,
 * the Moby's vaddr in the g_LevelMobys array — slot-stable across ticks).
 *
 * Contract per tick (called from run.c after a successful sim tick, same
 * point as openpete_interp_capture — read-only against PSX RAM):
 *
 *   begin_tick()                  // mark every tracked object unseen
 *   for each live object:
 *       capture(key, blob, len)   // roll its curr->prev, write curr, seen=1
 *   end_tick()                    // evict objects not seen (despawned)
 *
 * Then, during an intermediate (sub-tick) present, a feeder pairs an object
 * by identity:
 *
 *   const openpete_interp_obj_t* o = openpete_interp_objs_find(key);
 *   if (o && o->have_prev)  display lerp(o->prev, o->curr, alpha);
 *   else                    snap  (use curr / live RAM)   // spawn → no prev
 *
 * `have_prev` is the per-object pairing verdict: it is 0 on the first tick
 * an object appears (spawn), and reset to 0 if the object despawns and its
 * slot is later reused — so a feeder never lerps across a spawn/despawn gap.
 *
 * Determinism: this TU never writes PSX RAM. The blob bytes are copied out
 * of host-mapped PSX RAM; nothing is written back. Capture is gated by the
 * same enable as the interp ring, so it is inert unless --render-fps > sim.
 *
 * Game-agnostic: core owns the store + the pairing/eviction logic; the game
 * supplies the per-tick walk (which objects, which bytes) via a registered
 * capture callback (openpete_interp_register_obj_capture). core/ holds zero
 * game addresses.
 */

#ifndef OPENPETE_INTERP_OBJS_H
#define OPENPETE_INTERP_OBJS_H

#include <stdint.h>
#include "openpete_time.h"   /* op_time_t — per-object distinct-roll timestamps */

#ifdef __cplusplus
extern "C" {
#endif

/* Hash-table capacity (open addressing, power of two) and per-object blob
 * size. 4096 slots comfortably covers Spyro 1's few-hundred live mobys with
 * load factor < 0.5; 88 bytes captures a full stride-0x58 Moby record so a
 * consumer reads fields at their natural Moby offsets (pos @ +0x0C, anim @
 * +0x3C, rot @ +0x44) with no remapping. */
#define OPENPETE_INTERP_OBJ_SLOTS       4096u
#define OPENPETE_INTERP_OBJ_BLOB_BYTES  88u

typedef struct {
    uint32_t key;        /* object identity (0 = empty slot) */
    uint8_t  have_prev;  /* 1 iff prev[] holds the previous tick's blob */
    uint8_t  seen;       /* 1 iff captured this tick (despawn eviction) */
    uint8_t  blob_len;   /* valid bytes in prev/curr/prev2[] (<= BLOB_BYTES) */
    uint8_t  have_prev2; /* 1 iff prev2[] holds the DISTINCT tick before prev
                          * (two back from curr) — enables cubic/pred1 interp
                          * with a centered start-tangent, else fall back to
                          * the linear prev→curr lerp. */
    uint8_t  prev[OPENPETE_INTERP_OBJ_BLOB_BYTES];
    uint8_t  curr[OPENPETE_INTERP_OBJ_BLOB_BYTES];
    uint8_t  prev2[OPENPETE_INTERP_OBJ_BLOB_BYTES];
    /* Wall-clock (op_now) when curr/prev were last ROLLED — i.e. when each
     * DISTINCT state was captured (unchanged ticks dedupe and do NOT update
     * these). A consumer interpolates with alpha = (now - curr_ns)/period,
     * clamped, where `period` is (curr_ns - prev_ns) CAPPED AT ONE NOMINAL TICK
     * (W0.6): a steady object interpolates over its one-tick interval, and one
     * that resumes after a long static stretch sweeps the full delta over one
     * tick instead of crawling 1/K then popping (prev_ns is pinned K ticks back).
     * Clamps to curr once the object stops (curr_ns goes stale). The per-object
     * equivalent of the DIRECT ring's tick_ns alpha. */
    op_time_t prev_ns, curr_ns;
    op_time_t prev2_ns;   /* roll time of prev2 (for pred1's centered tangent) */
} openpete_interp_obj_t;

/* Per-tick capture callback supplied by the game. Invoked once per sim tick
 * (after the DIRECT-region capture) iff capture is enabled. The callback
 * must bracket its per-object capture() calls with begin_tick()/end_tick()
 * itself — core does not auto-bracket, so a game with several object pools
 * can capture them all inside one begin/end pair. NULL disables it. */
typedef void (*openpete_interp_obj_capture_fn)(void);
void openpete_interp_register_obj_capture(openpete_interp_obj_capture_fn fn);

/* Invoked by core (run.c) once per sim tick. No-op when capture is disabled
 * or no callback is registered. */
void openpete_interp_objs_run_capture(void);

/* --- the store, driven by the game's capture callback --- */

/* Begin a capture tick: marks every tracked object unseen (for despawn
 * detection in end_tick). */
void openpete_interp_objs_begin_tick(void);

/* Record object `key`'s current blob. If the key was captured last tick its
 * curr[] rolls to prev[] (have_prev=1); a brand-new key gets have_prev=0
 * (spawn). `len` is clamped to BLOB_BYTES. Marks the object seen. */
void openpete_interp_objs_capture(uint32_t key, const void* data, uint32_t len);

/* End a capture tick: evict every object not seen this tick (despawned), so
 * a later slot reuse pairs as a fresh spawn rather than across the gap. */
void openpete_interp_objs_end_tick(void);

/* Drop every tracked object (guest-state discontinuity: savestate load,
 * rewind restore). Every entry describes the pre-jump timeline; after this,
 * the next capture tick re-registers everything as a spawn (have_prev=0) so
 * feeders snap instead of lerping across the discontinuity. Fired via the
 * openpete_discontinuity broadcast — see openpete_discontinuity.h. */
void openpete_interp_objs_reset(void);

/* Look up an object by identity. Returns NULL if not tracked. The returned
 * pointer is valid until the next end_tick(). */
const openpete_interp_obj_t* openpete_interp_objs_find(uint32_t key);

/* Diagnostics (counts from the most recently completed capture tick). */
void openpete_interp_objs_stats(int* tracked, int* paired,
                                int* spawned, int* despawned);

/* One sim tick in WALL time — the interp denominator cap shared by every
 * sub-tick consumer (the DIRECT ring's interp_period_ns and the per-object
 * phase lerps). Defaults to one NTSC tick (1001/30000 s); run.c rescales it
 * by 1/tick_scale at startup so --tick-scale slow-motion stretches every
 * alpha denominator with the real tick cadence — a hardcoded NTSC cap makes
 * phase sweeps run 1/tick_scale× too fast and pin at the clamp for the rest
 * of each tick (at 0.5, every animation channel sweeps in half a tick and
 * freezes the other half). */
extern op_time_t g_openpete_tick_period_ns;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INTERP_OBJS_H */
