/* openpete_render_cache.c — host-side world-space cache, 3-generation
 * ring. See openpete_render_cache.h and
 * notes/plans/openpete-renderer-foundation.md.
 *
 * Generation model:
 *   gen[current]  — receiving GTE writes this yield.
 *   gen[prev]     — last GAME FRAME's GTE writes. This is the cache the
 *                   sim-due DrawOTag AND intermediate renders look up.
 *                   The OT being walked at sim-due was built by last
 *                   game frame's GTE → its SXY values were emitted into
 *                   THIS buffer.
 *   gen[prev2]    — two game frames ago. Held only so the recycle path
 *                   has somewhere to land without overwriting prev while
 *                   intermediate renders are still hitting it.
 *
 * Rotation policy:
 *   Host loop calls openpete_render_cache_arm_rotate() before each
 *   yield. The arm is one-shot: it sets a pending flag, and the FIRST
 *   emit_vertex of the next yield consumes it (rotate-on-first-emit).
 *   PadVSync-only yields don't emit, so the pending flag carries over —
 *   no data destruction on no-GTE yields, no rotate per VBL on a 30 Hz
 *   game.
 *
 * Why first-emit (not pre-co_switch):
 *   - Pre-co_switch unconditional rotate destroyed data on PadVSync-only
 *     yields (see soak f3112-f3140 vs f1-f3000: 1540 hits vs 9234
 *     misses).
 *   - Post-DrawOTag rotate forces sim-due and intermediate to read from
 *     different generations (prev vs prev2), needing API divergence.
 *   - First-emit-per-yield is the unique timing where the SAME lookup
 *     API (lookup_prev) serves both sim-due AND intermediate without
 *     losing alignment with the saved OT.
 *
 * Linear-probe open addressing, key = packed int16 SXY (matches what
 * the game mfc2's into prim packets). seq counter is monotonic; logical
 * clear of recycled gen happens by bumping its seq, which makes every
 * slot read as "stale".
 */

#include "openpete_render_cache.h"
#include "openpete_time.h"
#include "openpete_ui_moby.h"
#include "openpete_vertex_trace.h"  /* openpete_spanlog_enabled/record */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Live Hor+ widescreen / FOV state, defined in core/features/widescreen.c
 * and consumed by the GTE hot path (widescreen_gte_inline.h). The PGXP/
 * interp reproject below must mirror the same widening the live
 * projection applied, since vd.px is an ABSOLUTE screen position that
 * the renderer uses verbatim. */
extern float openpete_disp_w_factor;   /* 0 / 1.0 = no Hor+ widening */
extern int   openpete_fov_h_override;  /* 0 = no --fov H override */

/* Day 4 Phase 1.3 / 2.5: per-game perspective-eligibility classifier.
 * NULL = default (canonical-only). games/<game>/ registers an override
 * at boot to filter on PC. */
int (*openpete_pgxp_classify_fn)(uint32_t gte_pc, int is_canonical) = NULL;

/* Seeded by run.c from the resolved --pgxp state; -1 until boot. The
 * perspective-W branch follows it by default (env-overridable). */
int g_openpete_pgxp_perspective = -1;

/* Day 4 Phase 1.0 instrumentation: dump unique (h, ofx, ofy) tuples
 * seen at vertex emit. Resolves the cyclorama question — does
 * func_8004EBA8 swap projection-context mid-frame, or does it use the
 * global frustum set at InitGeom() boot. Enabled by env
 * OPENPETE_PGXP_PROJCTX_DUMP=1. */
