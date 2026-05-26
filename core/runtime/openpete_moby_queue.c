// core/runtime/openpete_moby_queue.c — see header.

#include "openpete_moby_queue.h"

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

void openpete_moby_queue_push(uint32_t moby_vaddr) {
    /* Drop on overflow rather than alias the ring — the worst case is
     * the renderer pops 0 for the dropped entry and gte falls back to
     * world-ordinal for that batch (no obj_tag mispair, just same
     * jitter the unhooked path had). */
    if (((s_tail - s_head) & MOBY_Q_MASK) == MOBY_Q_MASK) return;
    s_buf[s_tail & MOBY_Q_MASK] = moby_vaddr;
    s_tail++;
}

uint32_t openpete_moby_queue_pop(void) {
    if (s_head == s_tail) return 0u;
    uint32_t v = s_buf[s_head & MOBY_Q_MASK];
    s_head++;
    return v;
}

void openpete_moby_queue_clear(void) {
    s_head = s_tail = 0u;
}
