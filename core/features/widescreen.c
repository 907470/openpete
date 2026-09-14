// core/features/widescreen.c — the canonical-SX q16 FIFO.
//
// Verdict machinery for the particle / sparkle render->gameplay channels:
// gte.cpp's RTPS stashes every projection here and
// games/spyro-1/features/render_decouple.c reads it. Widescreen and FOV
// are native-renderer display policies (core/runtime/native_render.c) and
// never touch the GTE.

#include <stdint.h>

/* Canonical SX values in q16, stashed by gte.cpp's RTPS after each
 * projection in a 3-deep FIFO matching push_sxy semantics. sx2_q16 is the
 * latest projection (= what mfc2 C2_SXY2 reads, but in full q16 precision
 * before int16 truncation). Used by the particle cull substitutions in
 * r_particles.s to compute exact canonical 4:3 verdicts without truncation
 * ambiguity at the [0,512) boundaries. */
int64_t openpete_canonical_sx0_q16 = 0;
int64_t openpete_canonical_sx1_q16 = 0;
int64_t openpete_canonical_sx2_q16 = 0;
