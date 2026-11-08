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

#endif /* OPENPETE_EAGER_LADDER_H */
