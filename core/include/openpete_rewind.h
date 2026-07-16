/* core/include/openpete_rewind.h — rewind keyframe ring (savestate P4).
 *
 * A ring of in-memory snapshots (openpete_state_mem_t) captured every
 * OPENPETE_REWIND_KF_VBLS while gameplay is live, paired with the
 * input_log index at capture so the driver can replay forward from a
 * keyframe with the exact recorded inputs:
 *
 *   invariant: at capture, input_log entry (log_idx - 1) is the input
 *   for VBL `frame`; entry (log_idx + k) is the input for frame+1+k.
 *
 * The DRIVER lives in run.c (it needs the game fiber + pad overrides):
 * hold Backspace -> pick(target) -> quiet mem-load -> stamp inputs from
 * the log per VBL -> co_switch to target -> truncate log + drop_after.
 *
 * Scope: same level only. Keyframes are cleared whenever gameplay
 * liveness drops (level transition/loading — the mem snapshot excludes
 * VRAM/SPU-RAM by contract) and on any real savestate load (via the
 * discontinuity broadcast — a loaded state is a different timeline).
 * Memory: ~2.1 MiB per keyframe, 1 keyframe/second of window. */

#ifndef OPENPETE_REWIND_H
#define OPENPETE_REWIND_H

#include "openpete_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_REWIND_KF_VBLS 60u   /* one keyframe per second */

typedef struct {
    openpete_state_mem_t* mem;
    unsigned              frame;    /* openpete_frame at capture */
    int                   log_idx;  /* input_log_count() at capture */
    int                   valid;
} openpete_rewind_kf_t;

/* Set the window in seconds (0 = disabled, frees the ring). Call once at
 * startup from the --rewind flag; safe to call again (reconfigures). */
void openpete_rewind_configure(int window_seconds);
int  openpete_rewind_enabled(void);

/* Per-host-iteration bookkeeping: captures a keyframe on cadence while
 * `live`, clears the ring when liveness drops. Call at the savestate
 * quiescent point (game fiber yielded). */
void openpete_rewind_on_iter(const CPUState* cpu, unsigned frame, int live);

/* Newest keyframe with kf->frame <= target, or NULL (window exhausted /
 * empty). */
const openpete_rewind_kf_t* openpete_rewind_pick(unsigned target);

/* Invalidate keyframes newer than `frame` (the future was rewound away). */
void openpete_rewind_drop_after(unsigned frame);

/* Drop everything (liveness loss, savestate load, shutdown). */
void openpete_rewind_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_REWIND_H */
