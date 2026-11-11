/* openpete_eager_ladder.h - the eager producer's capacity estimate and
 * decimation ladder, as pure functions of the quantities the producer
 * measures.
 *
 * A span of ns display slots at decimation level L plans k(L) = ceil(ns / L)
 * scenes (every L-th slot). afford is the number of scenes the span's
 * production budget prices in. need = ceil(ns / afford) is the lowest level
 * whose plan fits that estimate.
 *
 * Leaf header: no openpete dependencies, so the unit test drives it. */

#ifndef OPENPETE_EAGER_LADDER_H
#define OPENPETE_EAGER_LADDER_H

#include <math.h>
#include <stdint.h>

/* Bound on the scenes one span can afford: the producer's target capacity. */
#define OP_EAGER_LADDER_MAX_AFFORD 128

/* The ladder's standing state. Initialise with level 1 and since_short at
 * the window, so the first shortfall is not a recurrence. */
typedef struct {
    int level;          /* standing divisor: later spans plan every level-th slot */
    int headroom_runs;  /* consecutive clean spans whose estimate covers a lower level */
    int hold_level;     /* the highest level any span of the current run needed */
    int since_short;    /* clean spans since the last shortfall, saturated at the window */
} op_eager_ladder_t;

/** @brief Return how many scenes of mean cost ema and deviation dev fit a budget, priced on their sum.
 *
 * The largest N with N * ema + 2 * sqrt(N) * dev <= budget: the sum of N
 * independent scene costs has mean N * ema and deviation sqrt(N) * dev, so
 * the allowance is two deviations of the sum at the N being priced. At least
 * 1, at most OP_EAGER_LADDER_MAX_AFFORD; with ema 0, the maximum.
 * @param budget The production budget in nanoseconds.
 * @param ema The per-scene production cost mean in nanoseconds.
 * @param dev The per-scene production cost mean absolute deviation in nanoseconds.
 * @return The number of scenes afforded. */
static inline int op_eager_ladder_afford(int64_t budget, int64_t ema, int64_t dev)
{
    if (ema <= 0) return OP_EAGER_LADDER_MAX_AFFORD;
    int n = 0;
    while (n < OP_EAGER_LADDER_MAX_AFFORD) {
        const double m = (double)(n + 1);
        if (m * (double)ema + 2.0 * sqrt(m) * (double)dev > (double)budget) break;
        n++;
    }
    return n < 1 ? 1 : n;
}

/** @brief Return how many scenes the produce guard admits in one production window.
 *
 * The guard starts a scene only while its start plus the per-scene tail
 * bound lies at or before the window's end, and checks between scenes, so
 * the window admits 1 + m scenes, m the largest count whose summed cost
 * (priced as in op_eager_ladder_afford, without its floor of 1) fits the
 * window less one tail; none when the window is shorter than the tail.
 * @param window The window's length in nanoseconds.
 * @param ema The per-scene production cost mean in nanoseconds.
 * @param dev The per-scene production cost mean absolute deviation in nanoseconds.
 * @param tail The guard's per-scene tail bound in nanoseconds.
 * @return The number of scenes admitted, 0 to OP_EAGER_LADDER_MAX_AFFORD. */
static inline int op_eager_ladder_window(int64_t window, int64_t ema, int64_t dev, int64_t tail)
{
    if (window < tail || window <= 0) return 0;
    if (ema <= 0) return OP_EAGER_LADDER_MAX_AFFORD;
    const int64_t room = window - tail;
    int m = 0;
    while (m < OP_EAGER_LADDER_MAX_AFFORD - 1) {
        const double k = (double)(m + 1);
        if (k * (double)ema + 2.0 * sqrt(k) * (double)dev > (double)room) break;
        m++;
    }
    return m + 1;
}

/** @brief Return how many scenes a span's two production windows admit together, at least 1.
 *
 * A span is produced in two windows split by the update VBL: from the plan
 * to the guard's stop before that VBL, and from the instant production
 * resumes after it to the guard's stop before the next tick. Each window
 * loses the part of a scene its guard refuses at the end, so the count is
 * the sum of op_eager_ladder_window over the two, not the fit of their sum.
 * The canonical scene is produced whatever the guard says, hence the floor.
 * @param w1 The first window in nanoseconds.
 * @param w2 The second window in nanoseconds; 0 or less when there is none.
 * @param ema The per-scene production cost mean in nanoseconds.
 * @param dev The per-scene production cost mean absolute deviation in nanoseconds.
 * @param tail The guard's per-scene tail bound in nanoseconds.
 * @return The number of scenes afforded, 1 to OP_EAGER_LADDER_MAX_AFFORD. */
