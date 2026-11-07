/* core/include/openpete_audio_consumer.h - the audio device feed's consumer
 * thread (core/host/audio_consumer.c). */
#ifndef OPENPETE_AUDIO_CONSUMER_H
#define OPENPETE_AUDIO_CONSUMER_H

#include "openpete_thread_roles.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Start the consumer thread that drains the device feed into the audio device; a second call returns at once.
 *
 * On the feed's synthetic device under the virtual clock the thread runs
 * the device inside every virtual-clock step (op_virtual_clock_set_step_hook);
 * otherwise it consumes every wake period of the wall clock. Called after
 * the pacer's init has requested the launch tick scale, which the thread
 * applies at its first wake. */
void openpete_audio_consumer_start(void) OP_REQUIRES(sim);

/** @brief Stop and join the consumer thread and remove its clock-step hook; returns at once when it is not running. */
void openpete_audio_consumer_stop(void) OP_REQUIRES(sim);

/** @brief Advance the virtual clock by the synthetic producer model's delay before this VBL's push (PsyX_SPUFeed_SimProducerDelay).
 *
 * A no-op unless the consumer runs on the virtual clock. Called once per
 * VBL before the game fiber runs it, so the VBL's push lands at the
 * delayed instant and the pacer's next sleep returns to its grid. */
void openpete_audio_consumer_producer_step(void) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_AUDIO_CONSUMER_H */
