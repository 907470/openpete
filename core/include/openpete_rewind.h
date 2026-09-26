/* core/include/openpete_rewind.h — rewind keyframe ring.
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
 * Memory: ~3.1 MiB per keyframe plus the mod arena's watermark, one
 * keyframe per second of window plus one. */

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

/** @brief Size the keyframe ring for a rewind window, freeing the previous ring and its snapshots.
 *
 * The ring holds window_seconds + 1 keyframes, each snapshot allocated on
 * first capture. The first enabling call registers a guest-discontinuity
 * handler that clears the ring on a savestate load. Called at startup
 * from --rewind and by the overlay's rewind knob. An allocation failure
 * leaves rewind disabled.
 * @param window_seconds Window length; 0 or less disables rewind. */
void openpete_rewind_configure(int window_seconds);
int  openpete_rewind_enabled(void);

/** @brief Capture a keyframe when one is due, or clear the ring when gameplay is not live.
 *
 * A keyframe is due on the first call after a clear and then every
 * OPENPETE_REWIND_KF_VBLS VBLs; it overwrites the oldest slot and records
 * input_log_count() beside the snapshot. A failed snapshot allocation or
 * save skips the capture and retries on the next call. Called once per
 * host iteration at the savestate quiescent point, with the game fiber
 * yielded. No effect when rewind is disabled.
 * @param cpu Guest CPU state to snapshot.
 * @param frame Current openpete_frame (VBL count).
 * @param live Non-zero while gameplay is live; 0 during a level
 *        transition or load, whose mutated sections the snapshot
 *        excludes. */
void openpete_rewind_on_iter(const CPUState* cpu, unsigned frame, int live);

/** @brief Find the newest valid keyframe at or before a VBL.
 *
 * @param target openpete_frame value to rewind to.
 * @return The keyframe with the largest frame not above @p target, or
 *         NULL when none is valid there. */
const openpete_rewind_kf_t* openpete_rewind_pick(unsigned target);

/** @brief Invalidate every keyframe captured after a VBL.
 *
 * Called after a rewind lands, since those keyframes describe the
 * discarded future. Their snapshots stay allocated for reuse.
 * @param frame openpete_frame the rewind landed on. */
void openpete_rewind_drop_after(unsigned frame);

/** @brief Invalidate every keyframe and make the next call to openpete_rewind_on_iter capture one.
 *
 * Snapshots stay allocated. Called on liveness loss, on a savestate load
 * through the discontinuity handler, on a mod-set change, and by
 * openpete_rewind_configure. */
void openpete_rewind_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_REWIND_H */
