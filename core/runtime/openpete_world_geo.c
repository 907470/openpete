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
    s_vsrc[s_vsrc_head & (VSRC_RING - 1u)] = src_addr;
    s_vsrc_head++;
}

void openpete_world_geo_mark_refine(void) {
    s_refine = 1;
}

uint32_t openpete_world_geo_take_vkey(void) {
    if (s_refine) {              /* re-RTPS of the SAME vertex */
        s_refine = 0;
        return s_last_vkey;
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
