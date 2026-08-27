/* core/runtime/perf_stats.c — see openpete_perf_stats.h for what this is for. */

#include "openpete_perf_stats.h"

#include "openpete_native_render.h"
#include "openpete_pace_log.h"
#include "openpete_time.h"

#include <stdio.h>
#include <string.h>

/* The sampling window. Matched to the renderer's rolling FPS window so the
 * whole readout steps together: an FPS line updating twice a second beside
 * pacing lines updating at some other cadence reads as flicker. */
#define PERF_WINDOW_NS (OP_NS_PER_SEC / 2)

/* Late-tick threshold: the S1 goal the pacing work is held to. Also the
 * boundary between phase-histogram buckets 2 and 3, which is what makes the
 * rate computable from census deltas alone. */
#define PERF_LATE_BUCKET 3

static openpete_pace_census_t s_prev;
static op_time_t s_prev_ns;
static int       s_seeded;

static char s_lines[OPENPETE_PERF_MAX_LINES][OPENPETE_PERF_LINE_MAX];
static int  s_line_n;
static int  s_level_cached = -1;

/* Percent of `part` in `whole`, or a negative sentinel when the population is
 * empty. A share of a nothing is not zero — printing 0% for "no presents were
 * attempted this window" states a result where there is no measurement. */
static double share_pct(uint64_t part, uint64_t whole)
{
    return whole ? 100.0 * (double)part / (double)whole : -1.0;
}

static void put_pct(char *buf, size_t cap, const char *label, double pct)
{
    if (pct < 0.0) snprintf(buf, cap, "%s: N/A", label);
    else           snprintf(buf, cap, "%s: %.0f%%", label, pct);
}

static void build_lines(int level, const openpete_pace_census_t *c,
                        op_time_t elapsed_ns)
{
    s_line_n = 0;
    snprintf(s_lines[s_line_n++], OPENPETE_PERF_LINE_MAX,
             "FPS: %d", openpete_native_fps_value());
    if (level < OPENPETE_PERF_DETAIL) return;

    const uint64_t vbls    = c->vbls        - s_prev.vbls;
    const uint64_t clean   = c->vbls_clean  - s_prev.vbls_clean;
    const uint64_t canon   = c->presents[0] - s_prev.presents[0];
    const uint64_t inter   = c->presents[1] - s_prev.presents[1];
    const uint64_t sheds   = c->sheds       - s_prev.sheds;
    const uint64_t slips   = (c->slip_events[0] - s_prev.slip_events[0]) +
                             (c->slip_events[1] - s_prev.slip_events[1]);

    uint64_t late = 0;
    for (int i = PERF_LATE_BUCKET; i < OP_PACE_HIST_BUCKETS; i++)
        late += c->phase_hist[i] - s_prev.phase_hist[i];

    /* VBL rate over the window rather than the nominal 59.826: the point of
     * the line is to show when the loop is NOT keeping the grid. */
    double vbl_hz = elapsed_ns > 0
        ? (double)vbls * (double)OP_NS_PER_SEC / (double)elapsed_ns : 0.0;
    snprintf(s_lines[s_line_n++], OPENPETE_PERF_LINE_MAX,
             "VBL: %.1f  SLIP: %llu", vbl_hz, (unsigned long long)slips);

    /* `clean` is the paced, non-transient population — the only VBLs whose
     * phase error means anything. An unpaced stretch (a level load, Tab
     * unlock, headless fast-forward) empties it, and the line says so rather
     * than reporting a perfect 0 late out of nothing. */
    if (clean)
        snprintf(s_lines[s_line_n++], OPENPETE_PERF_LINE_MAX,
                 "LATE: %llu/1000",
                 (unsigned long long)((late * 1000u + clean / 2u) / clean));
    else
        snprintf(s_lines[s_line_n++], OPENPETE_PERF_LINE_MAX, "LATE: N/A");

    /* Shed share is over presents ATTEMPTED (made + shed) — the fraction of
     * the opportunities the predicate declined. Interpolation share is over
     * presents MADE, which is a different denominator and deliberately so. */
    /* Half-lines: "SHED: 100%" and "INTERP: 100%" are the longest either
     * can be, and two of these plus the separator must fit a whole line. */
    char a[16], b[16];
    put_pct(a, sizeof a, "SHED",  share_pct(sheds, sheds + canon + inter));
    put_pct(b, sizeof b, "INTERP", share_pct(inter, canon + inter));
    snprintf(s_lines[s_line_n++], OPENPETE_PERF_LINE_MAX, "%s  %s", a, b);
}

int openpete_perf_stats_lines(int level, char lines[][OPENPETE_PERF_LINE_MAX],
                              int max_lines)
{
    if (level <= OPENPETE_PERF_OFF || !lines || max_lines <= 0) {
        s_level_cached = -1;   /* next enable rebuilds rather than reusing */
        return 0;
    }

    const op_time_t now = op_now();
    if (!s_seeded) {
        openpete_pace_census_get(&s_prev);
        s_prev_ns = now;
        s_seeded  = 1;
    }

    if (level != s_level_cached || now - s_prev_ns >= PERF_WINDOW_NS) {
        openpete_pace_census_t c;
        openpete_pace_census_get(&c);
        build_lines(level, &c, now - s_prev_ns);

        /* Roll the window only when one closed. A level change rebuilds the
         * text mid-window and must not consume the window with it, or the
         * next figures would cover a stub of time. */
        if (now - s_prev_ns >= PERF_WINDOW_NS) {
            s_prev    = c;
            s_prev_ns = now;
        }
        s_level_cached = level;
    }

    int n = s_line_n < max_lines ? s_line_n : max_lines;
    for (int i = 0; i < n; i++)
        memcpy(lines[i], s_lines[i], OPENPETE_PERF_LINE_MAX);
    /* Clear the tail. The caller compares the whole array against what it
     * last drew to decide whether to rebuild geometry, and leaving unwritten
     * rows as whatever was on its stack makes that comparison random. */
    for (int i = n; i < max_lines; i++)
        memset(lines[i], 0, OPENPETE_PERF_LINE_MAX);
    return n;
}
