/* openpete_render_matcher.c — draw-call matcher (renderer-foundation
 * Phase 1). See openpete_render_matcher.h for the contract and
 * notes/plans/openpete-renderer-foundation.md for the architecture.
 *
 * Per-frame flow:
 *
 *   PsyCross OT walker (per prim):
 *     openpete_render_matcher_observe_{tri,quad}(p0, p1, ...)
 *       └── for each vertex: lookup s_prev → recover matrix_span_id +
 *           world-space MAC + ...
 *       └── find-or-create batch keyed by matrix_span_id
 *       └── accumulate bbox + centroid sum into batch
 *
 *   PsyCross PsyX_EndScene:
 *     openpete_render_matcher_finalize_frame()
 *       └── for each batch: centroid = sum/count; bbox_size_log2;
 *           hash = fnv1a(vertex_count, size_log2, centroid_quantized)
 *       └── build hash table over prev frame's batches
 *       └── for each curr batch: probe prev → if match: inherit
 *           object_id, compute velocity = curr.centroid - prev.centroid;
 *           else: assign fresh object_id
 *       └── count prev batches not consumed = dropped
 *       └── swap curr/prev
 *
 * Match-rate diagnostics flow through five counters that PsyX_EndScene
 * dumps once per frame when OPENPETE_RENDER_PROBE=1.
 */

#include "openpete_render_matcher.h"
#include "openpete_render_cache.h"

#include <stdint.h>
#include <string.h>

#define MATCHER_MAX_BATCHES 256u
#define MATCHER_HASH_SLOTS  512u  /* power-of-two */
#define MATCHER_PROBE_MAX   32u

typedef struct matcher_batch {
    uint64_t hash;
    uint32_t matrix_span_id;
    uint32_t object_id;
    uint32_t prim_count;
    uint32_t vertex_count;
    int32_t  bbox_min[3];
    int32_t  bbox_max[3];
    int64_t  centroid_sum[3];
    int32_t  centroid[3];
    int32_t  velocity[3];
    int      consumed;       /* used by finalize to count dropped prev batches */
} matcher_batch_t;

typedef struct matcher_frame {
    matcher_batch_t batches[MATCHER_MAX_BATCHES];
    uint32_t        count;
    /* hash_table[slot] = batch_index + 1 (0 = empty). Open-addressed
     * with linear probe. Rebuilt over prev frame at finalize. */
    uint16_t        hash_table[MATCHER_HASH_SLOTS];
} matcher_frame_t;

static matcher_frame_t  s_frame_a;
static matcher_frame_t  s_frame_b;
static matcher_frame_t* s_curr = &s_frame_a;
static matcher_frame_t* s_prev = &s_frame_b;

static uint32_t s_next_object_id = 1u;

unsigned g_openpete_render_matcher_batch_count             = 0u;
unsigned g_openpete_render_matcher_matched_count           = 0u;
unsigned g_openpete_render_matcher_unmatched_new_count     = 0u;
unsigned g_openpete_render_matcher_unmatched_dropped_count = 0u;
unsigned g_openpete_render_matcher_no_cache_count          = 0u;

/* Accumulators reset on every observe call's caller-decided frame
 * boundary — we use s_curr->count == 0 as the "fresh frame" signal.
 * find_or_create_batch initializes a slot with bbox sentinels. */
static matcher_batch_t* find_or_create_batch(uint32_t matrix_span_id) {
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        if (s_curr->batches[i].matrix_span_id == matrix_span_id)
            return &s_curr->batches[i];
    }
    if (s_curr->count >= MATCHER_MAX_BATCHES) return NULL;
    matcher_batch_t* b = &s_curr->batches[s_curr->count++];
    memset(b, 0, sizeof(*b));
    b->matrix_span_id = matrix_span_id;
    b->bbox_min[0] = b->bbox_min[1] = b->bbox_min[2] = INT32_MAX;
    b->bbox_max[0] = b->bbox_max[1] = b->bbox_max[2] = INT32_MIN;
    return b;
}

