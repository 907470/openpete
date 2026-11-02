// core/runtime/host_state_ops.h - the savestate request queue, the stress
// harnesses and the rewind driver the host loop (run.c) runs at final yields.
#pragma once

#include "openpete.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Attribute one deferred request to the emitted polling loop whose delivery the fiber is yielding under, if any. */
void count_loop_deferral(void);

/** @brief Run every queued savestate request at a final yield, or keep the queue and count the wait.
 *
 * Requests run in arrival order. A load whose state was saved at another
 * park site stops the drain there: it and every later request stay queued
 * for the next final yield (state_op_site_wait). A slot request toasts when
 * it finishes or is refused, never while it waits (state_op_toast).
 * @return 1 at a final yield (the queue ran and the periodic state work may run), else 0. */
int state_ops_drain(void);

/** @brief Queue the savestate requests raised this iteration, then apply the slot-cycle key and announce a slot change.
 *
 * Requests are the hotkeys, the dev overlay's Save and Load, and the
 * scheduled one-shots; the slot ones use the active slot as it stood before
 * this iteration's cycle. The scheduled save is queued before the scheduled
 * load, so a save and a load armed for the same tick capture and then
 * restore (a same-tick roundtrip).
 * @param opts Run options (the --state-*-on-frame one-shots). */
void state_ops_collect(const openpete_options_t* opts);

/** @brief Step the savestate stress harnesses (restore-at-yield checks) by one quiescent iteration.
 * @param opts Run options (--state-stress, --state-stress-rewind). */
void state_stress_step(const openpete_options_t* opts);

/** @brief Run one quiescent iteration of rewind: the keyframe cadence, then one tick back while the rewind key is held in live, unpaused gameplay.
 * @param opts Run options.
 * @param paused The host is paused.
 * @param bk2_active A movie is loaded for replay.
 * @return true when the timeline moved (the iteration skips its forward tick). */
bool rewind_step(const openpete_options_t* opts, bool paused, bool bk2_active);

/** @brief Log the savestate census: requests deferred to a final yield, runahead deferrals and adoptions, park-site deferrals, and the first emitted-loop-site deferral. */
void state_ops_report(void);

/* Park-site deferrals by kind: a load whose snapshot was saved at another
 * park site than the current final yield's. */
enum { SITE_DEF_LOAD, SITE_DEF_REWIND, SITE_DEF_STRESS, SITE_DEF_RUNAHEAD, SITE_DEF_N };
extern unsigned long long s_site_deferred[SITE_DEF_N];
extern unsigned long long s_site_refused;
extern unsigned long long s_site_wait_yields;
extern unsigned long long s_site_abandoned;
extern unsigned long long s_state_ops_deferred[2];
extern unsigned long long s_state_yields_skipped;
extern unsigned long long s_loop_deferred;
extern uint32_t s_loop_deferred_header;
extern unsigned s_loop_deferred_key;
