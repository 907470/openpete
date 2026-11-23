/* core/renderer/openpete_interp_objs.c - per-object prev-state snapshot table.
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
#include "openpete_interp.h"   /* openpete_interp_enabled() - shared gate */
#include "openpete_time.h"     /* op_now() - per-object distinct-roll timestamps */

/* PLL-smoothed VBL stamp shared with the region ring (set in run.c right
 * after openpete_interp_capture; the ring's gridded curr_tick_ns under
 * deterministic capture). Roll timestamps must sit on the ideal VBL
 * grid, not the jittery actual tick-completion time - jittery stamps make the
 * per-object alpha mapping discontinuous at every roll (fast movers judder
 * ±half a tick per present). Falls back to op_now() if unset (unit harnesses
 * that drive the objs table without the run loop). */
OP_ROLE_ATOMIC(op_time_t) g_openpete_tick_stamp = 0;
/** @brief Return the timestamp that marks the current tick's snapshot.
 *
 * The PLL-smoothed VBL stamp g_openpete_tick_stamp when set, else the wall
 * clock (unit harnesses that drive the table without the run loop).
 * @return The stamp in nanoseconds. */
static op_time_t objs_tick_stamp(void) {
    const op_time_t s = atomic_load_explicit(&g_openpete_tick_stamp, memory_order_relaxed);
    return s ? s : op_now(); /* clock-guard: wall */ /* unit harnesses only */
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

/* The entries the last end_tick evicted, kept until the next one: an object
 * gone from the newest capture still has the state the tick before drew, which
 * a sub-tick present showing that tick's set draws. Each entry owns one blob
 * (its curr), carved out of s_ev_blob; prev and prev2 alias it and have_prev
 * is 0, so no consumer pairs across the gap. Past OPENPETE_INTERP_OBJ_EVICTED
 * the rest are dropped and counted. */
static openpete_interp_obj_t* s_ev;
static uint8_t*               s_ev_blob;
static uint32_t               s_ev_n, s_ev_cap, s_ev_blob_len;
static unsigned long          s_ev_dropped;
/* The stamps of the newest capture tick and the one before it (begin_tick). */
static op_time_t s_tick_ns, s_tick_prev_ns;

static openpete_interp_obj_capture_fn s_capture_cb = NULL;

/** @brief Point each entry's prev, curr and prev2 at its three consecutive blobs.
 *
 * Entry i owns the 3 * BLOB bytes at offset i * 3 * BLOB of @p blob, in the
 * order prev, curr, prev2. No bytes are written.
 * @param tab Table of SLOTS entries.
 * @param blob Flat allocation of SLOTS * 3 * BLOB bytes. */
static void bind_blobs(openpete_interp_obj_t* tab, uint8_t* blob) {
    for (uint32_t i = 0; i < SLOTS; ++i) {
        uint8_t* b = blob + (size_t)i * 3u * BLOB;
        tab[i].prev  = b;
        tab[i].curr  = b + BLOB;
        tab[i].prev2 = b + 2u * BLOB;
    }
}

/** @brief Zero every entry of @p tab, keeping each entry's blob pointers.
 *
 * @param tab Table of SLOTS entries. */
static void table_clear(openpete_interp_obj_t* tab) {
    for (uint32_t i = 0; i < SLOTS; ++i) {
        uint8_t *p = tab[i].prev, *c = tab[i].curr, *p2 = tab[i].prev2;
        memset(&tab[i], 0, sizeof tab[i]);
        tab[i].prev = p; tab[i].curr = c; tab[i].prev2 = p2;
    }
}

/** @brief Copy one entry into another slot, blob bytes included.
 *
 * The destination keeps its own blob pointers and receives the source's
 * bytes, so the two entries never alias. Copies the full registered blob
 * size of each of prev, curr and prev2.
 * @param dst Destination entry.
 * @param src Source entry. */
static void entry_move(openpete_interp_obj_t* dst, const openpete_interp_obj_t* src) {
    uint8_t *p = dst->prev, *c = dst->curr, *p2 = dst->prev2;
    *dst = *src;
    dst->prev = p; dst->curr = c; dst->prev2 = p2;
    memcpy(p,  src->prev,  BLOB);
    memcpy(c,  src->curr,  BLOB);
    memcpy(p2, src->prev2, BLOB);
}

/** @brief Allocate the object table, the survivor buffer and their blobs.
 *
 * Zero-filled, blob pointers bound, entry count reset. A no-op once the
 * table exists. On a failed allocation every partial block is freed and the
 * table stays unallocated.
 * @return 1 when the table exists, 0 on allocation failure. */
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

/** @brief Round up to a power of two, saturating at 2^30.
 *
 * @param v Requested count.
 * @return The smallest power of two not below @p v, at least 1 and at most
 *         0x40000000. */
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

/** @brief Map a key to its home slot with a multiplicative (Knuth) hash.
 *
 * Takes bits 20 and up of key * 2654435761, masked to SLOTS - 1; with more
 * than 4096 slots the home slots are the first 4096. A live key is never 0,
 * which marks an empty slot.
 * @param key Object identity.
 * @return Home slot index. */
static inline uint32_t slot_of(uint32_t key) {
    return (uint32_t)((key * 2654435761u) >> 20) & (SLOTS - 1u);
}

/** @brief Find the slot holding @p key, or the first empty slot on its linear-probe chain.
 *
 * @param key Object identity.
 * @param found Receives 1 when the returned slot holds @p key, else 0.
 * @return The slot index, or SLOTS when the table is unallocated or full. */
static uint32_t probe(uint32_t key, int* found) {
    uint32_t i = slot_of(key);
    if (!s_tab) { *found = 0; return SLOTS; }
    for (uint32_t n = 0; n < SLOTS; ++n) {
        if (s_tab[i].key == 0)   { *found = 0; return i; }   /* empty: insertion point */
        if (s_tab[i].key == key) { *found = 1; return i; }
        i = (i + 1u) & (SLOTS - 1u);
    }
    *found = 0;
    return SLOTS;   /* table full - caller drops rather than corrupt a colliding slot */
}

void openpete_interp_objs_begin_tick(void) {
    if (!objs_alloc()) return;
    s_tick_prev_ns = s_tick_ns;
    s_tick_ns = objs_tick_stamp();
    for (uint32_t i = 0; i < SLOTS; ++i)
        if (s_tab[i].key) s_tab[i].seen = 0;
    s_stat_spawned = 0;
    s_ev_n = 0;   /* the evicted set is rebuilt by this tick's captures and end_tick */
}

static uint32_t s_unshown_replaced;   /* cumulative unshown-spawn replacements */

int openpete_interp_objs_keylog(void) {
    static int s_keylog = -1;
    if (s_keylog < 0) {
        const char* e = getenv("OPENPETE_INTERP_OBJS_KEYLOG");
        s_keylog = (e && *e && *e != '0') ? 1 : 0;
    }
    return s_keylog;
}

uint32_t openpete_interp_objs_unshown_replacements(void) { return s_unshown_replaced; }

extern unsigned openpete_frame;
void openpete_interp_objs_capture(uint32_t key, const void* data, uint32_t len) {
    (void)openpete_interp_objs_capture_shown(key, data, len, 1);
}

static void evicted_keep(const openpete_interp_obj_t* e);

static unsigned long s_collisions;   /* second captures of a key in one tick, since startup */

unsigned long openpete_interp_objs_collisions(void) { return s_collisions; }

/** @brief Count a refused capture of a key another capture already wrote this tick; OPENPETE_INTERP_OBJS_COLLIDE prints it.
 *
 * The printed line names the key, the blob length the entry holds (the
 * first source's) and the length refused, for the first 2000 refusals.
 * @param e The entry the capture found already seen.
 * @param len The captured length. */
static void collision_note(const openpete_interp_obj_t* e, uint32_t len) {
    static int on = -1;
    if (on < 0) { const char* v = getenv("OPENPETE_INTERP_OBJS_COLLIDE"); on = (v && *v && *v != '0'); }
    s_collisions++;
    if (on && s_collisions <= 2000ul)
        fprintf(stderr, "[interp-collide] f=%u key=%08x held_len=%u new_len=%u n=%lu\n",
                openpete_frame, e->key, (unsigned)e->blob_len, (unsigned)len, s_collisions);
}
static unsigned long s_reborn;   /* captures that changed a tracked key's serial, since startup */

unsigned long openpete_interp_objs_reborn_count(void) { return s_reborn; }

/* ---- one entry's transitions, applied by a capture ----------------------- */

/** @brief Whether an entry's curr holds exactly the @p len bytes at @p data under the same serial offset.
 *
 * @param e The entry.
 * @param data The bytes captured.
 * @param len Their length.
 * @param serial_off The capture's serial offset, 0 for none.
 * @return 1 when curr is byte-identical and declared the same serial offset, else 0. */
static int entry_curr_equals(const openpete_interp_obj_t* e, const void* data, uint32_t len,
                             uint32_t serial_off) {
    return e->blob_len == (uint16_t)len && e->curr_serial_off == serial_off &&
           memcmp(e->curr, data, len) == 0;
}

/** @brief Whether a tracked entry's key now names another record: its curr declared a serial at @p serial_off that differs from the capture's.
 *
 * @param e The tracked entry.
 * @param data The bytes captured.
 * @param serial_off Offset of the capture's serial word (validated), or 0 for none.
 * @return 1 when both captures declared the serial at @p serial_off and it differs, else 0. */
static int entry_serial_changed(const openpete_interp_obj_t* e, const void* data, uint32_t serial_off) {
    if (!serial_off || e->curr_serial_off != serial_off) return 0;
    return memcmp(e->curr + serial_off, (const uint8_t*)data + serial_off, 4u) != 0;
}

/** @brief Move a tracked entry whose key now names another record to the evicted set at its curr state, counting it.
 *
 * The caller then records the capture as a spawn in the same slot.
 * @param e The tracked entry. */
static void entry_reborn(const openpete_interp_obj_t* e) {
    evicted_keep(e);
    s_reborn++;
    if (openpete_interp_objs_keylog())
        fprintf(stderr, "[interp-objs] f=%u reborn key=%08x n=%lu\n", openpete_frame, e->key, s_reborn);
}

/** @brief Occupy a slot with a new key: no previous state to pair against, curr stamped at this tick.
 *
 * Counts the slot when it was empty and counts the spawn; with
 * OPENPETE_INTERP_OBJS_KEYLOG prints one line per spawn with the key, the
 * identity instrument for keying tests (a re-keyed object shows as a spawn
 * every tick it is re-keyed).
 * @param e The slot.
 * @param key The object identity. */
static void entry_spawn(openpete_interp_obj_t* e, uint32_t key) {
    if (e->key == 0 && s_count < (int)SLOTS) s_count++;
    e->key          = key;
    e->have_prev    = 0;
    e->have_prev2   = 0;
    e->prev_unshown = 0;
    e->prev_serial_off = 0;
    e->prev_ns      = 0;
    e->prev_seen_ns = 0;
    e->curr_ns      = objs_tick_stamp();
    s_stat_spawned++;
    if (openpete_interp_objs_keylog())
        fprintf(stderr, "[interp-objs] f=%u spawn key=%08x\n", openpete_frame, key);
}

/** @brief Roll a tracked entry that moved: prev into prev2 (for the pred1 centred tangent), curr into prev, and stamp the roll.
 *
 * Consumers then interpolate over this distinct interval, sweeping across
 * the unchanged ticks in between.
 * @param e The tracked entry. */
static void entry_roll(openpete_interp_obj_t* e) {
    if (e->have_prev) {
        memcpy(e->prev2, e->prev, e->blob_len);
        e->have_prev2 = 1;
        e->prev2_ns   = e->prev_ns;
    }
    memcpy(e->prev, e->curr, e->blob_len);
    e->have_prev    = 1;
    e->prev_unshown = e->curr_unshown;
    e->prev_serial_off = e->curr_serial_off;
    e->prev_ns      = e->curr_ns;
    e->prev_seen_ns = e->curr_seen_ns;
    e->curr_ns      = objs_tick_stamp();
}

/** @brief Restart a never-rolled entry whose curr was not shown as a fresh spawn at this tick, counting it.
 *
 * Applies to a record spawned at a parking pose and changed into a shown
 * state, so no channel pairs from the unshown pose; a change between two
 * unshown states rolls instead, so the pair is there when the record is
 * first shown unchanged.
 * @param e The tracked entry. */
static void entry_unshown_replace(openpete_interp_obj_t* e) {
    e->curr_ns = objs_tick_stamp();
    s_unshown_replaced++;
    if (openpete_interp_objs_keylog())
        fprintf(stderr, "[interp-objs] f=%u unshown-replace key=%08x n=%u\n",
                openpete_frame, e->key, s_unshown_replaced);
}

int openpete_interp_objs_capture_shown(uint32_t key, const void* data, uint32_t len, int shown) {
    return openpete_interp_objs_capture_serial(key, data, len, shown, 0u, 0);
}

int openpete_interp_objs_capture_serial(uint32_t key, const void* data, uint32_t len, int shown,
                                        uint32_t serial_off, int respawn) {
    if (key == 0 || !data) return 0;
    if (!objs_alloc()) return 0;
    if (len > BLOB) len = BLOB;
    if (serial_off + 4u > len) serial_off = 0u;   /* no room for the word: no serial */

    int found;
    uint32_t i = probe(key, &found);
    if (i >= SLOTS) return 0;   /* table full - drop rather than corrupt a colliding slot */
    openpete_interp_obj_t* e = &s_tab[i];
    /* One entry per key per tick: a second capture of a key is another
     * source keyed on the same address, and rolling it in would leave the
     * first source's entry holding a foreign blob with prev_ns == curr_ns.
     * The first capture of the tick keeps the entry. */
    if (found && e->seen) {
        collision_note(e, len);
        return 0;
    }

    /* The key now names another record: the tracked one despawns at its curr
     * state (the evicted set) and the new one spawns in its slot. */
    if (found && respawn && entry_serial_changed(e, data, serial_off)) {
        entry_reborn(e);
        found = 0;
    }
    int replaced = 0;
    if (!found) {
        entry_spawn(e, key);
    } else if (entry_curr_equals(e, data, len, serial_off)) {
        /* Dedupe at capture (mirrors openpete_interp_capture): a blob
         * byte-identical to last tick's curr means the game logic did not
         * advance the object (30 Hz logic on a 60 Hz VBL), so prev stays
         * pinned to the last distinct state. Rolling here would lose the real
         * prev and the object would lead the deduped camera ring. */
        e->seen         = 1;
        e->curr_seen_ns = objs_tick_stamp();
        return 0;                     /* unchanged - keep prev/curr + their roll timestamps */
    } else if (shown && !e->have_prev && e->curr_unshown) {
        entry_unshown_replace(e);
        replaced = 1;
    } else {
        entry_roll(e);
    }
    memcpy(e->curr, data, len);
    e->blob_len     = (uint16_t)len;
    e->curr_serial_off = (uint16_t)serial_off;
    e->curr_unshown = shown ? 0 : 1;
    e->seen         = 1;
    e->curr_seen_ns = e->curr_ns;
    return replaced;
}

/** @brief Keep an evicted entry's identity and curr state in the evicted set until the next end_tick.
 *
 * The set is (re)allocated at OPENPETE_INTERP_OBJ_EVICTED entries of the
 * registered blob size on first use and after a size change; a full set, or a
 * failed allocation, drops the entry and counts it.
 * @param e The entry end_tick is evicting. */
static void evicted_keep(const openpete_interp_obj_t* e) {
    if (!s_ev || s_ev_blob_len != BLOB) {
        free(s_ev); free(s_ev_blob);
        s_ev_cap = OPENPETE_INTERP_OBJ_EVICTED;
        s_ev = (openpete_interp_obj_t*)calloc(s_ev_cap, sizeof *s_ev);
        s_ev_blob = (uint8_t*)calloc((size_t)s_ev_cap, BLOB);
        s_ev_blob_len = BLOB;
        s_ev_n = 0;
        if (!s_ev || !s_ev_blob) {
            free(s_ev); free(s_ev_blob);
            s_ev = NULL; s_ev_blob = NULL; s_ev_cap = 0;
        }
    }
    if (s_ev_n >= s_ev_cap) { s_ev_dropped++; return; }
    openpete_interp_obj_t* d = &s_ev[s_ev_n];
    uint8_t* b = s_ev_blob + (size_t)s_ev_n * BLOB;
    memset(d, 0, sizeof *d);
    d->key = e->key;
    d->blob_len = e->blob_len;
    d->curr_serial_off = e->curr_serial_off;
    d->curr_unshown = e->curr_unshown;
    d->curr_ns = e->curr_ns;
    d->curr_seen_ns = e->curr_seen_ns;
    memcpy(b, e->curr, e->blob_len);
    d->prev = d->curr = d->prev2 = b;
    s_ev_n++;
}

/** @brief Clear the table and insert the first @p nsurv entries of the survivor buffer under their keys, so every linear-probe chain is intact.
 * @param nsurv The survivors. */
static void table_reinsert(int nsurv) {
    table_clear(s_tab);
    s_count = 0;
    for (int k = 0; k < nsurv; ++k) {
        int found;
        uint32_t i = probe(s_surv[k].key, &found);
        if (i >= SLOTS) continue;   /* defensive: never full during rehash (nsurv <= SLOTS) */
        entry_move(&s_tab[i], &s_surv[k]);
        s_count++;
    }
}

/** @brief Whether @p v is one of the first @p n entries of @p list.
 * @param list The entries.
 * @param n Their count.
 * @param v The value.
 * @return 1 when present, else 0. */
static int index_in(const uint32_t* list, unsigned n, uint32_t v) {
    for (unsigned j = 0; j < n; ++j)
        if (list[j] == v) return 1;
    return 0;
}

static _Atomic(unsigned long) s_rekeyed;   /* entries moved to a new key, since startup */

unsigned long openpete_interp_objs_rekey_count(void) { return s_rekeyed; }

int openpete_interp_objs_rekey(const uint32_t* from, const uint32_t* to, unsigned n) {
    if (!s_tab || !n) return 0;
    /* Every lookup runs before any key changes, since a cleared or renamed
     * key breaks the probe chains through it; the rehash then restores the
     * chains. A chain of moves (a to b, b to c) moves both entries, and the
     * entry under b is not evicted. */
    enum { REKEY_MAX = 64 };
    if (n > REKEY_MAX) n = REKEY_MAX;
    uint32_t idx[REKEY_MAX], dsp[REKEY_MAX];
    unsigned m = 0;
    for (unsigned k = 0; k < n; ++k) {
        int found;
        uint32_t i = probe(from[k], &found);
        idx[k] = found ? i : SLOTS;
        i = probe(to[k], &found);
        dsp[k] = found ? i : SLOTS;
    }
    for (unsigned k = 0; k < n; ++k)
        if (idx[k] < SLOTS && dsp[k] < SLOTS && !index_in(idx, n, dsp[k])) {
            evicted_keep(&s_tab[dsp[k]]);
            s_tab[dsp[k]].key = 0u;   /* the rehash below drops it */
        }
    for (unsigned k = 0; k < n; ++k) {
        if (idx[k] >= SLOTS) continue;
        s_tab[idx[k]].key = to[k];
        m++;
    }
    if (!m) return 0;
    int nsurv = 0;
    for (uint32_t i = 0; i < SLOTS; ++i)
        if (s_tab[i].key) entry_move(&s_surv[nsurv++], &s_tab[i]);
    table_reinsert(nsurv);
    s_rekeyed += m;
    if (openpete_interp_objs_keylog())
        for (unsigned k = 0; k < n; ++k)
            if (idx[k] < SLOTS)
                fprintf(stderr, "[interp-objs] f=%u rekey %08x -> %08x\n", openpete_frame, from[k], to[k]);
    return (int)m;
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
            evicted_keep(&s_tab[i]);
        }
    }

    table_reinsert(nsurv);

    s_stat_tracked   = nsurv;
    s_stat_paired    = paired;
    s_stat_despawned = despawned;
}

void openpete_interp_objs_reset(void) {
    if (s_tab) table_clear(s_tab);
    s_ev_n = 0;
    s_tick_ns = s_tick_prev_ns = 0;
    s_count = 0;
    s_stat_tracked = s_stat_paired = s_stat_spawned = s_stat_despawned = 0;
}

const openpete_interp_obj_t* openpete_interp_objs_find(uint32_t key) {
    if (key == 0 || !s_tab) return NULL;
    int found;
    uint32_t i = probe(key, &found);
    return found ? &s_tab[i] : NULL;
}

const openpete_interp_obj_t* openpete_interp_objs_find_evicted(uint32_t key) {
    if (key == 0) return NULL;
    for (uint32_t i = 0; i < s_ev_n; ++i)
        if (s_ev[i].key == key) return &s_ev[i];
    return NULL;
}

unsigned long openpete_interp_objs_evicted_dropped(void) { return s_ev_dropped; }

int openpete_interp_objs_spawned_last_tick(const openpete_interp_obj_t* o) {
    return o && !o->have_prev && s_tick_prev_ns != 0 && o->curr_ns > s_tick_prev_ns;
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