static inline uint32_t sxy_key_from_pair(const int16_t* p) {
    return ((uint32_t)(uint16_t)p[0]) | (((uint32_t)(uint16_t)p[1]) << 16);
}

static inline void accumulate_vertex(matcher_batch_t* b,
                                     const openpete_render_cache_entry_t* e) {
    for (int axis = 0; axis < 3; ++axis) {
        int32_t v = e->mac[axis];
        if (v < b->bbox_min[axis]) b->bbox_min[axis] = v;
        if (v > b->bbox_max[axis]) b->bbox_max[axis] = v;
        b->centroid_sum[axis] += (int64_t)v;
    }
    b->vertex_count++;
}

/* Observe one vertex: lookup s_prev → return entry by value. On miss
 * (vertex not in cache, e.g. line or sprite not produced by GTE),
 * bumps the no_cache counter and returns 0. */
static int observe_vertex(const int16_t* p, openpete_render_cache_entry_t* out) {
    uint32_t key = sxy_key_from_pair(p);
    if (openpete_render_cache_lookup_prev(key, out)) return 1;
    g_openpete_render_matcher_no_cache_count++;
    return 0;
}

static void observe_prim_n(const int16_t* const* pts, int n) {
    /* Need the first vertex's cache hit to identify the batch. If even
     * the first vertex isn't in s_prev (untextured line, sprite, debug
     * tri, …), skip the prim — it can't be matched without world data. */
    openpete_render_cache_entry_t first;
    if (!observe_vertex(pts[0], &first)) return;
    matcher_batch_t* b = find_or_create_batch(first.matrix_span_id);
    if (!b) return;
    b->prim_count++;
    accumulate_vertex(b, &first);
    for (int v = 1; v < n; ++v) {
        openpete_render_cache_entry_t e;
        if (observe_vertex(pts[v], &e)) accumulate_vertex(b, &e);
    }
}

void openpete_render_matcher_observe_tri(const int16_t* p0,
                                          const int16_t* p1,
                                          const int16_t* p2) {
    const int16_t* pts[3] = { p0, p1, p2 };
    observe_prim_n(pts, 3);
}

void openpete_render_matcher_observe_quad(const int16_t* p0,
                                           const int16_t* p1,
                                           const int16_t* p2,
                                           const int16_t* p3) {
    const int16_t* pts[4] = { p0, p1, p2, p3 };
    observe_prim_n(pts, 4);
}

static inline int ilog2_ceil_i64(int64_t v_signed) {
    uint64_t v = (v_signed < 0) ? (uint64_t)-v_signed : (uint64_t)v_signed;
    if (v <= 1) return 0;
    int r = 0;
    v -= 1;
    while (v) { v >>= 1; r++; }
    return r;
}

