/* openpete_perf_stats.h — the on-screen performance-stats overlay's numbers.
 *
 * Three levels, because the two audiences want different things: level 1 is
 * the FPS counter a player glances at, level 2 is the pacing readout used
 * while working on the sim-tick/present seam (scripts/pacing/ answers the
 * same questions offline, in far more detail — this is the live view).
 *
 * Everything here is derived from the pacing census, which is compiled into
 * every build and counts unconditionally, so the overlay costs a subtraction
 * rather than an instrument of its own.
 *
 * WINDOWED, not cumulative. The census carries whole-run totals and maxima,
 * and a whole-run maximum stops moving after the first level load: it would
 * show the loading hitch for the rest of the session and nothing about what
 * the frame in front of you is doing. Every figure below is a delta over the
 * last sampling window.
 *
 * Rates, not near-threshold quantiles. `late_per_1000` counts VBLs that
 * started more than 250 µs after their deadline. The obvious alternative — a
 * windowed p99 of the phase error — is unstable exactly where it matters:
 * when few VBLs sit above the threshold, the quantile is decided by a handful
 * of events and swings several-fold between identical runs. A rate over the
 * same population does not.
 */

#ifndef OPENPETE_PERF_STATS_H
#define OPENPETE_PERF_STATS_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    OPENPETE_PERF_OFF    = 0,  /* nothing drawn                             */
    OPENPETE_PERF_FPS    = 1,  /* the FPS counter alone                     */
    OPENPETE_PERF_DETAIL = 2,  /* + pacing: VBL rate, slips, late ticks,
                                *   shed and interpolation share            */
    OPENPETE_PERF_LEVELS = 3
};

/* Longest line the overlay emits, and the most lines any level produces.
 * The glyph font has digits, letters, '%', '/', '.', '+', '^', '\'' and a
 * faked ':' — no minus sign, so no figure below is ever signed. */
#define OPENPETE_PERF_LINE_MAX  40
#define OPENPETE_PERF_MAX_LINES 4

/* Fill `lines` with the text for `level` and return how many were written.
 * Rows past the returned count are cleared, so the whole array is defined and
 * a caller can memcmp it against what it last drew.
 *
 * Recomputes at most once per sampling window (the same 0.5 s window the FPS
 * value already uses) and returns the same text in between — which is what
 * lets the caller skip rebuilding geometry that has not changed. */
int openpete_perf_stats_lines(int level,
                              char lines[][OPENPETE_PERF_LINE_MAX],
                              int max_lines);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_PERF_STATS_H */
