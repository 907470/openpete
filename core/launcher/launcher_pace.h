/* launcher_pace.h - the launcher's present rate. While the window can show,
 * the launcher presents once per display refresh: the pacer waits out
 * whatever is left of the refresh period when a present comes early (FIFO's
 * own blocking spends it on a display that blocks; a software surface or a
 * non-vsync mode does not), so the rate is the refresh on every present mode.
 * While the window is minimised, hidden or occluded no present runs and the
 * loops wait on events instead. During a build stage (the progress view of
 * a task or job: copy, read, translate, build) on a software rasterizer,
 * whose draw runs on the CPU cores the compile uses, presents run every nth
 * refresh, n the smallest divisor that puts the rate at or below 10 Hz.
 * Every call is on the launcher's thread.
 *
 * Harness variable: OPENPETE_LAUNCHER_PRESENT_LOG=1 logs, each second of
 * presents, the count, the measured rate, the refresh, the divisor, the mean time from
 * the pacer's release to the present's end, and the presents that began more
 * than half a period late; =2 also logs the interval before every present. */
#ifndef OPENPETE_LAUNCHER_PACE_H
#define OPENPETE_LAUNCHER_PACE_H

#include <stdint.h>

/** @brief Report whether the launcher's window exists and can show: not minimised, hidden or occluded.
 *
 * @return true when a present would reach the screen. */
bool launcher_pace_visible(void);

/** @brief Decide whether the next launcher present runs, waiting out the rest of its refresh period when it may.
 *
 * A window that cannot show skips the present, except before the first one
 * (a window system may map the window only at its first image). Logs each
 * change between showing and not showing.
 * @param presented_before true once the launcher has presented.
 * @param may_wait true to sleep to the period's end; false skips a present
 *        that comes before it instead (a caller whose own work the wait
 *        would stall).
 * @return true when the caller presents now. */
bool launcher_pace_begin(bool presented_before, bool may_wait);

/** @brief Count one present that ran, and log the per-second rollup when OPENPETE_LAUNCHER_PRESENT_LOG is set. */
void launcher_pace_done(void);

/** @brief Return the number of presents counted since the process started.
 *
 * A loop compares it before and after an iteration to learn whether that
 * iteration presented (and so was paced) or must wait itself.
 * @return The count. */
uint64_t launcher_pace_count(void);

/** @brief Enter or leave a build stage, which on a software rasterizer presents at the first refresh divisor at or below 10 Hz.
 *
 * @param on true while a task's or job's progress view is shown. */
void launcher_pace_build_stage(bool on);

#endif
