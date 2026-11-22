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

/** @brief Return a span's first instant when it continues the previous span's grid, and the instant that grid placed before it.
 *
 * The continuation is the previous span's last instant plus one period.
 * When that lies more than a period past the span's own first slot, the old
 * grid ran ahead of the plan instant (a period floored at a present-thread
 * cost above the tick advances the chain faster than time), and only its
 * phase is kept: the instant lands in [first, first + period), so the lead
 * of a span's first instant over its own first slot stays under one period
 * whatever the previous lead was. The caller checks last + period > first.
 * @param last The previous span's last instant.
 * @param first The span's first render-grid slot.
 * @param period The planned grid period, positive.
 * @param prev Receives the old grid's instant one period before the result:
 *        last itself, or the instant the phase-only case continues after.
 * @return The span's first instant. */
static inline int64_t op_eager_span_continue(int64_t last, int64_t first, int64_t period, int64_t *prev)
{
    int64_t f = last + period;
    *prev = last;
    if (f - first > period) {
        f = first + (f - first) % period;
        *prev = f - period;
    }
    return f;
}

/* Why a span ended with planned scenes unproduced. Only a deadline cut says
 * the plan held more scenes than their cost lets the budget produce. */
typedef enum {
    OP_EAGER_CUT_NONE = 0,   /* the span produced its plan */
    OP_EAGER_CUT_DEADLINE,   /* the produce guard stopped before a scene whose cost would pass the deadline */
    OP_EAGER_CUT_RING,       /* the scene ring stayed full: the present thread drained slower than the plan's spacing */
    OP_EAGER_CUT_WITHHELD,   /* production was withheld: the world was not live, or the span was never resumed */
    OP_EAGER_CUT_LATE        /* the span was planned after a stalled tick start, which the guard absorbs */
} op_eager_cut_t;

/* The count controller's unit: a deficit of OP_EAGER_COUNT_ONE plans none of
 * afford. The deficit never passes half of it. */
#define OP_EAGER_COUNT_ONE 65536
#define OP_EAGER_COUNT_MAX_DEFICIT (OP_EAGER_COUNT_ONE / 2)

/* A free span's count controller: with no display grid the span plans the
 * fraction 1 - deficit of afford, and its instants are spaced no closer than
 * the present thread's measured drain while a ring cut is recent.
 * Initialise zeroed. */
typedef struct {
    int     deficit;     /* fraction of afford left unplanned, in 1/OP_EAGER_COUNT_ONE */
    int     clean;       /* consecutive produced spans that finished with room to spare */
    int64_t drain_ns;    /* the last ring-cut span's length over the scenes it produced */
    int     drain_left;  /* spans drain_ns still floors the spacing for */
} op_eager_count_t;

/** @brief Return the deficit step worth one scene at afford: OP_EAGER_COUNT_ONE / afford, rounded up.
 * @param afford The scenes the budget affords; below 1 counts as 1.
 * @return The step, in 1/OP_EAGER_COUNT_ONE. */
static inline int op_eager_count_unit(int afford)
{
    if (afford < 1) afford = 1;
    return (OP_EAGER_COUNT_ONE + afford - 1) / afford;
}

/** @brief Return the scenes a free span plans: afford scaled by 1 - deficit, rounded to nearest, at least 1.
 *
 * The rounding makes k steps of op_eager_count_unit(afford) plan exactly
 * afford - k at that afford (the unit's excess over ONE / afford, summed over
 * at most afford / 2 steps, stays under half a scene for afford up to
 * OP_EAGER_LADDER_MAX_AFFORD), and a change of afford rescales the plan with
 * it, so no correction outlives the budget it was measured against.
 * @param c The controller.
 * @param afford The scenes the budget affords.
 * @return The scene count. */
static inline int op_eager_count_plan(const op_eager_count_t *c, int afford)
{
    const int64_t n = ((int64_t)afford * (OP_EAGER_COUNT_ONE - c->deficit) + OP_EAGER_COUNT_ONE / 2) /
                      OP_EAGER_COUNT_ONE;
    return n < 1 ? 1 : (int)n;
}

/** @brief Return the least spacing in nanoseconds a free span's instants keep for the present thread's drain, or 0 for none.
 * @param c The controller.
 * @return drain_ns while a ring cut is recent, else 0. */
