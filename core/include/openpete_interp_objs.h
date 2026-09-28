/* core/include/openpete_interp_objs.h — per-object prev-state snapshot table.
 *
 * The singleton
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
 *       capture(key, blob, len)   // on a changed blob roll curr->prev, write curr; seen=1
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
 * same enable as the interp ring (openpete_interp_set_enabled).
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

/* Defaults for the two sizes the game sets at registration: the hash-table
 * capacity (open addressing, power of two) and the per-object blob size. The
 * blob must hold one whole object record, so a consumer reads fields at their
 * natural offsets (the moby position, animation state and rotation) with no
 * remapping — which makes it a per-game size, since it is that game's record
 * stride. The capacity covers a few hundred live objects at load factor < 0.5.
 * A game that registers no sizes gets these. */
#define OPENPETE_INTERP_OBJ_SLOTS       4096u
#define OPENPETE_INTERP_OBJ_BLOB_BYTES  88u

typedef struct {
    uint32_t key;        /* object identity (0 = empty slot) */
    uint8_t  have_prev;  /* 1 iff prev[] holds the previous distinct state's blob */
    uint8_t  seen;       /* 1 iff captured this tick (despawn eviction) */
    uint8_t  have_prev2; /* 1 iff prev2[] holds the DISTINCT tick before prev
                          * (two back from curr) — enables cubic/pred1 interp
                          * with a centered start-tangent, else fall back to
                          * the linear prev→curr lerp. */
    uint16_t blob_len;   /* valid bytes in prev/curr/prev2 (<= the registered size) */
    /* The three blobs, each the registered blob size, carved out of the
     * table's own allocation: the entry never owns storage, so its slot owns
     * the bytes and a rehash copies them rather than aliasing them. */
    uint8_t* prev;
    uint8_t* curr;
    uint8_t* prev2;
    /* Tick stamp (openpete_interp_tick_stamp) when curr/prev were last ROLLED
     * — i.e. when each DISTINCT state was captured (unchanged ticks dedupe
     * and do NOT update these). A consumer interpolates with alpha = (now - curr_ns)/period,
     * clamped, where `period` is (curr_ns - prev_ns) CAPPED AT ONE NOMINAL TICK:
     * a steady object interpolates over its one-tick interval, and one
     * that resumes after a long static stretch sweeps the full delta over one
     * tick instead of crawling 1/K then popping (prev_ns is pinned K ticks back).
     * Clamps to curr once the object stops (curr_ns goes stale). The per-object
     * equivalent of the DIRECT ring's tick_ns alpha. */
    op_time_t prev_ns, curr_ns;
    op_time_t prev2_ns;   /* roll time of prev2 (for pred1's centered tangent) */
    /* The stamp of the last capture that observed curr (curr_seen_ns) and the
     * last capture that observed prev before it rolled out (prev_seen_ns, 0
     * before the first roll). Unchanged ticks update curr_seen_ns, so a record
     * that holds one state for K ticks and then changes has prev_ns K ticks
     * back while prev_seen_ns is the tick before curr_ns: prev IS its tick N-1
     * state. */
    op_time_t prev_seen_ns, curr_seen_ns;
} openpete_interp_obj_t;

/* Per-tick capture callback supplied by the game. Invoked once per sim tick
 * (after the DIRECT-region capture) iff capture is enabled. The callback
 * must bracket its per-object capture() calls with begin_tick()/end_tick()
 * itself — core does not auto-bracket, so a game with several object pools
 * can capture them all inside one begin/end pair. NULL disables it. */
typedef void (*openpete_interp_obj_capture_fn)(void);
/** @brief Register the game's per-tick object capture callback and size the table.
 *
 * Called from games/common/renderer/interp_regions_gen.c at startup. The
 * table is allocated here, so the sizes are the game's rather than the
 * build's. Registering again with the sizes in force keeps the table;
 * different sizes free it, reallocate it and drop every tracked object.
 * @param fn Capture callback, or NULL to disable object capture.
 * @param blob_bytes One object record of this game (its array stride); 0
 *        keeps OPENPETE_INTERP_OBJ_BLOB_BYTES.
 * @param slots Table capacity, rounded up to a power of two; 0 keeps
 *        OPENPETE_INTERP_OBJ_SLOTS. */
void openpete_interp_register_obj_capture(openpete_interp_obj_capture_fn fn,
                                          uint32_t blob_bytes, uint32_t slots);
/** @brief Return the per-object blob size in force.
 *
 * A capture walk bounds its copy length by it.
 * @return Blob size in bytes. */
uint32_t openpete_interp_objs_blob_bytes(void);
/** @brief Return the table capacity in force.
 *
 * @return Slot count, a power of two. */
uint32_t openpete_interp_objs_slots(void);

/** @brief Run the registered object capture callback.
 *
 * Called by run.c once per sim tick after the direct-region capture, except
 * on a runahead tick that skips capture. No-op when openpete_interp_enabled()
 * is 0 or no callback is registered. */
void openpete_interp_objs_run_capture(void);