static inline int op_eager_ladder_afford_windows(int64_t w1, int64_t w2, int64_t ema, int64_t dev, int64_t tail)
{
    int n = op_eager_ladder_window(w1, ema, dev, tail) + op_eager_ladder_window(w2, ema, dev, tail);
    if (n > OP_EAGER_LADDER_MAX_AFFORD) n = OP_EAGER_LADDER_MAX_AFFORD;
    return n < 1 ? 1 : n;
}

/** @brief Move the ladder for a span of ns slots of which afford scenes fit, and return the level that span plans at.
 *
 * A shortfall is a span whose estimate needs a higher level than the
 * standing one (need > level) or one that follows a cut span (cut). A
 * shortfall plans its own span at max(level, need); it raises the standing
 * level only when another shortfall came within the last window spans, to
 * need, and by at least one when the span follows a cut. An isolated
 * shortfall therefore costs its own span, not the window of spans a standing
 * raise holds. A clean span whose need is below the standing level extends a
 * headroom run; after window consecutive such spans the level falls in one
 * move to the highest need of the run, since the run measured that level.
 * The estimate already carries its own allowance (two deviations of the sum,
 * see op_eager_ladder_afford), so the fall applies no second margin.
 * @param s The ladder state.
 * @param ns The span's slot count, at least 1.
 * @param afford The scenes the budget affords, at least 1.
 * @param cut Nonzero when the previous span was cut short by its own cost (not a stall).
 * @param window The run length for both a recurrence and a fall, in spans.
 * @return The level this span plans at. */
static inline int op_eager_ladder_step(op_eager_ladder_t *s, int ns, int afford, int cut, int window)
{
    if (afford < 1) afford = 1;
    int need = (ns + afford - 1) / afford;
    if (need < 1) need = 1;
    if (need > s->level || cut) {
        const int recur = s->since_short < window;
        s->since_short = 0;
        s->headroom_runs = 0; s->hold_level = 0;
        if (!recur) return need > s->level ? need : s->level;
        if (cut && need <= s->level) need = s->level + 1;
        if (need > s->level) s->level = need;
        return s->level;
    }
    if (s->since_short < window) s->since_short++;
    if (need >= s->level) { s->headroom_runs = 0; s->hold_level = 0; return s->level; }
    if (need > s->hold_level) s->hold_level = need;
    if (++s->headroom_runs < window) return s->level;
    s->level = s->hold_level;
    s->headroom_runs = 0; s->hold_level = 0;
    return s->level;
}

/** @brief Return the period that spaces a free span's affordable scenes evenly over its length, and their count.
 *
 * A free span is one with no display grid to stay on (no vsync): its
 * instants are placed by the budget alone. With after nonzero the span
 * continues the previous one and its instants are start + k * period for k
 * = 1 .. count, the last at or before start + len; otherwise the first is at
 * start and the instants are start + k * period for k = 0 .. count - 1. The
 * count is afford, lowered until the period is at least floor_ns (the
 * render rate cap or the display side's per-scene cost), and at least 1. The period is
 * the largest whole nanosecond that fits count instants in len, so a span
 * of count instants never asks the ladder for a level above 1.
 * @param len The span's length in nanoseconds, from its anchor to its last admissible instant.
 * @param afford The scenes the budget affords, at least 1.
 * @param floor_ns The least period in nanoseconds, or 0 for none.
 * @param after Nonzero when the anchor is the previous span's last instant, which this span does not repeat.
 * @param count Receives the number of instants, at least 1.
 * @return The period in nanoseconds, at least 1. */
static inline int64_t op_eager_ladder_fill(int64_t len, int afford, int64_t floor_ns, int after, int *count)
{
    if (len < 0) len = 0;
    int n = afford < 1 ? 1 : afford;
    if (floor_ns > 0) {
        const int64_t fit = len / floor_ns + (after ? 0 : 1);   /* instants at floor_ns spacing */
        if (fit < n) n = fit < 1 ? 1 : (int)fit;
    }
    const int gaps = after ? n : n - 1;
    int64_t period = gaps > 0 ? len / gaps : len;
    if (period < floor_ns) period = floor_ns;
    if (period < 1) period = 1;
    *count = n;
    return period;
}

#endif /* OPENPETE_EAGER_LADDER_H */