static inline int64_t op_eager_count_floor(const op_eager_count_t *c)
{
    return c->drain_left > 0 ? c->drain_ns : 0;
}

/** @brief Move the free-span count controller for the span just closed, by the cause of its cut.
 *
 * A deadline cut raises the deficit by one scene at the span's afford, up to
 * OP_EAGER_COUNT_MAX_DEFICIT, and ends the clean run. A ring cut is the
 * present thread's limit, not the budget's: it sets the drain floor to the
 * span's length over the scenes it produced, for drain_runs spans, and leaves
 * the deficit alone. A withheld or late cut carries no measurement and
 * changes nothing. A produced span counts down the drain floor; with room
 * for at least need_spare more scenes it extends the clean run, and window
 * consecutive such spans lower the deficit by one scene at that span's
 * afford; with less room it ends the run (the plan sits at the guard's
 * edge, and probing past it would cut). From any state, a run of spans with
 * room reaches the plan with less than need_spare room in at most window
 * spans per scene of deficit, and the deficit is at most half of afford.
 * @param c The controller.
 * @param cause Why the span ended with scenes unproduced, or OP_EAGER_CUT_NONE.
 * @param afford The scenes the span's budget afforded.
 * @param spare The further scenes the guard would have started when the plan finished; read when not cut.
 * @param made The scenes the span produced; read on a ring cut.
 * @param len_ns The span's length in nanoseconds; read on a ring cut.
 * @param window The clean run that gives back one scene, in spans.
 * @param need_spare The room a span must leave to count as clean, in scenes.
 * @param drain_runs The spans a ring cut's drain floor applies for. */
static inline void op_eager_count_step(op_eager_count_t *c, op_eager_cut_t cause, int afford, int spare,
                                       int made, int64_t len_ns, int window, int need_spare, int drain_runs)
{
    switch (cause) {
    case OP_EAGER_CUT_DEADLINE:
        c->deficit += op_eager_count_unit(afford);
        if (c->deficit > OP_EAGER_COUNT_MAX_DEFICIT) c->deficit = OP_EAGER_COUNT_MAX_DEFICIT;
        c->clean = 0;
        return;
    case OP_EAGER_CUT_RING:
        c->drain_ns = len_ns / (made > 0 ? made : 1);
        c->drain_left = drain_runs;
        return;
    case OP_EAGER_CUT_WITHHELD:
    case OP_EAGER_CUT_LATE:
        return;
    case OP_EAGER_CUT_NONE:
        break;
    }
    if (c->drain_left > 0) c->drain_left--;
    if (spare < need_spare) { c->clean = 0; return; }
    if (++c->clean < window) return;
    c->clean = 0;
    c->deficit -= op_eager_count_unit(afford);
    if (c->deficit < 0) c->deficit = 0;
}

/** @brief Return how many scenes the produce guard starts in room nanoseconds: the last needs the tail cost, each other one the mean cost.
 *
 * The guard starts a scene while its start plus the tail cost is at or
 * before the deadline, so the room holds 1 plus the mean costs that fit in
 * room - tail, or 0 when room is below tail. Capped at
 * OP_EAGER_LADDER_MAX_AFFORD.
 * @param room The time left before the deadline, in nanoseconds.
 * @param ema The per-scene production cost mean in nanoseconds; below 1 counts as 1.
 * @param tail The per-scene tail cost in nanoseconds.
 * @return The scene count, 0 to OP_EAGER_LADDER_MAX_AFFORD. */
static inline int op_eager_guard_room(int64_t room, int64_t ema, int64_t tail)
{
    if (room < 0 || room < tail) return 0;
    if (ema < 1) ema = 1;
    const int64_t n = 1 + (room - tail) / ema;
    return n > OP_EAGER_LADDER_MAX_AFFORD ? OP_EAGER_LADDER_MAX_AFFORD : (int)n;
}

/** @brief Re-space a free span's unproduced instants to m scenes, evenly between its last produced instant and its last instant, and return the new count.
 *
 * target[0 .. i-1] are produced and stay; target[n-1], the span's last
 * instant, stays, so the next span continues after the same instant. The
 * m instants are prev + k * (last - prev) / m for k = 1 .. m, prev being
 * target[i-1], the m-th exactly last. m is lowered until the spacing is at
 * least floor_ns and at least 1 ns, and until i + m is at most max_n; it is
 * at least 1, which keeps the last instant even when it lies closer than
 * floor_ns to prev. With nothing produced (i < 1), nothing left (i >= n),
 * m below 1 (the guard stops before another scene and the plan is cut as
 * planned) or last not after prev, the plan is unchanged.
 * @param target The span's instants; entries i .. return value - 1 are rewritten.
 * @param i The number of instants produced, the index of the next one.
 * @param n The number of instants planned.
 * @param m The scenes the room left affords (op_eager_guard_room).
 * @param floor_ns The least spacing in nanoseconds, or 0 for none.
 * @param max_n The capacity of target.
 * @return The number of instants planned after the re-plan. */