/* --- the store, driven by the game's capture callback --- */

/** @brief Begin a capture tick by marking every tracked object unseen.
 *
 * Allocates the table on first use. Clears the spawn count; end_tick
 * evicts the objects still unseen. */
void openpete_interp_objs_begin_tick(void);

/** @brief Record object @p key's current blob.
 *
 * A tracked key whose blob is byte-identical to its curr[] only refreshes
 * curr_seen_ns: the logic did not advance it, and prev stays pinned to the
 * last distinct state, the same dedupe the direct ring applies. A changed
 * blob rolls prev into prev2 (when have_prev), curr into prev, and stamps
 * curr_ns with openpete_interp_tick_stamp(). A new key occupies an empty
 * slot with have_prev = 0 (spawn). A full table drops the capture. Marks
 * the object seen.
 * @param key Object identity; 0 is ignored.
 * @param data Blob source; NULL is ignored.
 * @param len Bytes to record, clamped to the registered blob size. */
void openpete_interp_objs_capture(uint32_t key, const void* data, uint32_t len);

/** @brief End a capture tick by evicting every object not seen since begin_tick.
 *
 * The survivors are rehashed into a cleared table so linear-probe chains
 * stay intact, and a later reuse of an evicted key pairs as a fresh spawn
 * rather than across the gap. Finalises the tracked, paired and despawned
 * counts. */
void openpete_interp_objs_end_tick(void);

/** @brief Drop every tracked object and zero the stats.
 *
 * Called on a guest-state discontinuity (the handler openpete_interp_init
 * registers: savestate load, rewind restore) and by run.c beside
 * openpete_interp_reset. The next capture tick records every object as a
 * spawn (have_prev = 0), so feeders snap instead of lerping across the
 * jump. */
void openpete_interp_objs_reset(void);

/** @brief Look up a tracked object by identity.
 *
 * @param key Object identity; 0 returns NULL.
 * @return The entry, valid until the next end_tick(), or NULL when not
 *         tracked. */
const openpete_interp_obj_t* openpete_interp_objs_find(uint32_t key);

/** @brief Return the present-clock instant at which every per-object consumer has reached its curr state.
 *
 * The identity legs (the alpha-1 intermediate screenshot, the identity
 * audit) set the present target to it. A per-object consumer's sub-tick
 * phase is
 * (present_now - o->curr_ns) / (o->curr_ns - o->prev_ns), clamped at 1, and the
 * store pins prev to the last distinct state. An object whose state advances
 * slower than the tick therefore carries a denominator of two or three tick
 * periods, and one nominal period past the ring's curr stamp leaves it at
 * t = 1/2 or 2/3 — mid-blend, not at the pose the canonical dump draws. This
 * returns the largest o->curr_ns + (o->curr_ns - o->prev_ns) over the tracked
 * objects, floored at one nominal period past the ring's curr stamp, so the
 * slowest live gap saturates and every faster consumer clamps at 1.
 * Reads no wall clock: the value is built from the stamps the store already
 * holds.
 * @return The identity present instant. */
op_time_t openpete_interp_identity_present_ns(void);

/** @brief Return the counts of the most recently completed capture tick.
 *
 * Each out pointer may be NULL.
 * @param tracked Receives the objects kept by the last end_tick.
 * @param paired Receives the kept objects with have_prev set.
 * @param spawned Receives the new keys since the last begin_tick.
 * @param despawned Receives the objects the last end_tick evicted. */
void openpete_interp_objs_stats(int* tracked, int* paired,
                                int* spawned, int* despawned);

/* One sim tick in WALL time — the interp denominator cap shared by every
 * sub-tick consumer (the DIRECT ring's interp_period_ns and the per-object
 * phase lerps). Defaults to one NTSC tick (37609/1125000 s = 29.9130527 Hz,
 * rounded to whole ns — this is an alpha denominator, not a deadline the
 * pacer steps, so the fractional 2/9 ns is far below any alpha's
 * resolution); run.c rescales it
 * by 1/tick_scale at startup so --tick-scale slow-motion stretches every
 * alpha denominator with the real tick cadence — a hardcoded NTSC cap makes
 * phase sweeps run 1/tick_scale× too fast and pin at the clamp for the rest
 * of each tick (at 0.5, every animation channel sweeps in half a tick and
 * freezes the other half). */
extern op_time_t g_openpete_tick_period_ns;

/** @brief Return the stamp of the most recent sim tick.
 *
 * run.c sets it beside openpete_interp_capture: the PLL-smoothed VBL grid
 * stamp, or the direct ring's curr_tick_ns under deterministic capture.
 * Any per-tick channel that interpolates against the present clock stamps
 * its captures with this, never op_now(): the wall-clock capture time
 * drifts from the tick grid by scheduling jitter, and under eager
 * production the scene is extracted long before the instant it displays.
 * @return The tick stamp, or op_now() while the run loop has not set one. */
op_time_t openpete_interp_tick_stamp(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_INTERP_OBJS_H */
