/* openpete_world_geo.c — see openpete_world_geo.h. */

#include "openpete_world_geo.h"

#include <stdlib.h>   /* getenv — §13.12 A/B kill switch */

static uint32_t s_current = 0;
static uint32_t s_vidx    = 0;   /* per-strip emission ordinal (fallback) */

/* PHASE-D vertex-source FIFO. Depth stays at the pipeline's prefetch
 * overhang (<=3); 16 slots absorb any variant. Power-of-two ring. */
#define VSRC_RING 16u
static uint32_t s_vsrc[VSRC_RING];
static uint32_t s_vsrc_head = 0, s_vsrc_tail = 0;
static int      s_refine    = 0;
static uint32_t s_last_vkey = 0;

/* §13.7c content-derived quad identity: FALSIFIED 2026-06-10. A
 * gather-entry hook fed hash(strip, packed corner-index word) tags
 * for the subdivided tri/quad variants (theory: positional list-
 * entry tags mispair whole quads on visible-set churn — the tier-3/4
 * harness tail). Tags demonstrably changed (capture md5 differed)
 * yet every midpoint-harness metric was identical to 4 digits over
 * 29M vert-frames: positional tags are already frame-stable in
 * practice, and the tier tail is legitimate near-geometry curvature.
 * Hook removed; see the plan §13.7c record. */

void openpete_world_geo_set_current(uint32_t strip_addr) {
    s_current   = strip_addr;
    s_vidx      = 0;             /* new strip → restart ordinal */
    s_vsrc_head = s_vsrc_tail = 0;   /* drop any prefetch overhang */
    s_refine    = 0;
}

void openpete_world_geo_clear(void) {
    s_current   = 0;
    s_vidx      = 0;
    s_vsrc_head = s_vsrc_tail = 0;
    s_refine    = 0;
}

uint32_t openpete_world_geo_get_current(void) {
    return s_current;
}

void openpete_world_geo_push_vsrc(uint32_t src_addr) {
    if (!s_current) return;      /* not in a tagged strip — ignore */
    /* Stamp the source-key class bit explicitly: KSEG0 addresses
     * carry it for free, but the subdivided-quad variants push
     * SCRATCHPAD slot addresses (0x1F8000xx, bit31 clear) — equally
     * stable per-vertex identities that must classify as source
     * keys (gate-exempt), not counter keys. */
    s_vsrc[s_vsrc_head & (VSRC_RING - 1u)] =
        src_addr | OPENPETE_WORLD_VKEY_SRC_BIT;
    s_vsrc_head++;
}

/* §13.7 subdivided-quad SUBDIV-TIER key namespaces. The four quad
 * variants (A–D = subdivision tiers) share each quad's strip tag AND
 * overlap scratchpad slot addresses, so when a quad flips tier with
 * camera distance, a surviving slot key (e.g. 0x1F800118) names a
 * DIFFERENT logical vertex on the other tier — the per-vertex tomap
 * then pairs across the two representations (midpoint harness: the
 * tier-3 RTPS measured mean 3.4 px with 1327 px spikes). Namespacing
 * the slot keys per tier makes a tier flip MISS cleanly (→ rigid
 * fill / snap) instead of cross-pairing, exactly the
 * REFINE-bit precedent. (§13.7b intra-strip slot-aliasing hypothesis
 * was FALSIFIED 2026-06-10: per-tier push-seq keys gave a
 * bit-identical 29M-vert harness capture both on and off — slot keys
 * are already unique within a matrix span; heads fire per quad.)
 *
 * §13.12 TIER-INDEPENDENT LATTICE KEYS (supersedes the per-tier slot
 * namespaces). The four variants are fixed serializations of one
 * quarter-unit lattice over the quad/tri (derived from the
 * r_environment.s synthesis chains; corner fields map identically
 * across tiers: srl22→(0,0), srl14→(0,4), sll2→(4,0), srl6→(4,4)):
 *   t1 quad 3x3:  k → (2(k/3), 2(k%3))     (even sublattice of t3)
 *   t3 quad 5x5:  k → (k/5, k%5)
 *   t2 tri ord-2: k → {(0,0),(0,2),(0,4),(2,0),(2,2),(4,0)}
 *   t4 tri ord-4: decreasing-row-major, row starts {0,5,9,12,14}
 * Keying by (i<<4)|j quarters instead of (tier, slot) makes a quad's
 * camera-distance TIER FLIP pair its shared lattice points EXACTLY
 * (a t1↔t3 flip pairs all 9 coarse points; only the fine-only points
 * miss into the weld/mean) — previously every vert missed for a
 * frame and the nearest ground quad (which flips constantly) rode
 * the batch mean: the user-visible bottom-screen cyan patch + seam
 * that survived the §13.11 gate retirement. Also makes the key
 * independent of the emission pattern, retiring the intra-tier
 * pattern-reshuffle residual. bit22 marks the lattice namespace —
 * a real stream source address with bit31+bit22 set would be
 * ≥0x8040xxxx, outside the 2MB PSX RAM, so the families can't
 * collide. The terminator slot's prefetch push (dropped at the next
 * head's FIFO reset, never consumed) gets a per-tier sentinel.
 * Env OPENPETE_QUAD_LATTICE=0 restores (tier, slot) keys for A/B. */
