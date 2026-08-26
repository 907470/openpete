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

/* Consumer. Stamps the published snapshot into the PsyCross override channel.
 * A no-op when `claimed` is set — that means a higher-priority override writer
 * (bk2 replay, the settings menu, the dev overlay, rewind) already owns the
 * channel this iteration. Live decode is the lowest-priority writer: it fills
 * the channel whenever nothing else claims it, which is what turns the
 * former "override inactive -> PsyCross polls SDL itself" fallback into
 * "override always active, host always the source". */
void openpete_host_input_latch(bool claimed);

/* Last published snapshot (diagnostics / the parity instrument). */
void openpete_host_input_last(openpete_pad_snapshot_t* out);

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