static int proj_ctx_dump_check_env(void) {
    static int s_state = -1;
    if (s_state < 0) {
        const char* v = getenv("OPENPETE_PGXP_PROJCTX_DUMP");
        s_state = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return s_state;
}

static void proj_ctx_observe(uint16_t h, int32_t ofx, int32_t ofy, uint32_t gte_pc) {
    if (!proj_ctx_dump_check_env()) return;

    /* Always-on h-watcher (Phase 1.0 cyclorama disprove). */
    {
        enum { H_TABLE_SZ = 8 };
        static uint16_t s_h_seen[H_TABLE_SZ];
        static unsigned s_h_seen_count = 0;
        int known = 0;
        for (unsigned i = 0; i < s_h_seen_count; ++i)
            if (s_h_seen[i] == h) { known = 1; break; }
        if (!known && s_h_seen_count < H_TABLE_SZ) {
            s_h_seen[s_h_seen_count++] = h;
            fprintf(stderr, "[PGXP-PROJCTX-H] new h=%u (distinct h's seen so far: %u)\n",
                    (unsigned)h, s_h_seen_count);
        }
    }

    /* Phase 1.1: also distinct PC tracker. Print every new calling PC
     * the cache emit sees — gives us the prim-class roster for Phase 2.5
     * gating. PC is stable per function for the duration of the run. */
    {
        enum { PC_TABLE_SZ = 64 };
        static uint32_t s_pc_seen[PC_TABLE_SZ];
        static uint32_t s_pc_count[PC_TABLE_SZ];
        static unsigned s_pc_seen_count = 0;
        unsigned i;
        for (i = 0; i < s_pc_seen_count; ++i) {
            if (s_pc_seen[i] == gte_pc) {
                s_pc_count[i]++;
                break;
            }
        }
        if (i == s_pc_seen_count && s_pc_seen_count < PC_TABLE_SZ) {
            s_pc_seen[s_pc_seen_count] = gte_pc;
            s_pc_count[s_pc_seen_count] = 1;
            s_pc_seen_count++;
            fprintf(stderr, "[PGXP-PC] new gte caller pc=0x%08x (distinct PCs: %u)\n",
                    gte_pc, s_pc_seen_count);
        }
    }

    enum { TABLE_SZ = 64 };
    static struct { uint16_t h; int32_t ofx; int32_t ofy; uint32_t count; } s_seen[TABLE_SZ];
    static unsigned s_seen_count = 0;
    static int s_table_full_warned = 0;

    for (unsigned i = 0; i < s_seen_count; ++i) {
        if (s_seen[i].h == h && s_seen[i].ofx == ofx && s_seen[i].ofy == ofy) {
            s_seen[i].count++;
            return;
        }
    }
    if (s_seen_count < TABLE_SZ) {
        s_seen[s_seen_count].h = h;
        s_seen[s_seen_count].ofx = ofx;
        s_seen[s_seen_count].ofy = ofy;
        s_seen[s_seen_count].count = 1;
        s_seen_count++;
        fprintf(stderr, "[PGXP-PROJCTX] new tuple #%u: h=%u ofx=%d (%.2f) ofy=%d (%.2f) first_pc=0x%08x\n",
                s_seen_count, (unsigned)h, ofx, (double)ofx / 65536.0,
                ofy, (double)ofy / 65536.0, gte_pc);
    } else if (!s_table_full_warned) {
        fprintf(stderr, "[PGXP-PROJCTX] tuple table full at %u entries; uniques suppressed (h/pc-watchers still active)\n",
                TABLE_SZ);
        s_table_full_warned = 1;
    }
}

typedef struct {
    uint32_t                       seq;
    openpete_render_cache_entry_t  slots[OPENPETE_RENDER_CACHE_SLOTS];
} openpete_render_cache_frame_t;

#define NUM_GENS 3

/* seq starts at 1 so BSS-zeroed slots (seq=0) read as empty on the
 * first frame's lookups. */
static openpete_render_cache_frame_t s_gens[NUM_GENS] = {
    { .seq = 1 }, { .seq = 1 }, { .seq = 1 },
};
static int s_current_idx = 0;

/* Rotate is one-shot: armed by host before yield, consumed by first
 * emit_vertex of the yield. PadVSync-only yields don't emit so the
 * arm carries over harmlessly. */
static int s_rotate_pending = 0;

/* Matrix-span detector state, scoped to the producer side. OFX/OFY
 * are part of the span key: HUD/text mobys position per-glyph via
 * OFX (not TR), so two adjacent wobbling letters with momentarily
 * identical rotation matrices would otherwise MERGE into one span —
 * the second letter's batch (and obj tag) vanished from that
 * generation, a wandering one-frame TAG_ABSENT per letter (the
 * F9 red letter flicker on DEMO MODE / menu text). */
static int16_t  s_last_rt[3][3];
static int32_t  s_last_tr[3];
static int32_t  s_last_ofx, s_last_ofy;
static int      s_matrix_state_initialized = 0;

unsigned g_openpete_render_cache_emit_count            = 0u;
unsigned g_openpete_render_cache_lookup_count          = 0u;
unsigned g_openpete_render_cache_hit_count             = 0u;
unsigned g_openpete_render_cache_probe_overflow_count  = 0u;
/* Same-pixel collision census (see emit_vertex): live same-key entry
 * evicted by a different object (harmful) / same object, different
 * vertex (benign neighbor). */
unsigned g_openpete_render_cache_collide_xobj           = 0u;
unsigned g_openpete_render_cache_collide_samobj         = 0u;
unsigned g_openpete_render_cache_matrix_span_id        = 0u;

/* Linker force-pull anchor for the renderer-foundation matcher.
 * See note in the file's prior incarnation — matcher.c.o lives in a
 * separate static archive that GNU ld won't revisit; this symbol
 * reference drags it along as a fellow-traveler. */
extern void openpete_render_matcher_finalize_frame(void);
__attribute__((used))
static void (*const s_openpete_render_matcher_force_link)(void) =
    openpete_render_matcher_finalize_frame;

static inline int gen_idx_prev(void) {
    return (s_current_idx + NUM_GENS - 1) % NUM_GENS;
}
/* Generation two game frames back; reserved for the recycle path. Not
 * read by sim-due or intermediate, but accessible for future plans
 * (e.g. quadratic-velocity prediction). */
static inline int gen_idx_prev2(void) {
    return (s_current_idx + NUM_GENS - 2) % NUM_GENS;
}

static inline openpete_render_cache_frame_t* gen_current(void) {
    return &s_gens[s_current_idx];
}
static inline openpete_render_cache_frame_t* gen_prev(void) {
    return &s_gens[gen_idx_prev()];
}
static inline openpete_render_cache_frame_t* gen_prev2(void) {
    return &s_gens[gen_idx_prev2()];
}

/* Monotonic rotation count — see header. */
static uint32_t s_rotation_count = 0u;
uint32_t openpete_render_cache_rotation_count(void) {
    return s_rotation_count;
}

static void consume_pending_rotate(void) {
    if (!s_rotate_pending) return;
    s_rotate_pending = 0;
    s_rotation_count++;
    s_current_idx = (s_current_idx + 1) % NUM_GENS;
    /* Logical clear of the newly-rotated-in slot (which holds gen[F-2]
     * data and is about to be overwritten with gen[F] writes). The
     * other two generations keep their seqs intact so prev and prev2
     * lookups stay valid. */
    s_gens[s_current_idx].seq++;
    /* Matrix-span detector resets so the first emit of the new game
     * frame always registers as a fresh span. */
    s_matrix_state_initialized = 0;
}

static inline uint32_t hash_sxy(uint32_t key) {
    uint32_t h = key ^ (key >> 16);
    h *= 0x9E3779B1u;
    return h & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
}

extern int  openpete_vstab_enabled(void);
extern void openpete_vstab_record_vertex(int16_t v_local[3], const int32_t mac[3]);

void openpete_render_cache_emit_vertex(const openpete_render_cache_entry_t* src) {
    /* First-emit-per-yield rotate. No-op if not armed. */
    consume_pending_rotate();

    proj_ctx_observe(src->h, src->ofx, src->ofy, src->gte_pc);

    /* Per-vertex stability trace (no-op unless OPENPETE_VERT_STABILITY_LOG
     * env is set). Called BEFORE span detection so vert_idx accounting
     * uses the freshest span id. */
    if (openpete_vstab_enabled()) {
        int16_t vl[3] = { src->v_local[0], src->v_local[1], src->v_local[2] };
        openpete_vstab_record_vertex(vl, src->mac);
    }

    /* Detect transform change against last-emitted RT/TR/OFX/OFY. */
    if (!s_matrix_state_initialized ||
        memcmp(s_last_rt, src->rt, sizeof(s_last_rt)) != 0 ||
        s_last_tr[0] != src->tr[0] ||
        s_last_tr[1] != src->tr[1] ||
        s_last_tr[2] != src->tr[2] ||
        s_last_ofx != src->ofx ||
        s_last_ofy != src->ofy) {
        g_openpete_render_cache_matrix_span_id++;
        memcpy(s_last_rt, src->rt, sizeof(s_last_rt));
        s_last_tr[0] = src->tr[0];
        s_last_tr[1] = src->tr[1];
        s_last_tr[2] = src->tr[2];
        s_last_ofx = src->ofx;
        s_last_ofy = src->ofy;
        s_matrix_state_initialized = 1;
        /* G3: one RT/TR record per span bump (no-op unless
         * OPENPETE_SPAN_LOG set). id already incremented above. */
        if (openpete_spanlog_enabled())
            openpete_spanlog_record(src->rt, src->tr, src->v_local,
                                    src->sxy_key, src->obj_tag);
    }

    openpete_render_cache_frame_t* curr = gen_current();
    const uint32_t key = src->sxy_key;
    uint32_t idx = hash_sxy(key);
    const uint32_t curr_seq = curr->seq;

    /* Same-pixel multi-entry policy: ~36% of gameplay emits land on a
     * pixel that already holds a live entry (census 2026-06-10:
     * ~840/frame cross-OBJECT + ~925 same-object). The old
     * most-recent-wins overwrite made the evicted vert resolve the
     * survivor at lookup — riding another object's motion (the
     * harmful class) or a neighbor's. Distinct identities
     * (obj_tag, world_vkey) now COEXIST along the probe run (capped
     * at SAME_KEY_MAX so a distant-terrain pinch pixel can't flood
     * its neighborhood); consumers disambiguate per prim (the OT
     * walker's consensus-tag pass) via lookup_*_multi. */
    /* §13.15 bucket-depth hypothesis FALSIFIED (2026-06-12): depth
     * 4→8 (and table 32k→64k) left Stone Hill ground cyan unchanged
     * and grew probe-overflow — the missing corner verts were never
     * EMITTED under the missing span (the game routes a shared quad
     * corner through exactly one adjacent quad per frame, and which
     * one migrates), so no cache depth can retain what never arrived.
     * The real fix is the matcher's span-free v_local fallback map.
     * Env knob retained for future A/B; default = the original 4. */
    static int s_same_key_max = -1;
    if (s_same_key_max < 0) {
        const char* e = getenv("OPENPETE_SAME_KEY_MAX");
        s_same_key_max = (e && *e) ? atoi(e) : 4;
        if (s_same_key_max < 1)  s_same_key_max = 1;
        if (s_same_key_max > 16) s_same_key_max = 16;
    }
    const unsigned SAME_KEY_MAX = (unsigned)s_same_key_max;
    openpete_render_cache_entry_t* last_same_key = NULL;
    unsigned same_key_n = 0;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_PROBE_MAX; ++i) {
        openpete_render_cache_entry_t* slot = &curr->slots[idx];
        if (slot->seq != curr_seq) {
            *slot = *src;
            slot->seq = curr_seq;
            slot->matrix_span_id = g_openpete_render_cache_matrix_span_id;
            g_openpete_render_cache_emit_count++;
            return;
        }
        if (slot->sxy_key == key) {
            if (slot->obj_tag == src->obj_tag &&
                slot->world_vkey == src->world_vkey) {
                /* Same logical vertex re-emitted — most-recent wins. */
                *slot = *src;
                slot->seq = curr_seq;
                slot->matrix_span_id = g_openpete_render_cache_matrix_span_id;
                g_openpete_render_cache_emit_count++;
                return;
            }
            /* Different identity on the same pixel: census + coexist. */
            if (slot->obj_tag != src->obj_tag)
                g_openpete_render_cache_collide_xobj++;
            else
                g_openpete_render_cache_collide_samobj++;
            last_same_key = slot;
            if (++same_key_n >= (unsigned)SAME_KEY_MAX) {
                /* Cap reached — replace the most recent same-key. */
                *last_same_key = *src;
                last_same_key->seq = curr_seq;
                last_same_key->matrix_span_id =
                    g_openpete_render_cache_matrix_span_id;
                g_openpete_render_cache_emit_count++;
                return;
            }
        }
        idx = (idx + 1u) & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
    }
    g_openpete_render_cache_probe_overflow_count++;
}