static uint64_t fnv1a64(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Centroid quantization shift. MAC is q12 (world * 4096). Shifting
 * right 18 = world / 64, so each bucket is 64 world units across —
 * big enough that single-frame motion stays in the same bucket
 * (~3-30 world units of motion at 30 Hz), small enough that distinct
 * objects in different parts of the level don't collide.
 *
 * Tuning history (level_10 f2200):
 *   shift=16 (16-unit buckets) + bbox_size_log2 + vc → 40 % match
 *   shift=18 (64-unit buckets), vc only             → see below */
#define MATCHER_CENTROID_SHIFT 18

void openpete_render_matcher_finalize_frame(void) {
    /* 1) Finalize each curr batch: centroid + bbox_size_log2 + hash. */
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* b = &s_curr->batches[i];
        if (b->vertex_count == 0) continue;
        for (int axis = 0; axis < 3; ++axis) {
            b->centroid[axis] =
                (int32_t)(b->centroid_sum[axis] / (int64_t)b->vertex_count);
        }
        int32_t cq[3] = {
            b->centroid[0] >> MATCHER_CENTROID_SHIFT,
            b->centroid[1] >> MATCHER_CENTROID_SHIFT,
            b->centroid[2] >> MATCHER_CENTROID_SHIFT,
        };
        /* Phase 1 hash: quantized centroid only.
         *
         * prim_count and vertex_count were attractive (per RT64) but
         * empirically jitter ±2..10 per frame even on stationary
         * batches (LOD / cull thresholds fluctuate). Including them
         * tanked match rate to 40 %. Bbox min/max have the same
         * problem.
         *
         * The centroid is averaged over all bbox vertices, so per-
         * vertex jitter cancels out — far more stable. Coarse
         * quantization (64 world units / bucket) absorbs object
         * motion up to ~64 units/frame, well above typical Spyro
         * mobility (~3-30 units/30Hz frame).
         *
         * Centroid-only collides for objects co-located on screen —
         * Phase 2 will narrow ambiguity by including a hash of the
         * first vertex's model-local position (stable invariant per
         * logical mesh, not affected by world transform). */
        struct {
            int32_t cx, cy, cz;
        } hk = { cq[0], cq[1], cq[2] };
        b->hash = fnv1a64(&hk, sizeof(hk));
    }

    /* 2) Build hash table over prev frame's batches. */
    memset(s_prev->hash_table, 0, sizeof(s_prev->hash_table));
    for (uint32_t i = 0; i < s_prev->count; ++i) {
        s_prev->batches[i].consumed = 0;
        uint64_t h = s_prev->batches[i].hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            if (s_prev->hash_table[slot] == 0) {
                s_prev->hash_table[slot] = (uint16_t)(i + 1u);
                break;
            }
            slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
        }
    }

    /* 3) Match curr batches against prev. */
    unsigned matched = 0u, unmatched_new = 0u;
    for (uint32_t i = 0; i < s_curr->count; ++i) {
        matcher_batch_t* b = &s_curr->batches[i];
        uint64_t h = b->hash;
        uint32_t slot = (uint32_t)(h & (MATCHER_HASH_SLOTS - 1u));
        int found = 0;
        for (uint32_t p = 0; p < MATCHER_PROBE_MAX; ++p) {
            uint16_t entry = s_prev->hash_table[slot];
            if (entry == 0) break;
            matcher_batch_t* pb = &s_prev->batches[entry - 1u];
            if (pb->hash == h && !pb->consumed) {
                pb->consumed = 1;
                b->object_id   = pb->object_id;
                b->velocity[0] = b->centroid[0] - pb->centroid[0];
                b->velocity[1] = b->centroid[1] - pb->centroid[1];
                b->velocity[2] = b->centroid[2] - pb->centroid[2];
                found = 1;
                matched++;
                break;
            }
            slot = (slot + 1u) & (MATCHER_HASH_SLOTS - 1u);
        }
        if (!found) {
            b->object_id = s_next_object_id++;
            b->velocity[0] = b->velocity[1] = b->velocity[2] = 0;
            unmatched_new++;
        }
    }

    /* 4) Count dropped prev batches. */
    unsigned unmatched_dropped = 0u;
    for (uint32_t i = 0; i < s_prev->count; ++i) {
        if (!s_prev->batches[i].consumed) unmatched_dropped++;
    }

    /* 5) Publish stats. */
    g_openpete_render_matcher_batch_count             = s_curr->count;
    g_openpete_render_matcher_matched_count           = matched;
    g_openpete_render_matcher_unmatched_new_count     = unmatched_new;
    g_openpete_render_matcher_unmatched_dropped_count = unmatched_dropped;

    /* 6) Swap prev / curr; reset new-curr for next observe pass. */
    matcher_frame_t* tmp = s_prev;
    s_prev = s_curr;
    s_curr = tmp;
    s_curr->count = 0u;
    /* no_cache_count is monotonic-per-frame; reset here so the next
     * frame's observe pass starts clean. */
    g_openpete_render_matcher_no_cache_count = 0u;
}
