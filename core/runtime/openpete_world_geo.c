/* openpete_world_geo.c — see openpete_world_geo.h. */

#include "openpete_world_geo.h"

static uint32_t s_current = 0;
static uint32_t s_vidx    = 0;   /* per-strip emission ordinal (fallback) */

/* PHASE-D vertex-source FIFO. Depth stays at the pipeline's prefetch
 * overhang (<=3); 16 slots absorb any variant. Power-of-two ring. */
#define VSRC_RING 16u
static uint32_t s_vsrc[VSRC_RING];
static uint32_t s_vsrc_head = 0, s_vsrc_tail = 0;
static int      s_refine    = 0;
static uint32_t s_last_vkey = 0;

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