static int lookup_frame(const openpete_render_cache_frame_t* frame,
                        uint32_t sxy_key,
                        openpete_render_cache_entry_t* out) {
    g_openpete_render_cache_lookup_count++;
    uint32_t idx = hash_sxy(sxy_key);
    const uint32_t frame_seq = frame->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_PROBE_MAX; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[idx];
        if (slot->seq != frame_seq) return 0;
        if (slot->sxy_key == sxy_key) {
            if (out) *out = *slot;
            g_openpete_render_cache_hit_count++;
            return 1;
        }
        idx = (idx + 1u) & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
    }
    return 0;
}

/* Multi-entry variant: collect every live same-key entry along the
 * probe run (same-pixel coexistence, see emit_vertex). Returns the
 * count; out[0] is the first-inserted (oldest) candidate. */
static int lookup_frame_multi(const openpete_render_cache_frame_t* frame,
                              uint32_t sxy_key,
                              openpete_render_cache_entry_t* out,
                              int max) {
    int n = 0;
    uint32_t idx = hash_sxy(sxy_key);
    const uint32_t frame_seq = frame->seq;
    g_openpete_render_cache_lookup_count++;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_PROBE_MAX; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[idx];
        if (slot->seq != frame_seq) break;
        if (slot->sxy_key == sxy_key && n < max) {
            out[n++] = *slot;
        }
        idx = (idx + 1u) & (OPENPETE_RENDER_CACHE_SLOTS - 1u);
    }
    if (n) g_openpete_render_cache_hit_count++;
    return n;
}