static inline int op_eager_replan(int64_t *target, int i, int n, int m, int64_t floor_ns, int max_n)
{
    if (i < 1 || i >= n || m < 1) return n;
    const int64_t prev = target[i - 1], len = target[n - 1] - prev;
    if (len <= 0) return n;
    if (floor_ns > 0 && len / floor_ns < m) m = (int)(len / floor_ns);
    if (len < m) m = (int)len;
    if (i + m > max_n) m = max_n - i;
    if (m < 1) m = 1;
    const int64_t last = target[n - 1];
    for (int k = 1; k < m; k++) target[i - 1 + k] = prev + len * k / m;
    target[i + m - 1] = last;
    return i + m;
}

/** @brief Return the cut cause a free span's seed plan of seed scenes would have closed with, and its spare, from the span the re-plan reshaped.
 *
 * The re-plan changes the count after the update VBL, so the count
 * controller, which only seeds the plan, is stepped on the seed: a span
 * whose produce walk passed fewer than seed instants (produced, or skipped
 * because their instant had gone) would have cut the seed at the deadline;
 * one that passed seed or more would have finished it with done - seed
 * scenes of room, plus the guard's room at the end when the reshaped plan
 * was not cut. A ring, withheld or late cut is passed through with spare.
 * A span the re-plan did not reshape passes all seed instants unless cut,
 * so the view is then the span's own cause and spare.
 * @param cause Why the span ended with scenes unproduced, or OP_EAGER_CUT_NONE.
 * @param seed The scenes the span's seed plan held.
 * @param done The instants the span's produce walk passed.
 * @param spare The guard's room when the span's plan finished; 0 when cut.
 * @param seed_spare Receives the spare the seed plan would have finished with.
 * @return The cause the seed plan would have closed with. */
static inline op_eager_cut_t op_eager_seed_view(op_eager_cut_t cause, int seed, int done, int spare,
                                                int *seed_spare)
{
    if (cause != OP_EAGER_CUT_NONE && cause != OP_EAGER_CUT_DEADLINE) { *seed_spare = spare; return cause; }
    if (done < seed) { *seed_spare = 0; return OP_EAGER_CUT_DEADLINE; }
    *seed_spare = done - seed + (cause == OP_EAGER_CUT_NONE ? spare : 0);
    return OP_EAGER_CUT_NONE;
}

/** @brief Return the display-grid slot of the q-th of k scenes spread over ns slots, the first and last slot kept.
 *
 * The slot is q * (ns - 1) / (k - 1) rounded to nearest, halves up, so the
 * k slots run from 0 to ns - 1 and consecutive slots differ by the floor or
 * the ceiling of (ns - 1) / (k - 1): the ns - k skipped slots are spread as
 * evenly as whole slots allow, and while 2 * (k - 1) >= ns - 1 no two of them
 * are adjacent. k at or above ns gives slot q (every slot); k of 1 gives slot
 * 0, the canonical scene's. Keeping the last slot leaves the span's last
 * target where a plan of every slot puts it, so the next span continues
 * after it as it does at level 1.
 * @param q The scene's index, 0 to min(k, ns) - 1.
 * @param k The scenes planned, at least 1.
 * @param ns The span's slot count, at least 1.
 * @return The slot index, 0 to ns - 1. */
static inline int op_eager_spread_slot(int q, int k, int ns)
{
    if (k >= ns) return q;
    if (k <= 1) return 0;
    return (int)((2LL * q * (ns - 1) + (k - 1)) / (2LL * (k - 1)));
}

