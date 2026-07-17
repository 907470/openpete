/* core/runtime/openpete_interp_objs.c — per-object prev-state snapshot table.
 *
 * See openpete_interp_objs.h for the contract. Open-addressing hash table
 * (linear probe) keyed by object identity. Despawn eviction is done by
 * rehash-compaction in end_tick (cheaper than tombstones for a table this
 * small, and keeps probe chains clean for the next tick's lookups).
 *
 * Determinism: read-only against PSX RAM. Capture is gated by the interp
 * ring's enable, so this is inert unless --render-fps exceeds the sim rate.
 */

#include "openpete_interp_objs.h"
#include "openpete_interp.h"   /* openpete_interp_enabled() — shared gate */
#include "openpete_time.h"     /* op_now() — per-object distinct-roll timestamps */

/* PLL-smoothed VBL stamp shared with the region ring (set in run.c right
 * before openpete_interp_capture). Roll timestamps must sit on the ideal VBL
 * grid, not the jittery actual tick-completion time — jittery stamps make the
 * per-object alpha mapping discontinuous at every roll (fast movers judder
 * ±half a tick per present). Falls back to op_now() if unset (unit harnesses
 * that drive the objs table without the run loop). */
op_time_t g_openpete_tick_stamp = 0;
static op_time_t objs_tick_stamp(void) {
    return g_openpete_tick_stamp ? g_openpete_tick_stamp : op_now();
}

/* One sim tick in wall time (see the header note). Default NTSC; run.c
 * rescales by 1/tick_scale so --tick-scale stretches every consumer's
 * alpha denominator with the real cadence. */
op_time_t g_openpete_tick_period_ns = (op_time_t)((OP_NS_PER_SEC * 1001) / 30000);

#include <stdint.h>
#include <string.h>

#define SLOTS  OPENPETE_INTERP_OBJ_SLOTS
#define BLOB   OPENPETE_INTERP_OBJ_BLOB_BYTES

static openpete_interp_obj_t s_tab[SLOTS];
static int                   s_count;        /* live entries currently in the table */

/* Per-tick stats (finalised in end_tick). */
static int s_stat_tracked, s_stat_paired, s_stat_spawned, s_stat_despawned;

static openpete_interp_obj_capture_fn s_capture_cb = NULL;

void openpete_interp_register_obj_capture(openpete_interp_obj_capture_fn fn) {
    s_capture_cb = fn;
}

/* Multiplicative (Knuth) hash → slot index. key is never 0 for a live
 * object (0 marks an empty slot). */
static inline uint32_t slot_of(uint32_t key) {
    return (uint32_t)((key * 2654435761u) >> 20) & (SLOTS - 1u);
}

/* Find the slot holding `key`, or the first empty slot on its probe chain.
 * Returns the index; *found = 1 if the slot already holds `key`. */
static uint32_t probe(uint32_t key, int* found) {
    uint32_t i = slot_of(key);
    for (uint32_t n = 0; n < SLOTS; ++n) {
        if (s_tab[i].key == 0)   { *found = 0; return i; }   /* empty: insertion point */
        if (s_tab[i].key == key) { *found = 1; return i; }
        i = (i + 1u) & (SLOTS - 1u);
    }
    *found = 0;
    return SLOTS;   /* table full — caller drops rather than corrupt a colliding slot */
}

void openpete_interp_objs_begin_tick(void) {
    for (uint32_t i = 0; i < SLOTS; ++i)
        if (s_tab[i].key) s_tab[i].seen = 0;
    s_stat_spawned = 0;
}