int openpete_render_cache_lookup_prev2_multi(uint32_t sxy_key,
                                             openpete_render_cache_entry_t* out,
                                             int max) {
    return lookup_frame_multi(gen_prev2(), sxy_key, out, max);
}

int openpete_render_cache_lookup_curr(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out) {
    return lookup_frame(gen_current(), sxy_key, out);
}

int openpete_render_cache_lookup_prev(uint32_t sxy_key,
                                      openpete_render_cache_entry_t* out) {
    return lookup_frame(gen_prev(), sxy_key, out);
}

/* Interpolation "from" endpoint. At DrawOTag#N the lagged OT we display
 * (OT#(N-1), built by GTE G(N-1)) has SXY keys that were emitted into
 * the generation now sitting at prev2 (= G(N-1)); gen_prev holds G(N)
 * = the "to" endpoint. See notes/plans/openpete-interpolation-frame-
 * gen.md "Architecture". */
int openpete_render_cache_lookup_prev2(uint32_t sxy_key,
                                       openpete_render_cache_entry_t* out) {
    return lookup_frame(gen_prev2(), sxy_key, out);
}

/* OT-walker entry point. Target = gen[prev] = last game frame's GTE
 * writes, which is the source of the SXY values in the OT being
 * walked. 98% hit rate empirically (level_10 active gameplay). */