static int quad_lattice_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_QUAD_LATTICE");
                 v = (e && *e) ? (e[0] != '0') : 1; }
    return v;
}
static uint32_t lattice_ij(uint32_t tier, uint32_t k) {
    switch (tier) {
    case 1u: return k < 9u
        ? ((2u * (k / 3u)) << 4) | (2u * (k % 3u)) : 0x100u;
    case 3u: return k < 25u
        ? ((k / 5u) << 4) | (k % 5u) : 0x100u;
    case 2u: {
        static const uint8_t t2[6] = {0x00,0x02,0x04,0x20,0x22,0x40};
        return k < 6u ? t2[k] : 0x100u;
    }
    case 4u: {
        static const uint8_t row_start[5] = {0,5,9,12,14};
        uint32_t i;
        if (k >= 15u) return 0x100u;
        for (i = 4u; row_start[i] > k; --i) {}
        return (i << 4) | (k - row_start[i]);
    }
    default: return 0x100u;
    }
}
static void push_vsrc_tier(uint32_t tier, uint32_t src_addr) {
    uint32_t key;
    if (!s_current) return;
    if (quad_lattice_on()) {
        uint32_t k  = (src_addr & 0xFFFFu) >> 4;
        uint32_t ij = lattice_ij(tier, k);
        key = (ij <= 0xFFu)
            ? OPENPETE_WORLD_VKEY_SRC_BIT | (1u << 22) | ij
            : OPENPETE_WORLD_VKEY_SRC_BIT | (1u << 22) | 0x3F00u | tier;
    } else {
        key = OPENPETE_WORLD_VKEY_SRC_BIT | (tier << 20)
            | (src_addr & 0xFFFFu);
    }
    s_vsrc[s_vsrc_head & (VSRC_RING - 1u)] = key;
    s_vsrc_head++;
}
void openpete_world_geo_push_vsrc_t1(uint32_t a) { push_vsrc_tier(1u, a); }
void openpete_world_geo_push_vsrc_t2(uint32_t a) { push_vsrc_tier(2u, a); }
void openpete_world_geo_push_vsrc_t3(uint32_t a) { push_vsrc_tier(3u, a); }
void openpete_world_geo_push_vsrc_t4(uint32_t a) { push_vsrc_tier(4u, a); }

/* Visibility-probe variant of set_current: tags the probe RTPS with
 * a tag DISJOINT from the same piece's render batches (bit29 — free
 * in KSEG0 vaddrs) so the 1-vert probe batch never shares a rank
 * group with the real geometry. Without this, a probe entry evicted
 * from the sxy-keyed cache shifted the render batch's rank 0↔1
 * frame to frame: the probe cross-paired into the render batch
 * (sane-gate reject) and the render batch went rank-absent — a
 * 1-frame whole-piece freeze per eviction (sky stutter). */
void openpete_world_geo_set_current_probe(uint32_t strip_addr) {
    openpete_world_geo_set_current(strip_addr
                                   ? (strip_addr | 0x20000000u) : 0u);
}

void openpete_world_geo_mark_refine(void) {
    s_refine = 1;
}

uint32_t openpete_world_geo_take_vkey(void) {
    if (s_refine) {              /* re-RTPS of the SAME vertex */
        /* Refined projections get their OWN key namespace (REFINE
         * bit). Sharing the original's key made the two projection
         * spaces alias in the per-vertex tomap: when the near-clip
         * refinement fired on only one of two consecutive frames,
         * the matcher paired original↔refined and lerped across the
         * projection difference (midpoint harness measured mean
         * 33 px at the refine RTPS — its worst systematic offender).
         * Segregated, refined verts pair refined↔refined when both
         * frames refine, and miss (→ rigid fill) when the status
         * flips. */
        s_refine = 0;
        return s_last_vkey | OPENPETE_WORLD_VKEY_REFINE_BIT;
    }
    if (s_vsrc_head != s_vsrc_tail) {
        s_last_vkey = s_vsrc[s_vsrc_tail & (VSRC_RING - 1u)];
        s_vsrc_tail++;
        return s_last_vkey;
    }
    /* No pushed source (subdivided/LOD variants) — PHASE-B counter.
     * Small int ⇒ bit31 clear ⇒ matcher keeps the count-eq gate. */
    s_last_vkey = s_vidx++;
    return s_last_vkey;
}
