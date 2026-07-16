/* core/runtime/rewind.c — see openpete_rewind.h. */

#include "openpete_rewind.h"
#include "openpete_discontinuity.h"
#include "openpete_input_log.h"
#include "openpete_log.h"

#include <stdlib.h>
#include <string.h>

static openpete_rewind_kf_t* s_ring = NULL;
static int s_ring_n   = 0;   /* capacity (window seconds + 1) */
static int s_head     = 0;   /* next slot to write */
static unsigned s_next_due = 0;  /* frame at/after which the next kf fires */
static int s_registered = 0;

static void rewind_on_guest_discontinuity(void) {
    /* A real savestate load replaced the timeline; every keyframe (and
     * the input-log suffix they anchor) describes the OLD one. */
    openpete_rewind_clear();
}

void openpete_rewind_configure(int window_seconds) {
    openpete_rewind_clear();
    free(s_ring);
    s_ring = NULL;
    s_ring_n = 0;
    if (window_seconds <= 0) return;
    s_ring_n = window_seconds + 1;
    s_ring = (openpete_rewind_kf_t*)calloc((size_t)s_ring_n, sizeof(*s_ring));
    if (!s_ring) { s_ring_n = 0; return; }
    if (!s_registered) {
        s_registered = 1;
        (void)openpete_on_guest_discontinuity(rewind_on_guest_discontinuity);
    }
    OPL_INFO_F("rewind", "window %d s (%d keyframes x ~2.1 MiB)",
               window_seconds, s_ring_n);
}

int openpete_rewind_enabled(void) { return s_ring_n > 0; }

void openpete_rewind_clear(void) {
    for (int i = 0; i < s_ring_n; i++) s_ring[i].valid = 0;
    s_head = 0;
    s_next_due = 0;
}

void openpete_rewind_on_iter(const CPUState* cpu, unsigned frame, int live) {
    if (!s_ring_n) return;
    if (!live) {
        /* Level transition / loading: the mem snapshot excludes the
         * sections that mutate here — rewinding across is undefined. */
        openpete_rewind_clear();
        return;
    }
    if (s_next_due && frame < s_next_due) return;
    openpete_rewind_kf_t* kf = &s_ring[s_head];
    if (!kf->mem) {
        kf->mem = openpete_state_mem_new();
        if (!kf->mem) return;   /* OOM: ring shrinks by one, logged inside */
    }
    if (openpete_state_mem_save(kf->mem, cpu, frame) != 0) return;
    kf->frame   = frame;
    kf->log_idx = input_log_count();
    kf->valid   = 1;
    s_head = (s_head + 1) % s_ring_n;
    s_next_due = frame + OPENPETE_REWIND_KF_VBLS;
}

const openpete_rewind_kf_t* openpete_rewind_pick(unsigned target) {
    const openpete_rewind_kf_t* best = NULL;
    for (int i = 0; i < s_ring_n; i++) {
        const openpete_rewind_kf_t* kf = &s_ring[i];
        if (!kf->valid || kf->frame > target) continue;
        if (!best || kf->frame > best->frame) best = kf;
    }
    return best;
}

void openpete_rewind_drop_after(unsigned frame) {
    for (int i = 0; i < s_ring_n; i++)
        if (s_ring[i].valid && s_ring[i].frame > frame) s_ring[i].valid = 0;
}