int openpete_render_cache_lookup_curr_raw(uint32_t sxy_key) {
    return lookup_frame(gen_prev(), sxy_key, NULL);
}

/* Float-project one cache entry (PGXP core math, factored out of the
 * keyed lookups so the matcher can project the interpolation "to"
 * endpoint from an entry it already resolved via the tomap). */
int openpete_render_cache_pgxp_project(const openpete_render_cache_entry_t* ep,
                                       openpete_pgxp_lookup_t* out) {
    const openpete_render_cache_entry_t e = *ep;

    /* Compute float-precision SX/SY in PSX pixel space by mirroring
     * the PSX integer projection (gte_rtps_internal lines 247-256)
     * in float arithmetic. Vertex screen positions stop quantizing
     * to int16 SXY pixels — the wobble that motivated PGXP.
     *
     * The upstream PsyCross PGXP path (PsyX_GTE.cpp::PGXP_EmitCacheData
     * + the `(px+0.5)*scr_h` shader perspective branch) was tuned for
     * a different game's GTE magnitudes; for Spyro it produces NDC
     * values 3+ orders of magnitude off-screen. Float-project here
     * directly and let the existing ortho fallback shader path
     * consume the result. PGXP-Z-buffer mode and texture correction
     * are deferred to a follow-up (see openpete-pgxp.md). */
    const float fmac1 = (float)e.mac[0];
    const float fmac2 = (float)e.mac[1];
    const float fmac3 = (float)e.mac[2];
    /* SZ is clamped positive by the integer GTE (Lm_D). Mirror that
     * to avoid divide-by-zero for verts at / behind the near plane —
     * the resulting projection is approximate but those verts are
     * clipped/culled by downstream stages anyway. */
    const float fsz = (fmac3 > 1.0f) ? fmac3 : 1.0f;
    /* Mirror the live GTE H override (widescreen_eff_h): --fov swaps H,
     * --aspect leaves it native. The cache stores canonical H, so apply
     * the override here too or PGXP'd verts under --fov project at the
     * wrong scale. Identity (== e.h) when no override is set. */
    const float fH = (openpete_fov_h_override != 0)
                     ? (float)openpete_fov_h_override
                     : (float)e.h;
    const float h_div_sz = fH / fsz;
    /* OFX/OFY are stored as raw Q16 ints in the cache; divide to get
     * pixel units, matching the integer projection's `>> 16`. */
    float fofx = (float)e.ofx * (1.0f / 65536.0f);
    const float fofy = (float)e.ofy * (1.0f / 65536.0f);

    /* Mirror the live Hor+ OFX widening (widescreen_eff_ofx). The cache
     * deliberately stores the CANONICAL (pre-Hor+) OFX (for the
     * is_canonical discriminator + aspect-independent semantics), but
     * out->px is an ABSOLUTE screen position the renderer writes to
     * v->x verbatim — so it must match the widened live projection.
     * Without this, every PGXP-hit vertex lands at the canonical 4:3
     * origin (left-aligned under PsyCross's left-anchored widened ortho)
     * while PGXP-miss verts keep the widened int16 SXY, so triangles
     * spanning the two stretch toward the left edge. UI mobys get the
     * constant centring shift; everything else gets the proportional
     * widening — exactly as widescreen_eff_ofx does live. Identity when
     * disp_w_factor <= 1.0 (no --aspect), so 4:3 is byte-unchanged. */
    if (openpete_disp_w_factor > 1.0f) {
        /* §13.16: normalized screen-anchored text tags carry the UI
         * marker (bit28|bit27) instead of an address — the live RTPS
         * centred them via is_current_ui, and the mirror must agree
         * or PGXP-hit glyph verts land at the WIDENED position while
         * the int16 path stays centred (the two-position HUD
         * flicker). */
        if (openpete_ui_moby_is_addr_ui(e.obj_tag) ||
            (e.obj_tag & 0x18000000u) == 0x18000000u) {
            fofx += (float)((openpete_disp_w_factor - 1.0f) * 256.0f);
        } else {
            fofx *= openpete_disp_w_factor;
        }
    }

    /* Float-precision SXY in PSX pixel space (same coord space the
     * int16 SXY path produces, just with sub-pixel detail preserved). */
    out->px = fofx + fmac1 * h_div_sz;
    out->py = fofy + fmac2 * h_div_sz;
    /* Day 3 — intra-batch perturbation is zero. Spyro's draw-call
     * vertex order isn't depth-sorted (the player mesh has overlap
     * between body parts at the same screen pixel; LEQUAL test on
     * unsorted intra-batch real Z rejects fragments and visibly
     * breaks the character model). The per-split uniform
     * u_pgxp_z_bias carries the OT-order signal; intra-batch verts
     * all share that bias, so they degenerate to OT/triangle-order
     * which is what Spyro's renderer expects.
     *
     * Real-Z intra-batch tiebreaking (for gem-pile / ledge Z-fight
     * resolution) requires per-primitive opt-in by prim type or
     * region — that's a follow-up. */
    out->pz = 0.0f;
    out->ofx = fofx;             /* unused in ortho path */
    out->ofy = fofy;             /* unused in ortho path */

    /* Day 4 Phase 2: canonical-context discriminator (OFF by default).
     * Phase 1.0 found h=341 is the only frustum and non-canonical
     * (ofx, ofy) are the PSYQ 2D-via-GTE glyph-emit idiom. But that
     * discriminator turned out to be necessary-not-sufficient: the
     * cyclorama, authored-affine world surfaces, and Spyro's character
     * mesh all hit the canonical context yet want different rendering
     * semantics. Phase 2 stays in tree as plumbing for the eventual
     * per-prim-class selective gating (Phase 2.5), gated behind
     * OPENPETE_PGXP_ENABLE_PERSPECTIVE=1. Shipping default is
     * Day 1c (float-projected SXY, ortho path) + Day 3 v2 (per-split
     * depth bias). */
    static int s_enable_persp = -1;
    if (s_enable_persp < 0) {
        /* Default: follow --pgxp (seeded by run.c). The env remains an
         * override in either direction (the old experiment gate,
         * promoted 2026-06-10 after the level-10 missing-geo
         * regression failed to reproduce — see openpete-pgxp.md). */
        const char* v = getenv("OPENPETE_PGXP_ENABLE_PERSPECTIVE");
        if (v && v[0]) s_enable_persp = (v[0] != '0') ? 1 : 0;
        else s_enable_persp = (g_openpete_pgxp_perspective > 0) ? 1 : 0;
    }
    if (!s_enable_persp) {
        out->scr_h = 0.0f;
        return 1;
    }
    const int32_t k_canonical_ofx_q16 = (int32_t)(256u << 16);
    const int32_t k_canonical_ofy_q16 = (int32_t)(120u << 16);
    const int is_canonical = (e.ofx == k_canonical_ofx_q16 &&
                              e.ofy == k_canonical_ofy_q16);
    /* Phase 2.5: game-specific per-PC classifier decides whether each
     * vertex should run perspective or stay on ortho. */
    int wants_perspective = is_canonical;
    if (openpete_pgxp_classify_fn) {
        wants_perspective = openpete_pgxp_classify_fn(e.gte_pc, is_canonical);
    }

    /* Diagnostic: per-PC perspective vs ortho counters. Enabled by
     * OPENPETE_PGXP_LOOKUP_DUMP=1. Helps verify the classifier
     * actually runs on the PCs we expect (i.e., does this prim type
     * even reach the lookup path?). */
    {
        static int s_dump_enabled = -1;
        if (s_dump_enabled < 0) {
            const char* v = getenv("OPENPETE_PGXP_LOOKUP_DUMP");
            s_dump_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_dump_enabled) {
            enum { N = 64 };
            static uint32_t pc_seen[N];
            static uint32_t pc_persp[N];
            static uint32_t pc_ortho[N];
            static unsigned n_seen = 0;
            unsigned i;
            for (i = 0; i < n_seen; ++i) if (pc_seen[i] == e.gte_pc) break;
            if (i == n_seen && n_seen < N) {
                pc_seen[n_seen] = e.gte_pc;
                pc_persp[n_seen] = 0;
                pc_ortho[n_seen] = 0;
                n_seen++;
                fprintf(stderr, "[PGXP-LOOKUP] new pc=0x%08x seen at lookup (canonical=%d)\n",
                        e.gte_pc, is_canonical);
            }
            if (i < N) {
                if (wants_perspective) pc_persp[i]++; else pc_ortho[i]++;
            }
        }
    }

    out->scr_h = wants_perspective ? fsz : 0.0f;
    return 1;
}

