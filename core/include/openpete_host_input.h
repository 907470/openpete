#ifndef OPENPETE_HOST_INPUT_H
#define OPENPETE_HOST_INPUT_H

/* Host-owned input decode.
 *
 * openpete owns *decode* (SDL -> a pad snapshot); PsyCross's override channel
 * still owns *injection* (the byte-exact write into the PSX pad buffer). The
 * two halves are deliberately split into a producer and a consumer with a
 * snapshot between them:
 *
 *   producer   openpete_host_input_sample()   pump-side: reads SDL, publishes
 *   consumer   openpete_host_input_latch()    sim-side: arbitrates, stamps
 *
 * Today both run on the host loop, back to back. The split exists because the
 * pump and the sim are destined for different threads, and a snapshot is the
 * only handoff that survives that move unchanged: the consumer never reads SDL.
 *
 * Decode itself is PsyX_Pad_DecodeP1 — the same merge the non-override path
 * uses — so host-decoded bits are identical to PsyCross-decoded bits by
 * construction rather than by comparison.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct openpete_pad_snapshot {
    unsigned short bits;      /* active-low, released = 1 */
    unsigned char  axes[4];   /* PSX pad->analog order: RX, RY, LX, LY */
    bool           valid;
} openpete_pad_snapshot_t;

/* 1 when OPENPETE_INPUT_LEGACY selects the pre-ownership path (PsyCross pumps,
 * decodes and injects). Diagnostic A/B only. */
int openpete_host_input_legacy(void);

/* Producer. Decodes live SDL input (as of the last event pump) and publishes
 * it. `alt_held` suppresses the sample the way PsyX_UpdateInput's own Alt gate
 * does, so Alt+Enter cannot register as Start; a suppressed sample republishes
 * the neutral pad rather than going stale. */
void openpete_host_input_sample(bool alt_held);

/* Publish a neutral snapshot so the consumer has something valid to stamp
 * before the producer's first real sample. Required before starting a
 * consumer on another thread: without it the first latch leaves the override
 * inactive and the injection path falls back to reading SDL directly, on the
 * wrong thread. */
void openpete_host_input_seed_neutral(void);

/* Consumer. Stamps the published snapshot into the PsyCross override channel.
 * A no-op when `claimed` is set — that means a higher-priority override writer
 * (bk2 replay, the settings menu, the dev overlay, rewind) already owns the
 * channel this iteration. Live decode is the lowest-priority writer: it fills
 * the channel whenever nothing else claims it, which is what turns the
 * former "override inactive -> PsyCross polls SDL itself" fallback into
 * "override always active, host always the source". */
void openpete_host_input_latch(bool claimed);

/* ---- host SDL input-state snapshot -------------------------------------
 *
 * Everything that reads live SDL keyboard/gamepad state does so through this
 * snapshot instead of calling SDL directly. Same producer/consumer split as
 * the pad above and for the same reason: those SDL arrays are refreshed by
 * the event pump, and the pump is moving to a different thread from the host
 * loop that consumes them. A snapshot is the handoff that survives the move.
 *
 * Double-buffered with an atomic index, so a consumer always reads one
 * internally consistent sample rather than a half-updated one.
 *
 * Publishing is a producer-side (pump) act; every accessor below is
 * consumer-side and never touches SDL. */
void openpete_host_input_publish_sdl(void);

int      openpete_host_input_key_down(int scancode);
int      openpete_host_input_pad_button(int button);
int      openpete_host_input_pad_axis(int axis);
unsigned openpete_host_input_modstate(void);

/* Last published snapshot (diagnostics / the parity instrument). */
void openpete_host_input_last(openpete_pad_snapshot_t* out);

/* op_now() at the moment the last snapshot was published, or 0 before the
 * first one. The consumer runs on a different thread from the producer, so
 * "how stale was the input this tick consumed" is a real quantity now rather
 * than zero by construction; this is what makes it measurable. */
int64_t openpete_host_input_last_sample_ns(void);

/* Publish instant of the snapshot the last latch actually CONSUMED, captured
 * at the latch. This, not the one above, is what a tick's input staleness is
 * measured against: reading the live value after the tick has run names a
 * sample the tick never saw. Zero when nothing has latched (the legacy input
 * path never latches, and its staleness is therefore unmeasured, not zero). */
int64_t openpete_host_input_latched_sample_ns(void);

/* Parity instrument (--input-parity). Compares what the host actually drove
 * into the pad against a fresh live decode taken in the same iteration, on the
 * frames where live decode is supposed to be the source. Counts engagement
 * (frames carrying real input) and refuses to report a pass at zero, because a
 * run that never pressed anything proves nothing. */
void openpete_host_input_parity_enable(void);
void openpete_host_input_parity_check(bool claimed);
/* Returns 1 = pass, 0 = mismatches seen, -1 = vacuous (nothing engaged). */
int  openpete_host_input_parity_report(void);

#endif /* OPENPETE_HOST_INPUT_H */