/** @brief Return the scenes a fill span plans on a display grid of ns slots: the count controller's plan of min(afford, ns), at most ns.
 *
 * The controller (op_eager_count_plan) is priced on the slots the span can
 * show, not on an afford above them, so one deadline cut at a full plan
 * removes one slot's scene; at deficit 0 a span that affords its slots plans
 * all of them, the plan of the divisor ladder at level 1.
 * @param c The grid's count controller.
 * @param afford The scenes the budget affords, at least 1.
 * @param ns The span's slot count, at least 1.
 * @return The scene count, 1 to ns. */
static inline int op_eager_fill_count(const op_eager_count_t *c, int afford, int ns)
{
    const int k = op_eager_count_plan(c, afford < ns ? afford : ns);
    return k > ns ? ns : k;
}

/** @brief Move a grid fill span's count controller for the span just closed: one scene fewer per cut, all of them back after a clean run.
 *
 * The ladder's hysteresis in count space. A deadline cut, or a ring cut (on
 * a display grid the slots' spacing is the refresh, so a ring that stays full
 * says the present thread shows fewer scenes than the plan holds), raises
 * the deficit by one scene at the span's afford, up to
 * OP_EAGER_COUNT_MAX_DEFICIT, and ends the clean run. A withheld or late cut
 * carries no measurement and changes nothing. A produced span with room for
 * at least need_spare more scenes extends the clean run, and window
 * consecutive such spans clear the deficit in one move, as the divisor
 * ladder falls in one move to the level its run measured; with less room it
 * ends the run. A stall's cuts, priced at the small afford the stall left,
 * then cost window spans once the spans clear, not window spans per scene.
 * The ring cut's drain floor (drain_ns, drain_left) is not used.
 * @param c The grid's count controller.
 * @param cause Why the span ended with scenes unproduced, or OP_EAGER_CUT_NONE.
 * @param afford The afford the span's count was planned from (min(afford, ns)).
 * @param spare The further scenes the guard would have started when the plan finished; read when not cut.
 * @param window The clean run that clears the deficit, in spans.
 * @param need_spare The room a span must leave to count as clean, in scenes. */
static inline void op_eager_fill_step(op_eager_count_t *c, op_eager_cut_t cause, int afford, int spare,
                                      int window, int need_spare)
{
    switch (cause) {
    case OP_EAGER_CUT_DEADLINE:
    case OP_EAGER_CUT_RING:
        c->deficit += op_eager_count_unit(afford);
        if (c->deficit > OP_EAGER_COUNT_MAX_DEFICIT) c->deficit = OP_EAGER_COUNT_MAX_DEFICIT;
        c->clean = 0;
        return;
    case OP_EAGER_CUT_WITHHELD:
    case OP_EAGER_CUT_LATE:
        return;
    case OP_EAGER_CUT_NONE:
        break;
    }
    if (spare < need_spare) { c->clean = 0; return; }
    if (++c->clean < window) return;
    c->clean = 0;
    c->deficit = 0;
}

/* A cost sample above this multiple of the running mean enters the mean at
 * the multiple. One stall (a debugger stop, a suspend, a blocked present)
 * then moves the mean by at most (OP_EAGER_COST_SAMPLE_CAP - 1) / 8 of it,
 * and a sustained rise still grows the mean by up to
 * (7 + OP_EAGER_COST_SAMPLE_CAP) / 8 per sample until the cap no longer
 * binds. */
#define OP_EAGER_COST_SAMPLE_CAP 4

/** @brief Return a cost sample bounded at OP_EAGER_COST_SAMPLE_CAP times the running mean; the sample whole while the mean is 0.
 *
 * @param ema The running mean in nanoseconds, 0 before the first sample.
 * @param sample The measured cost in nanoseconds.
 * @return The sample the mean and its deviation take. */
static inline int64_t op_eager_cost_sample(int64_t ema, int64_t sample)
{
    if (ema <= 0) return sample;
    const int64_t cap = ema * OP_EAGER_COST_SAMPLE_CAP;
    return sample > cap ? cap : sample;
}

/** @brief Fold one cost sample into an 8-sample exponential mean: the first sample seeds it, each later one enters bounded by op_eager_cost_sample.
 *
 * @param ema The running mean in nanoseconds, 0 before the first sample.
 * @param sample The measured cost in nanoseconds.
 * @return The new mean. */
static inline int64_t op_eager_cost_ema(int64_t ema, int64_t sample)
{
    if (ema <= 0) return sample;
    return (ema * 7 + op_eager_cost_sample(ema, sample)) / 8;
}

#endif /* OPENPETE_EAGER_LADDER_H */