/* Keyed PGXP lookup for the OT-walker (ApplyVertexPGXP). MUST search
 * the generation the walked OT was emitted from, or moving vertices
 * miss (fall back to int16 wobble) — or worse, collide with a
 * DIFFERENT vertex that landed on the same int SXY in the newer
 * generation, float-positioning the vertex somewhere wrong entirely.
 * With frame-gen ON the displayed OT is the LAGGED one (OT#(N-1),
 * psyq_thunks openpete_render_walk_lagged_ot) → its keys live in
 * gen_prev2; with the 'I' kill switch OFF the current OT is walked →
 * gen_prev. Searching gen_prev unconditionally (the pre-2026-06-09
 * behavior, written before the lagged walk existed) was the "world
 * geometry unstable under camera motion with PGXP on" bug —
 * transform-interp plan §11 F5. */
int openpete_render_cache_lookup_pgxp(uint32_t sxy_key,
                                      openpete_pgxp_lookup_t* out) {
    extern int g_openpete_interp_enabled;
    openpete_render_cache_entry_t e;
    const int hit = g_openpete_interp_enabled
        ? openpete_render_cache_lookup_prev2(sxy_key, &e)
        : openpete_render_cache_lookup_prev(sxy_key, &e);
    if (!hit) return 0;
    return openpete_render_cache_pgxp_project(&e, out);
}

