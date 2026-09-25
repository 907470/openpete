/* core/renderer/openpete_interp_objs.c — per-object prev-state snapshot table.
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
    return g_openpete_tick_stamp ? g_openpete_tick_stamp : op_now(); /* clock-guard: wall */ /* unit harnesses only */
}
op_time_t openpete_interp_tick_stamp(void) { return objs_tick_stamp(); }

/* One sim tick in wall time (see the header note). Default NTSC; run.c
 * rescales by 1/tick_scale so --tick-scale stretches every consumer's
 * alpha denominator with the real cadence. */
op_time_t g_openpete_tick_period_ns = (op_time_t)33430222;  /* 37609/1125000 s */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The two sizes the game registers, and the table they size. Each slot owns
 * three consecutive blobs in one flat allocation and the entry's three
 * pointers address them, so a slot's bytes never move and a rehash copies
 * them explicitly. The survivor buffer end_tick compacts through owns a
 * second flat allocation for the same reason. */
static uint32_t SLOTS = OPENPETE_INTERP_OBJ_SLOTS;
static uint32_t BLOB  = OPENPETE_INTERP_OBJ_BLOB_BYTES;

static openpete_interp_obj_t* s_tab;
static uint8_t*               s_tab_blob;
static openpete_interp_obj_t* s_surv;
static uint8_t*               s_surv_blob;
static int                    s_count;   /* live entries currently in the table */

/* Per-tick stats (finalised in end_tick). */
static int s_stat_tracked, s_stat_paired, s_stat_spawned, s_stat_despawned;

static openpete_interp_obj_capture_fn s_capture_cb = NULL;

static void bind_blobs(openpete_interp_obj_t* tab, uint8_t* blob) {
    for (uint32_t i = 0; i < SLOTS; ++i) {
        uint8_t* b = blob + (size_t)i * 3u * BLOB;
        tab[i].prev  = b;
        tab[i].curr  = b + BLOB;
        tab[i].prev2 = b + 2u * BLOB;
    }
}

/* Clear every entry without dropping the blob bindings. */
static void table_clear(openpete_interp_obj_t* tab) {
    for (uint32_t i = 0; i < SLOTS; ++i) {
        uint8_t *p = tab[i].prev, *c = tab[i].curr, *p2 = tab[i].prev2;
        memset(&tab[i], 0, sizeof tab[i]);
        tab[i].prev = p; tab[i].curr = c; tab[i].prev2 = p2;
    }
}

/* Copy one entry into another slot, blob bytes included: the destination keeps
 * its own storage, so the two entries never alias. */
static void entry_move(openpete_interp_obj_t* dst, const openpete_interp_obj_t* src) {
    uint8_t *p = dst->prev, *c = dst->curr, *p2 = dst->prev2;
    *dst = *src;
    dst->prev = p; dst->curr = c; dst->prev2 = p2;
    memcpy(p,  src->prev,  BLOB);
    memcpy(c,  src->curr,  BLOB);
    memcpy(p2, src->prev2, BLOB);
}

static int objs_alloc(void) {
    if (s_tab) return 1;
    size_t blobs = (size_t)SLOTS * 3u * BLOB;
    s_tab       = (openpete_interp_obj_t*)calloc(SLOTS, sizeof *s_tab);
    s_tab_blob  = (uint8_t*)calloc(blobs, 1);
    s_surv      = (openpete_interp_obj_t*)calloc(SLOTS, sizeof *s_surv);
    s_surv_blob = (uint8_t*)calloc(blobs, 1);
    if (!s_tab || !s_tab_blob || !s_surv || !s_surv_blob) {
        free(s_tab); free(s_tab_blob); free(s_surv); free(s_surv_blob);
        s_tab = s_surv = NULL; s_tab_blob = s_surv_blob = NULL;
        return 0;
    }
    bind_blobs(s_tab,  s_tab_blob);
    bind_blobs(s_surv, s_surv_blob);
    s_count = 0;
    return 1;
}

static uint32_t round_pow2(uint32_t v) {
    uint32_t p = 1u;
    while (p < v && p < 0x40000000u) p <<= 1;
    return p;
}

uint32_t openpete_interp_objs_blob_bytes(void) { return BLOB; }
uint32_t openpete_interp_objs_slots(void)      { return SLOTS; }

void openpete_interp_register_obj_capture(openpete_interp_obj_capture_fn fn,
                                          uint32_t blob_bytes, uint32_t slots) {
    s_capture_cb = fn;
    uint32_t nb = blob_bytes ? blob_bytes : OPENPETE_INTERP_OBJ_BLOB_BYTES;
    uint32_t ns = round_pow2(slots ? slots : OPENPETE_INTERP_OBJ_SLOTS);
    if (s_tab && nb == BLOB && ns == SLOTS) return;
    free(s_tab); free(s_tab_blob); free(s_surv); free(s_surv_blob);
    s_tab = s_surv = NULL; s_tab_blob = s_surv_blob = NULL;
    BLOB = nb; SLOTS = ns;
    objs_alloc();
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
    if (!s_tab) { *found = 0; return SLOTS; }
    for (uint32_t n = 0; n < SLOTS; ++n) {
        if (s_tab[i].key == 0)   { *found = 0; return i; }   /* empty: insertion point */
        if (s_tab[i].key == key) { *found = 1; return i; }
        i = (i + 1u) & (SLOTS - 1u);
    }
    *found = 0;
    return SLOTS;   /* table full — caller drops rather than corrupt a colliding slot */
}

