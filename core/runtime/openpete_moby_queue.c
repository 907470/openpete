// core/runtime/openpete_moby_queue.c — see header.

#include "openpete_moby_queue.h"

#include <stdio.h>
#include <stdlib.h>

/* Ring sized for the worst-case per-frame moby count. Spyro's moby pools
 * (g_LevelMobys + g_DynMobys) are well under 1024 entries combined; a
 * 1024-deep ring covers every frame even if both regular and specular
 * cycles overlap (they don't, but the headroom is free). Power of two
 * so the modulo collapses to a mask. */
#define MOBY_Q_SLOTS 1024u
#define MOBY_Q_MASK  (MOBY_Q_SLOTS - 1u)

static uint32_t s_buf[MOBY_Q_SLOTS];
static uint32_t s_head;  /* next pop index */
static uint32_t s_tail;  /* next push index */

/* §11 F0 diag counters (OPENPETE_MOBYQ_DIAG=1). Reset per sim tick by
 * diag_tick. late_nonempty is the smoking-gun counter: a non-empty pop
 * that happens AFTER at least one empty pop this tick means a later
 * render pass consumed entries that did not come from its own cull
 * pass (cross-cycle identity leak). */
static unsigned s_d_push, s_d_pop, s_d_pop_empty, s_d_late_nonempty;
static unsigned s_d_drop;            /* overflow drops (push side)   */
static int      s_d_saw_empty;       /* an empty pop happened this tick */

static int diag_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBYQ_DIAG");
                 v = (e && *e && atoi(e) != 0) ? 1 : 0; }
    return v;
}

void openpete_moby_queue_push(uint32_t moby_vaddr) {
    /* Drop on overflow rather than alias the ring — the worst case is
     * the renderer pops 0 for the dropped entry and gte falls back to
     * world-ordinal for that batch (no obj_tag mispair, just same
     * jitter the unhooked path had). */
    if (((s_tail - s_head) & MOBY_Q_MASK) == MOBY_Q_MASK) { s_d_drop++; return; }
    s_buf[s_tail & MOBY_Q_MASK] = moby_vaddr;
    s_tail++;
    s_d_push++;
}

uint32_t openpete_moby_queue_pop(void) {
    if (s_head == s_tail) { s_d_pop_empty++; s_d_saw_empty = 1; return 0u; }
    uint32_t v = s_buf[s_head & MOBY_Q_MASK];
    s_head++;
    s_d_pop++;
    if (s_d_saw_empty) s_d_late_nonempty++;
    return v;
}

void openpete_moby_queue_clear(void) {
    s_head = s_tail = 0u;
}

void openpete_moby_queue_diag_tick(unsigned frame) {
    if (!diag_on()) return;

    const unsigned depth = (s_tail - s_head) & MOBY_Q_MASK;

    /* Cumulative residue/anomaly tallies across the run (static — the
     * per-tick counters reset below). */
    static unsigned s_ticks, s_ticks_residue, s_ticks_leak,
                    s_max_depth, s_total_push, s_total_pop,
                    s_total_empty, s_total_late, s_total_drop;
    s_ticks++;
    s_total_push  += s_d_push;
    s_total_pop   += s_d_pop;
    s_total_empty += s_d_pop_empty;
    s_total_late  += s_d_late_nonempty;
    s_total_drop  += s_d_drop;
    if (depth) s_ticks_residue++;
    if (s_d_late_nonempty) s_ticks_leak++;
    if (depth > s_max_depth) s_max_depth = depth;

    /* Per-tick line only on anomaly (residue or cross-cycle leak) —
     * the balanced case is summarised every 1000 ticks. */
    if (depth || s_d_late_nonempty || s_d_drop) {
        fprintf(stderr,
                "[mobyq] f=%u push=%u pop=%u empty=%u late_nonempty=%u "
                "drop=%u RESIDUE=%u\n",
                frame, s_d_push, s_d_pop, s_d_pop_empty,
                s_d_late_nonempty, s_d_drop, depth);
    }
    if ((s_ticks % 1000u) == 0u) {
        fprintf(stderr,
                "[mobyq] summary t=%u push=%u pop=%u empty=%u "
                "late_nonempty=%u drop=%u residue_ticks=%u leak_ticks=%u "
                "max_depth=%u\n",
                s_ticks, s_total_push, s_total_pop, s_total_empty,
                s_total_late, s_total_drop, s_ticks_residue, s_ticks_leak,
                s_max_depth);
    }

    s_d_push = s_d_pop = s_d_pop_empty = s_d_late_nonempty = s_d_drop = 0;
    s_d_saw_empty = 0;
}