/* Interpolation "to"-endpoint projection: the matcher resolves a lagged
 * vertex's K-frame int SXY via the tomap, then float-projects the
 * matching gen_prev (G(N)) entry here so the substitute delta can be
 * computed float-endpoint-to-float-endpoint (PGXP-consistent) instead
 * of int-to-int on a float base (which re-quantized every intermediate
 * present to ±1px of the float path — 30 Hz subpixel oscillation). */
int openpete_render_cache_lookup_pgxp_to(uint32_t sxy_key,
                                         openpete_pgxp_lookup_t* out) {
    openpete_render_cache_entry_t e;
    if (!openpete_render_cache_lookup_prev(sxy_key, &e)) return 0;
    return openpete_render_cache_pgxp_project(&e, out);
}

void openpete_render_cache_for_each_curr_entry(
        openpete_render_cache_iter_fn cb, void* userdata) {
    const openpete_render_cache_frame_t* frame = gen_current();
    const uint32_t seq = frame->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_SLOTS; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[i];
        if (slot->seq == seq) cb(slot, userdata);
    }
}

void openpete_render_cache_for_each_prev_entry(
        openpete_render_cache_iter_fn cb, void* userdata) {
    const openpete_render_cache_frame_t* frame = gen_prev();
    const uint32_t seq = frame->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_SLOTS; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[i];
        if (slot->seq == seq) cb(slot, userdata);
    }
}

/* "from" endpoint scan (gen_prev2 = G(N-1)). Paired with the
 * for_each_prev scan (gen_prev = G(N) = "to") inside the matcher's
 * interpolation prepare pass. */
void openpete_render_cache_for_each_prev2_entry(
        openpete_render_cache_iter_fn cb, void* userdata) {
    const openpete_render_cache_frame_t* frame = gen_prev2();
    const uint32_t seq = frame->seq;
    for (unsigned i = 0; i < OPENPETE_RENDER_CACHE_SLOTS; ++i) {
        const openpete_render_cache_entry_t* slot = &frame->slots[i];
        if (slot->seq == seq) cb(slot, userdata);
    }
}

void openpete_render_cache_arm_rotate(void) {
    s_rotate_pending = 1;
}

/* Legacy no-deferred rotate. Retained for tests that pre-date the
 * arm_rotate split; advances the ring immediately. */
void openpete_render_cache_begin_frame(void) {
    s_rotate_pending = 1;
    consume_pending_rotate();
}

/* Phase 2.1 — sim-tick boundary state. The interpolation period is the
 * MEASURED interval between consecutive mark_now() calls (= actual
 * DrawOTag cadence), not a hardcoded constant. The old fixed 2×VBL
 * assumed a 30 fps game frame; when the game draws every VBL (~60 Hz)
 * the real window is ~16.6 ms, so alpha = elapsed / 33.3 ms could only
 * reach ~0.5 before mark_now reset it — the displayed position
 * sawtoothed 0→~0.5·step→0 every frame instead of completing a smooth
 * 0→1 sweep (judder, not interpolation). Measuring the cadence makes
 * alpha sweep a true 0→~1 regardless of the game's draw rate. */