void openpete_interp_objs_begin_tick(void) {
    if (!objs_alloc()) return;
    for (uint32_t i = 0; i < SLOTS; ++i)
        if (s_tab[i].key) s_tab[i].seen = 0;
    s_stat_spawned = 0;
}

extern unsigned openpete_frame;
void openpete_interp_objs_capture(uint32_t key, const void* data, uint32_t len) {
    if (key == 0 || !data) return;
    if (!objs_alloc()) return;
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
        if (s_tab[i].blob_len == (uint16_t)len &&
            memcmp(s_tab[i].curr, data, len) == 0) {
            s_tab[i].seen         = 1;
            s_tab[i].curr_seen_ns = objs_tick_stamp();
            return;                       /* unchanged — keep prev/curr + their roll timestamps */
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
        s_tab[i].prev_seen_ns = s_tab[i].curr_seen_ns;
        s_tab[i].curr_ns   = objs_tick_stamp();
    } else {
        /* Spawn: occupy the slot, no previous tick to pair against. */
        if (s_tab[i].key == 0 && s_count < (int)SLOTS) s_count++;
        s_tab[i].key       = key;
        s_tab[i].have_prev = 0;
        s_tab[i].have_prev2 = 0;
        s_tab[i].prev_ns   = 0;
        s_tab[i].prev_seen_ns = 0;
        s_tab[i].curr_ns   = objs_tick_stamp();
        s_stat_spawned++;
        /* OPENPETE_INTERP_OBJS_KEYLOG: one stderr line per spawn, with the
         * key — the identity instrument for keying tests (a re-keyed object
         * shows as a spawn every tick it is re-keyed). */
        {
            static int s_keylog = -1;
            if (s_keylog < 0) {
                const char* e = getenv("OPENPETE_INTERP_OBJS_KEYLOG");
                s_keylog = (e && *e && *e != '0') ? 1 : 0;
            }
            if (s_keylog)
                fprintf(stderr, "[interp-objs] f=%u spawn key=%08x\n", openpete_frame, key);
        }
    }
    memcpy(s_tab[i].curr, data, len);
    s_tab[i].blob_len     = (uint16_t)len;
    s_tab[i].seen         = 1;
    s_tab[i].curr_seen_ns = s_tab[i].curr_ns;
}

void openpete_interp_objs_end_tick(void) {
    /* Evict unseen (despawned) entries, then rehash-compact the survivors so
     * linear-probe chains stay intact for next tick's lookups. */
    if (!s_tab) return;
    int nsurv = 0, paired = 0, despawned = 0;

    for (uint32_t i = 0; i < SLOTS; ++i) {
        if (!s_tab[i].key) continue;
        if (s_tab[i].seen) {
            entry_move(&s_surv[nsurv++], &s_tab[i]);
            if (s_tab[i].have_prev) paired++;
        } else {
            despawned++;
        }
    }

    table_clear(s_tab);
    s_count = 0;
    for (int k = 0; k < nsurv; ++k) {
        int found;
        uint32_t i = probe(s_surv[k].key, &found);
        if (i >= SLOTS) continue;   /* defensive: never full during rehash (nsurv <= SLOTS) */
        entry_move(&s_tab[i], &s_surv[k]);
        s_count++;
    }

    s_stat_tracked   = nsurv;
    s_stat_paired    = paired;
    s_stat_despawned = despawned;
}

void openpete_interp_objs_reset(void) {
    if (s_tab) table_clear(s_tab);
    s_count = 0;
    s_stat_tracked = s_stat_paired = s_stat_spawned = s_stat_despawned = 0;
}

const openpete_interp_obj_t* openpete_interp_objs_find(uint32_t key) {
    if (key == 0 || !s_tab) return NULL;
    int found;
    uint32_t i = probe(key, &found);
    return found ? &s_tab[i] : NULL;
}

op_time_t openpete_interp_identity_present_ns(void) {
    op_time_t base = openpete_interp_curr_tick_ns() + g_openpete_tick_period_ns;
    if (!s_tab) return base;
    for (uint32_t i = 0; i < SLOTS; ++i) {
        const openpete_interp_obj_t* e = &s_tab[i];
        if (!e->key || !e->have_prev) continue;
        if (e->prev_ns == 0 || e->curr_ns <= e->prev_ns) continue;
        op_time_t h = e->curr_ns + (e->curr_ns - e->prev_ns);
        if (h > base) base = h;
    }
    return base;
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