void openpete_interp_objs_capture(uint32_t key, const void* data, uint32_t len) {
    if (key == 0 || !data) return;
    if (len > BLOB) len = BLOB;

    int found;
    uint32_t i = probe(key, &found);
    if (i >= SLOTS) return;   /* table full — drop rather than corrupt a colliding slot */

    if (found) {
        /* Dedupe at capture (mirrors openpete_interp_capture): if this object's
         * blob is byte-identical to last tick's curr, the game logic didn't
         * advance it (30 Hz logic on a 60 Hz VBL → every other tick is static),
         * so DON'T roll — keep prev pinned to the last DISTINCT state. Otherwise
         * a static VBL rolls curr into prev, losing the real prev, and the
         * object snaps to curr while the (deduped) camera ring is still
         * interpolating prev→curr → the object leads the world (ghosting with
         * camera movement). Keeping both rings on the same distinct-tick
         * boundaries makes the shared alpha consistent. */
        if (s_tab[i].blob_len == (uint8_t)len &&
            memcmp(s_tab[i].curr, data, len) == 0) {
            s_tab[i].seen = 1;
            return;                       /* unchanged — keep prev/curr + their timestamps */
        }
        /* Existing object that moved: roll prev -> prev2 (for the pred1 centered
         * tangent), then curr -> prev, write curr, and stamp the roll times so
         * consumers interpolate over this DISTINCT interval (sweeping across the
         * unchanged ticks in between). */
        if (s_tab[i].have_prev) {
            memcpy(s_tab[i].prev2, s_tab[i].prev, s_tab[i].blob_len);
            s_tab[i].have_prev2 = 1;
            s_tab[i].prev2_ns   = s_tab[i].prev_ns;
        }
        memcpy(s_tab[i].prev, s_tab[i].curr, s_tab[i].blob_len);
        s_tab[i].have_prev = 1;
        s_tab[i].prev_ns   = s_tab[i].curr_ns;
        s_tab[i].curr_ns   = objs_tick_stamp();
    } else {
        /* Spawn: occupy the slot, no previous tick to pair against. */
        if (s_tab[i].key == 0 && s_count < (int)SLOTS) s_count++;
        s_tab[i].key       = key;
        s_tab[i].have_prev = 0;
        s_tab[i].have_prev2 = 0;
        s_tab[i].prev_ns   = 0;
        s_tab[i].curr_ns   = objs_tick_stamp();
        s_stat_spawned++;
    }
    memcpy(s_tab[i].curr, data, len);
    s_tab[i].blob_len = (uint8_t)len;
    s_tab[i].seen     = 1;
}

void openpete_interp_objs_end_tick(void) {
    /* Evict unseen (despawned) entries, then rehash-compact the survivors so
     * linear-probe chains stay intact for next tick's lookups. */
    static openpete_interp_obj_t s_survivors[SLOTS];
    int nsurv = 0, paired = 0, despawned = 0;

    for (uint32_t i = 0; i < SLOTS; ++i) {
        if (!s_tab[i].key) continue;
        if (s_tab[i].seen) {
            s_survivors[nsurv++] = s_tab[i];
            if (s_tab[i].have_prev) paired++;
        } else {
            despawned++;
        }
    }

    memset(s_tab, 0, sizeof(s_tab));
    s_count = 0;
    for (int k = 0; k < nsurv; ++k) {
        int found;
        uint32_t i = probe(s_survivors[k].key, &found);
        if (i >= SLOTS) continue;   /* defensive: never full during rehash (nsurv <= SLOTS) */
        s_tab[i] = s_survivors[k];
        s_count++;
    }

    s_stat_tracked   = nsurv;
    s_stat_paired    = paired;
    s_stat_despawned = despawned;
}

void openpete_interp_objs_reset(void) {
    memset(s_tab, 0, sizeof(s_tab));
    s_count = 0;
    s_stat_tracked = s_stat_paired = s_stat_spawned = s_stat_despawned = 0;
}

const openpete_interp_obj_t* openpete_interp_objs_find(uint32_t key) {
    if (key == 0) return NULL;
    int found;
    uint32_t i = probe(key, &found);
    return found ? &s_tab[i] : NULL;
}

void openpete_interp_objs_stats(int* tracked, int* paired,
                                int* spawned, int* despawned) {
    if (tracked)   *tracked   = s_stat_tracked;
    if (paired)    *paired    = s_stat_paired;
    if (spawned)   *spawned   = s_stat_spawned;
    if (despawned) *despawned = s_stat_despawned;
}

void openpete_interp_objs_run_capture(void) {
    if (!openpete_interp_enabled()) return;
    if (!s_capture_cb) return;
    s_capture_cb();
}