static openpete_op_time_t s_last_sim_tick_ns   = 0;
static openpete_op_time_t s_last_sim_period_ns = 0;
static openpete_op_time_t s_prev_mark_ns       = 0;

/* Clamp the measured cadence to a sane band so a load hitch / scene
 * transition can't poison the period (8 ms ≈ 120 Hz, 50 ms ≈ 20 Hz). */
#define OPENPETE_ALPHA_PERIOD_MIN_NS  8000000
#define OPENPETE_ALPHA_PERIOD_MAX_NS 50000000

void openpete_render_sim_tick_advance_time(openpete_op_time_t sim_tick_ns,
                                           openpete_op_time_t sim_period_ns) {
    /* STARTUP SEED ONLY. This is called every sim-due iter (~60/s, one
     * per PSX VBL), but mark_now() — fired at the actual DrawOTag
     * (~30/s, the real keyframe boundary) — is the sole authority for
     * the alpha origin. If advance_time kept resetting s_last_sim_tick_ns
     * every VBL, the ~30/s sim-due-no-draw iters would re-anchor the
     * origin to "just now", collapsing alpha to ~0 for the intermediates
     * that follow → substitute applies ~0 delta → no sub-frame motion
     * (the "still looks 30 fps / no blue" symptom). So only set the
     * origin/period before the first mark_now has ever run. */
    if (s_prev_mark_ns == 0) {
        if (s_last_sim_tick_ns <= 0)   s_last_sim_tick_ns   = sim_tick_ns;
        if (s_last_sim_period_ns <= 0) s_last_sim_period_ns = sim_period_ns;
    }
}

void openpete_render_sim_tick_rotate_cache(void) {
    /* Compat shim — arm the deferred rotate so the next yield's first
     * emit consumes it. Old callers that expected immediate rotation
     * are gone (run.c calls arm_rotate directly now). */
    openpete_render_cache_arm_rotate();
}

float g_openpete_substitute_alpha_snapshot = 0.0f;

void openpete_render_sim_tick_mark_now(void) {
    openpete_op_time_t now = op_now();
    /* Measure the actual draw cadence: interval since the previous
     * mark_now (= previous DrawOTag). EMA-smooth (3:1) so a one-frame
     * hitch doesn't wobble the period (which would itself judder), but
     * a sustained 30↔60 Hz cadence change is tracked within a few
     * frames. Clamped to the sane band. */
    if (s_prev_mark_ns > 0) {
        openpete_op_time_t iv = now - s_prev_mark_ns;
        if (iv < OPENPETE_ALPHA_PERIOD_MIN_NS) iv = OPENPETE_ALPHA_PERIOD_MIN_NS;
        if (iv > OPENPETE_ALPHA_PERIOD_MAX_NS) iv = OPENPETE_ALPHA_PERIOD_MAX_NS;
        s_last_sim_period_ns = (s_last_sim_period_ns > 0)
            ? (s_last_sim_period_ns * 3 + iv) / 4
            : iv;
    }
    s_prev_mark_ns     = now;
    s_last_sim_tick_ns = now;
    g_openpete_substitute_alpha_snapshot = 0.0f;
}

/* §5 / #24 camera-cut snap. Armed per sim tick by the game-side
 * discontinuity detector (game_on_frame — the g_Camera layout is
 * game-specific, so the PSX-RAM read lives there, not in core/).
 * While armed, every intermediate render of the current display
 * cycle pins alpha to 0: the lagged OT presents unchanged instead of
 * lerping toward a post-cut frame — a clean snap, not a full-screen
 * smear. Re-evaluated (set or cleared) every tick, so the flag's
 * lifetime is exactly the one display cycle that spans the cut. */
static int s_interp_snap_cycle = 0;

int openpete_render_interp_snap_get(void) {
    return s_interp_snap_cycle;
}

void openpete_render_interp_snap_set(int snap) {
    s_interp_snap_cycle = snap ? 1 : 0;
}

void openpete_render_substitute_alpha_snapshot_intermediate(void) {
    g_openpete_substitute_alpha_snapshot = s_interp_snap_cycle
        ? 0.0f
        : openpete_render_compute_alpha(op_now());
}

float openpete_render_compute_alpha(openpete_op_time_t now_ns) {
    if (s_last_sim_period_ns <= 0) return 0.0f;
    openpete_op_time_t delta = now_ns - s_last_sim_tick_ns;
    if (delta <= 0) return 0.0f;
    if (delta >= s_last_sim_period_ns) return 1.0f;
    return (float)((double)delta / (double)s_last_sim_period_ns);
}
