/* tests/unit/eager_ladder_test.c - the eager producer's capacity estimate
 * and decimation ladder on synthetic spans and on recorded plan lines.
 *
 * The recorded rows are the OPENPETE_EAGER_DBG=1 plan lines of a Spyro 2
 * Skelos Badlands freecam run under --texture-mapping affine at 480 Hz:
 * slots, budget and the per-scene tail cost (mean plus three deviations).
 * The mean is recovered as tail - 3 * dev with dev the run's final 0.762 ms,
 * which reproduces the logged afford on 12 of the 13 post-warmup rows under
 * the previous pricing (the deviation moved during the run). The previous
 * pricing (eight deviations whatever the count) and the previous ladder
 * (rise to ceil(ns / afford) or one past a cut at once; fall after eight
 * spans that cover a lower level with afford * 4 / 5 scenes) are kept here
 * as the negative control: on the recorded rows they must hold the
 * canonical-only level where the current ones fall to two scenes per span.
 *
 * The free-span rows are the EAGER_PLAN records of Windows runs with vsync
 * off and --render-fps 10000 (RTX 4090, 2560x1440, extended LOD, spawn
 * poses): the per-span median slots, afford and tail cost. The grid there
 * was floored at the tail cost, which gives a continuing span of length len
 * floor(len / tail) slots; that count against afford is the negative
 * control, which halves the plan.
 *
 * The count rows are 240 consecutive spans of two levels from Windows runs
 * with the guard-window price (vsync off, the same flags): per span the
 * price the run planned, the sum price the live cost estimate gives
 * (op_eager_ladder_afford, reconstructed from the run's scene costs and
 * equal to the logged afford wherever that price ran), and the capacity, the
 * scenes a replay of the produce guard on the span's recorded costs starts.
 * A span is cut when it plans more than its capacity. The transition rows
 * are the EAGER_PLAN records of a Spyro 1 attract-demo replay (vsync off,
 * the same flags) through a level load: one-VBL spans at afford 128 whose
 * plans the scene ring cut, with the scenes each span produced (the present
 * thread's drain), followed here by gameplay spans at afford 12. The
 * previous count controller (an absolute shortfall below afford) is kept as
 * the negative control: it carries the transition's shortfall into the
 * level and plans one scene per span there. The vsync rows are
 * spans of the vsync-on runs with that price that it planned below the
 * slot count, where the sum price stays at or above it.
 *
 * The spread rows are the fill cadence's slot placement
 * (op_eager_spread_slot) on the named cases and on every k of every ns up to
 * the ladder's bound, with the first-k placement as the control; the fill
 * count rows are its controller (op_eager_fill_step) against a fixed
 * capacity and through a stall, with the free span's count step as the
 * control.
 *
 * The chain rows run 1000 continued spans with the period floored at a
 * present-thread cost of 200 ms over the 33.4 ms tick, on the display-grid
 * and the free planner, from no lead and from a lead of three periods: each
 * span's first instant stays under one period past its own first slot
 * (op_eager_span_continue_in), with the continuation that kept whole periods
 * as the control. The retarget rows run the display-grid chain at a 480 Hz
 * period under the tick: no span's first instant is at or before the
 * previous span's last (op_eager_span_continue_in), with the phase-only
 * continuation (op_eager_span_continue) as the control.
 *
 * `eager_ladder_test replay` prints the per-row table. */

#include "openpete_eager_ladder.h"

#include <stdio.h>
#include <string.h>

#define WINDOW 8
#define MS 1000000LL

static int g_fail;

/** @brief Record a failed check and print it.
 * @param ok Nonzero when the check passed.
 * @param what The check's description. */
static void check(int ok, const char *what)
{
    if (!ok) { g_fail = 1; printf("FAIL: %s\n", what); }
}

/** @brief The previous pricing: N scenes at the mean plus eight deviations, whatever N.
 * @param budget The budget in nanoseconds.
 * @param ema The per-scene mean in nanoseconds.
 * @param dev The per-scene deviation in nanoseconds.
 * @return The scenes afforded, at least 1. */
static int old_afford(int64_t budget, int64_t ema, int64_t dev)
{
    const int64_t room = budget > 8 * dev ? budget - 8 * dev : 0;
    const int a = (int)(room / ema);
    return a < 1 ? 1 : a;
}

/** @brief The previous ladder step: rise at once, fall after a run covering a lower level with a quarter margin.
 * @param s The ladder state (since_short unused).
 * @param ns The span's slot count.
 * @param afford The scenes afforded.
 * @param cut Nonzero after a cut span.
 * @return The level the span plans at. */
static int old_step(op_eager_ladder_t *s, int ns, int afford, int cut)
{
    int need = (ns + afford - 1) / afford;
    if (need < 1) need = 1;
    if (cut && need <= s->level) need = s->level + 1;
    if (need > s->level) { s->level = need; s->headroom_runs = 0; s->hold_level = 0; return s->level; }
    const int m = afford * 4 / 5;
    const int cover = m >= 1 ? (ns + m - 1) / m : s->level;
    if (cover >= s->level) { s->headroom_runs = 0; s->hold_level = 0; return s->level; }
    if (cover > s->hold_level) s->hold_level = cover;
    if (++s->headroom_runs < WINDOW) return s->level;
    s->level = s->hold_level > 1 ? s->hold_level : 1;
    s->headroom_runs = 0; s->hold_level = 0;
    return s->level;
}

/** @brief Return a fresh ladder at level 1 with no shortfall in its window.
 * @return The state. */
static op_eager_ladder_t fresh(void)
{
    op_eager_ladder_t s;
    memset(&s, 0, sizeof s);
    s.level = 1; s.since_short = WINDOW;
    return s;
}

/** @brief Check the sum pricing: equal to the eight-deviation constant at sixteen scenes, cheaper below. */
static void test_afford(void)
{
    /* 16 scenes of 1 ms, deviation 0.1 ms: the sum needs 16 + 2 * 4 * 0.1 = 16.8 ms. */
    check(op_eager_ladder_afford(16800000, 1 * MS, 100000) == 16, "afford: 16 at exactly 16.8 ms");
    check(op_eager_ladder_afford(16799999, 1 * MS, 100000) == 15, "afford: 15 below 16.8 ms");
    check(old_afford(16800000, 1 * MS, 100000) == 16, "control: old pricing agrees at N = 16");
    /* Two scenes of 7.7 ms, deviation 0.762 ms: 15.4 + 2 * 1.414 * 0.762 = 17.56 ms. */
    check(op_eager_ladder_afford(18400000, 7700000, 762000) == 2, "afford: 2 of 7.7 ms in 18.4 ms");
    check(old_afford(18400000, 7700000, 762000) == 1, "control: old pricing affords 1 there");
    check(op_eager_ladder_afford(0, 7700000, 762000) == 1, "afford: floor 1");
    check(op_eager_ladder_afford(1000 * MS, 0, 0) == OP_EAGER_LADDER_MAX_AFFORD, "afford: no cost yet");
}

/** @brief Check the transitions: isolated shortfall, recurrence, cut recurrence, and the one-move fall. */
static void test_steps(void)
{
    op_eager_ladder_t s = fresh();
    /* ns 4: afford 4 plans level 1. One span affording 2 plans level 2 for itself only. */
    check(op_eager_ladder_step(&s, 4, 4, 0, WINDOW) == 1, "steady at 1");
    check(op_eager_ladder_step(&s, 4, 2, 0, WINDOW) == 2 && s.level == 1, "isolated shortfall: span 2, standing 1");
    check(op_eager_ladder_step(&s, 4, 4, 0, WINDOW) == 1, "back to 1 next span");
    /* Recurrence within the window raises the standing level. */
    check(op_eager_ladder_step(&s, 4, 2, 0, WINDOW) == 2 && s.level == 2, "recurrent shortfall: standing 2");
    /* Seven clean spans covering level 1 do not fall; the eighth does, in one move. */
    for (int i = 0; i < WINDOW - 1; i++) op_eager_ladder_step(&s, 4, 4, 0, WINDOW);
    check(s.level == 2, "no fall before the run completes");
    check(op_eager_ladder_step(&s, 4, 4, 0, WINDOW) == 1 && s.level == 1, "fall after the run");
    /* An isolated cut holds the level; a second within the window raises it by one. */
    op_eager_ladder_t c = fresh();
    for (int i = 0; i < WINDOW; i++) op_eager_ladder_step(&c, 4, 4, 0, WINDOW);
    check(op_eager_ladder_step(&c, 4, 4, 1, WINDOW) == 1 && c.level == 1, "isolated cut: level held");
    check(op_eager_ladder_step(&c, 4, 4, 1, WINDOW) == 2 && c.level == 2, "recurrent cut: level + 1");
    /* A fall goes to the highest need of the run, not one step. */
    op_eager_ladder_t f = fresh();
    f.level = 16;
    for (int i = 0; i < WINDOW; i++) op_eager_ladder_step(&f, 16, i == 3 ? 4 : 8, 0, WINDOW);
    check(f.level == 4, "fall to the run's highest need (4), from 16");
    /* The trap: ns 5 at the canonical-only level, afford 2 every span. */
    op_eager_ladder_t t = fresh(), o = fresh();
    t.level = o.level = 5;
    for (int i = 0; i < 4 * WINDOW; i++) { op_eager_ladder_step(&t, 5, 2, 0, WINDOW); old_step(&o, 5, 2, 0); }
    check(t.level == 3, "trap: afford 2 at ns 5 falls to level 3 (two scenes)");
    check(o.level == 5, "control: the old ladder holds level 5 at afford 2");
}

/* The recorded plan lines: VBL, slots, budget ms, tail ms (0 before warmup), logged afford, logged level. */
static const struct { int f, ns; double budget, tail; int afford, level; } k_rows[] = {
    {  789, 23, 14.71, 0.000, 23, 1 }, {  791, 16, 18.33, 0.000, 16, 1 },
    {  793,  2, 11.33, 0.000,  2, 1 }, {  795,  2, 18.61, 0.000,  2, 1 },
    {  797,  2, 11.41, 0.000,  2, 1 }, {  803,  1, 17.94, 9.580,  1, 2 },
    {  811,  3, 18.77, 9.256,  1, 3 }, {  905,  4, 18.40, 9.081,  1, 4 },
    {  907,  5, 19.96, 8.947,  1, 5 }, { 1517,  6, 14.76, 6.862,  1, 6 },
    { 1571,  5, 21.07, 8.676,  2, 5 }, { 1845,  6, 19.66, 6.718,  3, 3 },
    { 1933,  4, 16.71, 8.705,  1, 4 }, { 1953,  5, 14.01, 8.414,  1, 5 },
    { 2543,  5, 21.24, 6.836,  3, 4 }, { 2571,  5, 15.26, 7.845,  1, 5 },
    { 2897,  6, 15.63, 7.352,  1, 6 }, { 2971,  5, 21.50, 8.787,  2, 5 },
};

/** @brief Replay the recorded rows through both ladders; print the table when asked.
 *
 * Each row is fed once in order (a cut flag is not fed: the rows past warmup
 * are uncut), then held for 2 * WINDOW spans from the state the sequence
 * left, which is the level the ladder settles at if every span looked like
 * that row.
 * @param print Nonzero to print the table. */
static void test_replay(int print)
{
    const int64_t dev = 762000;
    op_eager_ladder_t s = fresh(), o = fresh();
    if (print) printf("vbl ns budget  afford(log old new)  level(log old new)  settled(old new)  scenes/span(old new)\n");
    int settled_new_two = 0, settled_old_one = 0, rows = 0;
    for (size_t i = 0; i < sizeof k_rows / sizeof k_rows[0]; i++) {
        const int64_t b = (int64_t)(k_rows[i].budget * 1e6 + 0.5);
        const int64_t ema = k_rows[i].tail > 0 ? (int64_t)(k_rows[i].tail * 1e6 + 0.5) - 3 * dev : 0;
        const int ao = ema > 0 ? old_afford(b, ema, dev) : k_rows[i].afford;
        const int an = ema > 0 ? op_eager_ladder_afford(b, ema, dev) : k_rows[i].afford;
        const int lo = old_step(&o, k_rows[i].ns, ao, 0);
        const int ln = op_eager_ladder_step(&s, k_rows[i].ns, an, 0, WINDOW);
        op_eager_ladder_t so = o, sn = s;
        int po = lo, pn = ln;
        for (int r = 0; r < 2 * WINDOW; r++) {
            po = old_step(&so, k_rows[i].ns, ao, 0);
            pn = op_eager_ladder_step(&sn, k_rows[i].ns, an, 0, WINDOW);
        }
        const int ko = (k_rows[i].ns + po - 1) / po, kn = (k_rows[i].ns + pn - 1) / pn;
        if (print)
            printf("%4d %2d %6.2f  %3d %3d %3d            %2d %3d %3d           %3d %3d            %d %d\n",
                   k_rows[i].f, k_rows[i].ns, k_rows[i].budget, k_rows[i].afford, ao, an,
                   k_rows[i].level, lo, ln, po, pn, ko, kn);
        if (ema > 0 && k_rows[i].ns >= 2) { rows++; settled_new_two += kn >= 2; settled_old_one += ko == 1; }
    }
    if (print) printf("rows past warmup with 2+ slots %d: settled at 2+ scenes new %d, at 1 scene old %d\n",
                      rows, settled_new_two, settled_old_one);
    /* The one row that settles at one scene under the new pricing affords one (14.01 ms budget). */
    check(settled_new_two >= rows - 1, "replay: the new ladder settles at two scenes on all but one row");
    check(settled_old_one >= rows - 3, "control: the old ladder settles canonical-only on most rows");
}

/** @brief Check the free-span arithmetic: count instants fit len, the floor lowers the count, at least one. */
static void test_fill(void)
{
    int n = 0;
    int64_t p = op_eager_ladder_fill(33 * MS, 22, 0, 1, &n);
    check(n == 22 && p == 33 * MS / 22 && 22 * p <= 33 * MS, "fill: 22 after the anchor, last within the span");
    p = op_eager_ladder_fill(33 * MS, 22, 0, 0, &n);
    check(n == 22 && p == 33 * MS / 21 && 21 * p <= 33 * MS, "fill: 22 from the start, 21 gaps");
    p = op_eager_ladder_fill(33 * MS, 22, 3 * MS, 1, &n);
    check(n == 11 && p == 3 * MS, "fill: the floor lowers the count to 11");
    p = op_eager_ladder_fill(33 * MS, 22, 3 * MS, 0, &n);
    check(n == 12 && p == 3 * MS, "fill: the floor from the start, 12 with the fencepost");
    p = op_eager_ladder_fill(0, 5, 2 * MS, 1, &n);
    check(n == 1 && p == 2 * MS, "fill: an empty span holds one instant");
    p = op_eager_ladder_fill(10 * MS, 1, 0, 0, &n);
    check(n == 1 && p == 10 * MS, "fill: one instant from the start");
}

/* Free-span records (vsync off): level, median slots, afford and tail ms. */
static const struct { const char *level; int ns, afford; double tail; } k_free[] = {
    { "s1 stone_hill",    23, 22, 1.531 }, { "s1 artisans",    22, 20, 1.590 },
    { "s2 summer_forest", 25, 24, 1.364 }, { "s2 hurricos",    25, 24, 1.404 },
    { "s3 sunny_villa",   16, 14, 2.190 }, { "s3 cloud_spires", 18, 16, 1.903 },
};

/** @brief Replay the free-span rows: the tail-floored grid halves the plan, the filled span plans afford at level 1.
 *
 * Each row is held for 2 * WINDOW spans from a fresh ladder. The span length
 * is ns + 1/2 tail periods, which the tail-floored grid enumerates as ns
 * continuing slots.
 * @param print Nonzero to print the table. */
static void test_free(int print)
{
    if (print) printf("level             ns afford  old(level planned)  new(slots level planned)\n");
    for (size_t i = 0; i < sizeof k_free / sizeof k_free[0]; i++) {
        const int64_t tail = (int64_t)(k_free[i].tail * 1e6 + 0.5);
        const int64_t len = k_free[i].ns * tail + tail / 2;
        const int a = k_free[i].afford;
        int n = 0;
        op_eager_ladder_fill(len, a, 0, 1, &n);
        op_eager_ladder_t so = fresh(), sn = fresh();
        int lo = 1, ln = 1;
        for (int r = 0; r < 2 * WINDOW; r++) {
            lo = op_eager_ladder_step(&so, (int)(len / tail), a, 0, WINDOW);
            ln = op_eager_ladder_step(&sn, n, a, 0, WINDOW);
        }
        const int ko = ((int)(len / tail) + lo - 1) / lo, kn = (n + ln - 1) / ln;
        if (print) printf("%-17s %2d %3d        %d %3d               %3d %d %3d\n",
                          k_free[i].level, k_free[i].ns, a, lo, ko, n, ln, kn);
        char what[96];
        snprintf(what, sizeof what, "free %s: level 1, planned within one of afford %d (got %d)", k_free[i].level, a, kn);
        check(ln == 1 && kn >= a - 1 && kn <= a, what);
        snprintf(what, sizeof what, "control %s: the tail-floored grid plans at level 2", k_free[i].level);
        check(lo == 2 && ko <= (k_free[i].ns + 1) / 2, what);
    }
}

/* The span chain under a presenter slower than the tick: the planned period
 * is floored at a present-thread cost P above the tick T, so each continued
 * span moves the previous span's grid P - T further ahead of the plan
 * instant. Spans step T apart with up to 2 ms of start jitter; each span's
 * first render-grid slot is the 60 Hz slot at or after its plan instant; the
 * span ends one tick plus the lag past the plan instant. */
#define CHAIN_SPANS 1000
#define CHAIN_TICK  33430222LL   /* 1125000/37609 Hz */
#define CHAIN_GRID  16666667LL
#define CHAIN_LAG   (8 * MS)
#define CHAIN_P     (200 * MS)

/** @brief The previous continuation: whole periods of the old grid's lead kept.
 * @param last The previous span's last instant.
 * @param first The span's first render-grid slot.
 * @param end The span's lag-extended end, unused.
 * @param period The planned grid period.
 * @param prev Receives the previous span's last instant.
 * @return The span's first instant. */
static int64_t old_span_continue(int64_t last, int64_t first, int64_t end, int64_t period, int64_t *prev)
{
    (void)end;
    int64_t f = last + period;
    *prev = last;
    if (f - first > period) f = first + ((f - first) / period) * period;
    return f;
}

/** @brief The phase-only continuation, which keeps the old grid's phase whenever it lies a period past the span's slot.
 * @param last The previous span's last instant.
 * @param first The span's first render-grid slot.
 * @param end The span's lag-extended end, unused.
 * @param period The planned grid period.
 * @param prev Receives the old grid's instant before the result.
 * @return The span's first instant. */
static int64_t phase_span_continue(int64_t last, int64_t first, int64_t end, int64_t period, int64_t *prev)
{
    (void)end;
    return op_eager_span_continue(last, first, period, prev);
}

/** @brief Run CHAIN_SPANS spans of one planner and continuation rule from a seeded lead.
 * @param cont The continuation rule.
 * @param free_span Nonzero for the free span's placement (op_eager_ladder_fill after prev), else the display grid's.
 * @param seed The previous span's last instant at the first span, in periods past its plan instant (0: none).
 * @param max_over Receives the largest first instant minus the span's own first slot, nanoseconds.
 * @param last_lead Receives the last span's first instant minus its plan instant, nanoseconds.
 * @param max_lead Receives the largest first instant minus plan instant, nanoseconds. */
static void chain_run(int64_t (*cont)(int64_t, int64_t, int64_t, int64_t, int64_t *), int free_span, int seed,
                      int64_t *max_over, int64_t *last_lead, int64_t *max_lead)
{
    const int64_t base = 1000 * MS;
    int64_t last = seed ? base + seed * CHAIN_P : 0;
    *max_over = INT64_MIN; *max_lead = INT64_MIN; *last_lead = 0;
    for (int n = 0; n < CHAIN_SPANS; n++) {
        const int64_t now = base + n * CHAIN_TICK + (n % 3) * MS;
        const int64_t slot = ((now + CHAIN_GRID - 1) / CHAIN_GRID) * CHAIN_GRID;
        int64_t first = slot, prev = 0;
        int64_t end = now + CHAIN_TICK;
        if (end < first) end = first;
        end += CHAIN_LAG;
        if (last && last + CHAIN_P > first) first = cont(last, first, end, CHAIN_P, &prev);
        int k = 1;
        if (free_span) {
            const int64_t start = prev ? prev : first;
            const int64_t p = op_eager_ladder_fill(end - start, 3, CHAIN_P, prev != 0, &k);
            if (prev) first = prev + p;
            last = first + p * (k - 1);
        } else {
            while (first + CHAIN_P * k <= end) k++;
            last = first + CHAIN_P * (k - 1);
        }
        if (first - slot > *max_over) *max_over = first - slot;
        if (first - now > *max_lead) *max_lead = first - now;
        *last_lead = first - now;
    }
}

/** @brief Check that a continued span's first instant stays under one period past its own first slot when the period exceeds the tick, on both planners, from no lead and from a lead of three periods. */
static void test_chain(void)
{
    static const char *const planner[] = { "grid", "free" };
    static const int seeds[] = { 0, 3 };
    char what[192];
    for (int fs = 0; fs < 2; fs++)
        for (size_t si = 0; si < sizeof seeds / sizeof seeds[0]; si++) {
            int64_t over, last_lead, max_lead, oover, olast, omax;
            chain_run(op_eager_span_continue_in, fs, seeds[si], &over, &last_lead, &max_lead);
            chain_run(old_span_continue, fs, seeds[si], &oover, &olast, &omax);
            snprintf(what, sizeof what,
                     "chain %s seed %dP: first instant %.3f ms past the span's slot, under P %.0f ms (lead max %.3f ms)",
                     planner[fs], seeds[si], (double)over / MS, (double)CHAIN_P / MS, (double)max_lead / MS);
            check(over < CHAIN_P && max_lead < CHAIN_P + CHAIN_GRID, what);
            snprintf(what, sizeof what, "control %s seed %dP: the old continuation ends %.3f ms ahead, over P",
                     planner[fs], seeds[si], (double)olast / MS);
            check(olast > CHAIN_P, what);
        }
}

/* The retarget chain: a 480 Hz display-grid period under the tick, the lag
 * of a 4.5 ms tick tail, spans T apart with up to 2 ms of start jitter. */
#define RETARGET_P   2083333LL
#define RETARGET_LAG 4500000LL

/** @brief Run CHAIN_SPANS display-grid spans at RETARGET_P and count the spans whose first instant is at or before the previous span's last.
 * @param cont The continuation rule.
 * @param max_over Receives the largest first instant minus the span's lag-extended end, nanoseconds.
 * @return The spans that retarget an instant the previous span planned. */
static int retarget_run(int64_t (*cont)(int64_t, int64_t, int64_t, int64_t, int64_t *), int64_t *max_over)
{
    const int64_t base = 1000 * MS;
    int64_t last = 0;
    int back = 0;
    *max_over = INT64_MIN;
    for (int n = 0; n < CHAIN_SPANS; n++) {
        const int64_t now = base + n * CHAIN_TICK + (n % 3) * MS;
        const int64_t slot = ((now + RETARGET_P - 1) / RETARGET_P) * RETARGET_P;
        int64_t first = slot, prev = 0;
        const int64_t end = now + CHAIN_TICK + RETARGET_LAG;
        if (last && last + RETARGET_P > first) first = cont(last, first, end, RETARGET_P, &prev);
        back += last && first <= last;
        if (first - end > *max_over) *max_over = first - end;
        int k = 1;
        while (first + RETARGET_P * k <= end) k++;
        last = first + RETARGET_P * (k - 1);
    }
    return back;
}

/** @brief Check that with the period under the tick no continued span retargets an instant the previous span planned, and that its first instant stays inside its own span. */
static void test_retarget(void)
{
    char what[192];
    int64_t over, pover;
    const int back = retarget_run(op_eager_span_continue_in, &over);
    const int pback = retarget_run(phase_span_continue, &pover);
    snprintf(what, sizeof what, "retarget: %d of %d spans start at or before the previous last instant, first at most %.3f ms past the end",
             back, CHAIN_SPANS, (double)over / MS);
    check(back == 0 && over <= 0, what);
    snprintf(what, sizeof what, "control retarget: the phase-only continuation starts %d of %d spans at or before the previous last instant",
             pback, CHAIN_SPANS);
    check(pback > CHAIN_SPANS / 2, what);
}

static const unsigned char k_stone_hill_sum[] = {
        22, 22, 22, 23, 22, 22, 22, 21, 23, 22, 22, 22, 22, 21, 22, 21, 21, 22, 21, 22, 19, 21, 21, 20,
        21, 21, 23, 23, 22, 22, 22, 23, 23, 22, 23, 23, 22, 22, 22, 23, 21, 21, 22, 21, 21, 22, 23, 22,
        23, 23, 22, 23, 19, 17, 20, 22, 22, 22, 23, 22, 21, 22, 24, 20, 22, 22, 21, 23, 21, 21, 23, 22,
        21, 21, 20, 21, 22, 23, 24, 23, 22, 22, 22, 24, 24, 21, 19, 21, 20, 22, 21, 23, 22, 23, 24, 23,
        21, 22, 23, 23, 24, 23, 22, 22, 22, 24, 23, 24, 23, 23, 23, 21, 21, 21, 22, 23, 23, 22, 21, 19,
        23, 23, 23, 23, 20, 21, 23, 23, 22, 24, 23, 23, 22, 22, 22, 22, 22, 23, 23, 24, 21, 20, 21, 22,
        21, 21, 23, 22, 22, 22, 22, 19, 17, 21, 22, 22, 20, 22, 23, 23, 23, 23, 22, 22, 23, 22, 21, 22,
        21, 22, 22, 22, 21, 21, 21, 21, 21, 21, 22, 23, 21, 21, 22, 21, 19, 19, 21, 23, 22, 22, 22, 24,
        22, 22, 20, 21, 21, 22, 22, 22, 22, 22, 22, 20, 21, 20, 21, 22, 21, 22, 22, 23, 23, 21, 19, 22,
        21, 22, 19, 22, 21, 21, 22, 23, 21, 22, 23, 23, 22, 23, 20, 23, 20, 21, 22, 22, 21, 22, 22, 22,
};
static const unsigned char k_stone_hill_win[] = {
        20, 22, 20, 22, 21, 22, 22, 20, 22, 20, 20, 21, 20, 20, 21, 20, 20, 22, 20, 21, 18, 20, 20, 20,
        20, 20, 22, 22, 20, 20, 22, 22, 22, 22, 22, 22, 20, 21, 22, 22, 20, 20, 22, 20, 20, 20, 22, 22,
        22, 22, 22, 22, 18, 16, 19, 22, 20, 20, 22, 22, 20, 21, 22, 18, 20, 20, 20, 22, 20, 20, 22, 20,
        20, 20, 19, 20, 20, 22, 22, 22, 20, 21, 21, 22, 22, 20, 18, 20, 20, 20, 20, 22, 21, 22, 22, 22,
        20, 21, 22, 22, 22, 22, 21, 21, 22, 22, 22, 23, 22, 22, 22, 20, 20, 20, 20, 22, 22, 20, 20, 18,
        22, 22, 22, 22, 19, 20, 22, 22, 21, 22, 22, 22, 20, 20, 22, 20, 21, 22, 22, 22, 20, 20, 20, 20,
        20, 20, 22, 22, 20, 20, 20, 18, 16, 20, 20, 22, 19, 20, 22, 22, 22, 22, 22, 22, 22, 22, 20, 21,
        20, 22, 20, 20, 20, 20, 20, 20, 20, 20, 20, 22, 20, 20, 20, 20, 18, 18, 20, 22, 20, 22, 22, 24,
        20, 22, 19, 20, 20, 20, 20, 22, 21, 21, 21, 18, 20, 19, 20, 22, 20, 20, 21, 22, 22, 20, 18, 20,
        20, 22, 18, 20, 20, 20, 21, 22, 20, 20, 22, 22, 21, 22, 19, 22, 20, 20, 20, 20, 20, 20, 20, 20,
};
static const unsigned char k_stone_hill_cap[] = {
        22, 20, 22, 22, 22, 22, 21, 22, 20, 22, 22, 21, 20, 22, 21, 20, 22, 21, 22, 18, 22, 19, 20, 20,
        20, 24, 22, 22, 20, 22, 22, 22, 22, 21, 22, 22, 22, 22, 22, 21, 20, 22, 20, 22, 20, 21, 22, 23,
        23, 22, 22, 19, 16, 22, 22, 22, 22, 22, 21, 21, 22, 23, 19, 21, 22, 21, 22, 21, 20, 22, 21, 20,
        20, 19, 21, 22, 21, 22, 22, 21, 22, 22, 22, 23, 21, 18, 20, 21, 21, 20, 22, 22, 22, 22, 22, 21,
        22, 23, 22, 24, 22, 22, 22, 22, 22, 22, 23, 22, 22, 22, 21, 20, 20, 22, 22, 22, 22, 20, 19, 21,
        21, 22, 22, 21, 20, 22, 22, 22, 24, 22, 22, 22, 20, 22, 21, 22, 22, 22, 23, 21, 20, 20, 22, 20,
        20, 22, 22, 22, 20, 20, 20, 16, 20, 21, 22, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22, 21, 22, 21,
        21, 20, 22, 20, 20, 20, 20, 20, 21, 22, 21, 21, 20, 21, 20, 18, 18, 22, 22, 22, 22, 22, 24, 21,
        22, 19, 22, 20, 22, 22, 22, 22, 22, 21, 20, 20, 19, 20, 21, 21, 22, 22, 22, 22, 20, 19, 21, 21,
        21, 19, 22, 21, 20, 22, 22, 20, 22, 22, 22, 22, 22, 20, 22, 20, 20, 22, 21, 20, 22, 22, 22, 22,
};
static const unsigned char k_hurricos_sum[] = {
        24, 26, 24, 24, 18, 19, 18, 20, 17, 20, 19, 21, 17, 19, 19, 21, 23, 26, 23, 27, 24, 27, 24, 27,
        24, 28, 23, 25, 22, 25, 23, 25, 22, 25, 22, 25, 23, 27, 23, 26, 24, 26, 24, 25, 24, 27, 25, 28,
        24, 25, 24, 26, 25, 29, 25, 26, 23, 26, 22, 24, 22, 25, 23, 25, 23, 27, 23, 26, 22, 24, 22, 24,
        22, 23, 20, 24, 20, 26, 22, 24, 20, 23, 22, 24, 22, 23, 20, 23, 21, 26, 22, 24, 22, 25, 22, 25,
        22, 25, 23, 25, 22, 27, 21, 25, 22, 25, 24, 26, 24, 27, 24, 27, 24, 27, 23, 27, 22, 25, 24, 27,
        25, 28, 24, 27, 24, 27, 23, 26, 23, 26, 24, 26, 23, 26, 24, 27, 22, 25, 22, 25, 24, 26, 23, 26,
        24, 25, 23, 26, 24, 26, 23, 26, 23, 26, 24, 27, 22, 26, 23, 28, 24, 28, 25, 27, 19, 20, 19, 20,
        20, 20, 19, 21, 19, 20, 20, 22, 23, 27, 23, 25, 23, 26, 24, 27, 25, 25, 23, 27, 22, 26, 22, 26,
        23, 26, 23, 25, 24, 26, 23, 25, 22, 27, 24, 25, 22, 25, 23, 25, 23, 26, 23, 27, 24, 26, 22, 24,
        22, 24, 22, 24, 22, 24, 22, 25, 23, 26, 23, 26, 23, 25, 22, 25, 21, 24, 23, 26, 23, 26, 23, 25,
};
static const unsigned char k_hurricos_win[] = {
        23, 24, 22, 23, 17, 18, 16, 18, 16, 18, 18, 19, 16, 18, 17, 19, 22, 24, 22, 25, 23, 25, 23, 25,
        23, 26, 22, 23, 21, 23, 22, 24, 21, 24, 21, 23, 21, 25, 22, 25, 23, 25, 23, 24, 23, 26, 24, 27,
        22, 23, 23, 24, 24, 27, 23, 25, 22, 25, 20, 23, 21, 24, 22, 23, 22, 25, 22, 24, 21, 23, 20, 22,
        20, 22, 18, 23, 19, 23, 21, 23, 19, 21, 20, 22, 20, 21, 18, 22, 20, 24, 20, 22, 21, 23, 21, 24,
        20, 23, 21, 23, 21, 26, 20, 23, 20, 23, 22, 25, 22, 25, 23, 26, 23, 26, 22, 25, 20, 23, 22, 25,
        23, 26, 22, 25, 23, 26, 21, 25, 22, 24, 22, 25, 22, 25, 22, 25, 21, 24, 21, 23, 22, 24, 22, 24,
        22, 24, 22, 25, 22, 25, 22, 25, 22, 25, 22, 25, 20, 24, 21, 26, 23, 26, 24, 25, 17, 18, 18, 19,
        18, 19, 18, 19, 18, 19, 19, 21, 22, 25, 22, 23, 22, 24, 23, 25, 23, 23, 22, 25, 21, 25, 21, 24,
        22, 25, 22, 24, 22, 24, 22, 24, 20, 25, 22, 24, 21, 24, 21, 24, 22, 24, 22, 25, 22, 25, 21, 22,
        21, 23, 21, 22, 21, 23, 21, 24, 21, 25, 22, 24, 22, 23, 21, 23, 20, 22, 22, 24, 21, 24, 22, 24,
};
static const unsigned char k_hurricos_cap[] = {
        25, 23, 24, 17, 19, 18, 19, 17, 19, 18, 20, 17, 19, 18, 21, 23, 25, 22, 27, 23, 26, 24, 24, 24,
        27, 22, 25, 21, 25, 22, 25, 21, 25, 22, 24, 23, 26, 22, 27, 24, 25, 23, 25, 23, 27, 25, 27, 23,
        24, 24, 25, 24, 28, 24, 25, 22, 25, 21, 24, 21, 25, 22, 25, 22, 25, 23, 25, 21, 23, 21, 23, 21,
        23, 20, 23, 20, 25, 21, 23, 20, 22, 21, 23, 21, 23, 20, 22, 20, 25, 20, 23, 22, 25, 21, 25, 21,
        25, 22, 24, 21, 26, 22, 25, 22, 25, 23, 27, 23, 27, 24, 25, 24, 25, 23, 27, 21, 25, 24, 27, 24,
        27, 22, 25, 24, 26, 22, 27, 22, 25, 23, 25, 22, 27, 23, 26, 22, 24, 22, 25, 23, 25, 22, 25, 23,
        25, 22, 25, 22, 25, 23, 26, 22, 27, 23, 27, 21, 27, 22, 27, 24, 27, 24, 27, 17, 19, 18, 21, 19,
        20, 18, 20, 18, 20, 19, 21, 23, 26, 22, 25, 22, 24, 24, 27, 24, 25, 22, 25, 21, 25, 22, 25, 22,
        26, 22, 25, 23, 25, 22, 24, 22, 27, 23, 25, 22, 25, 22, 25, 23, 25, 22, 25, 24, 25, 22, 23, 22,
        24, 21, 23, 21, 25, 21, 24, 21, 25, 22, 25, 22, 25, 22, 25, 20, 23, 22, 24, 22, 24, 22, 25, 22,
};

/* Count rows: level, the guard-window price as planned, the sum price, the guard replay's capacity. */
static const struct { const char *level; const unsigned char *win, *sum, *cap; } k_count[] = {
    { "s1 stone_hill", k_stone_hill_win, k_stone_hill_sum, k_stone_hill_cap },
    { "s2 hurricos",   k_hurricos_win,   k_hurricos_sum,   k_hurricos_cap },
};
#define COUNT_SPANS 240

#define COUNT_RUNS 16
#define COUNT_SPARE 2
#define DRAIN_RUNS 32

/** @brief Step the count controller for a span that produced its plan with spare scenes of room.
 * @param c The controller.
 * @param afford The span's afford.
 * @param spare The room left, in scenes. */
static void count_clean(op_eager_count_t *c, int afford, int spare)
{
    op_eager_count_step(c, OP_EAGER_CUT_NONE, afford, spare, 0, 0, COUNT_RUNS, COUNT_SPARE, DRAIN_RUNS);
}

/** @brief Step the count controller for a span cut by cause.
 * @param c The controller.
 * @param cause The cut's cause.
 * @param afford The span's afford.
 * @param made The scenes the span produced.
 * @param len The span's length in nanoseconds. */
static void count_cut(op_eager_count_t *c, op_eager_cut_t cause, int afford, int made, int64_t len)
{
    op_eager_count_step(c, cause, afford, 0, made, len, COUNT_RUNS, COUNT_SPARE, DRAIN_RUNS);
}

/** @brief Check the count controller's transitions by cause, its afford-relative plan, its deficit bound and its drain floor. */
static void test_count_steps(void)
{
    op_eager_count_t c;
    memset(&c, 0, sizeof c);
    check(op_eager_count_plan(&c, 20) == 20, "count: plans afford with no deficit");
    count_cut(&c, OP_EAGER_CUT_DEADLINE, 20, 0, 0);
    check(op_eager_count_plan(&c, 20) == 19, "count: a deadline cut plans one fewer");
    check(op_eager_count_plan(&c, 200) == 190, "count: the deficit scales with afford");
    for (int i = 0; i < 15; i++) count_clean(&c, 20, 2);
    check(op_eager_count_plan(&c, 20) == 19, "count: fifteen clean spans give nothing back");
    count_clean(&c, 20, 2);
    check(op_eager_count_plan(&c, 20) == 20, "count: the sixteenth gives one back");
    count_cut(&c, OP_EAGER_CUT_DEADLINE, 20, 0, 0);
    for (int i = 0; i < 40; i++) count_clean(&c, 20, 1);
    check(op_eager_count_plan(&c, 20) == 19, "count: spans finishing at the guard's edge hold the count");
    for (int a = 2; a <= OP_EAGER_LADDER_MAX_AFFORD; a++) {
        op_eager_count_t d;
        memset(&d, 0, sizeof d);
        for (int k = 1; k <= a / 2; k++) {
            count_cut(&d, OP_EAGER_CUT_DEADLINE, a, 0, 0);
            if (op_eager_count_plan(&d, a) == a - k) continue;
            char what[80];
            snprintf(what, sizeof what, "count: %d deadline cuts at afford %d plan %d", k, a, op_eager_count_plan(&d, a));
            check(0, what);
        }
    }
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 100; i++) count_cut(&c, OP_EAGER_CUT_DEADLINE, 3, 0, 0);
    check(op_eager_count_plan(&c, 20) == 10, "count: the deficit stops at half of afford");
    check(op_eager_count_plan(&c, 3) == 2 && op_eager_count_plan(&c, 2) == 1, "count: half of afford rounds to nearest, at least 1");
    memset(&c, 0, sizeof c);
    count_cut(&c, OP_EAGER_CUT_RING, 128, 46, 20 * MS);
    count_cut(&c, OP_EAGER_CUT_WITHHELD, 128, 1, 20 * MS);
    count_cut(&c, OP_EAGER_CUT_LATE, 128, 1, 20 * MS);
    check(c.deficit == 0, "count: ring, withheld and late cuts leave the deficit alone");
    check(op_eager_count_floor(&c) == 20 * MS / 46, "count: a ring cut floors the spacing at the span over the scenes it made");
    for (int i = 0; i < DRAIN_RUNS - 1; i++) count_clean(&c, 128, 0);
    check(op_eager_count_floor(&c) == 20 * MS / 46, "count: the drain floor holds through its run");
    count_clean(&c, 128, 0);
    check(op_eager_count_floor(&c) == 0, "count: the drain floor lapses after its run");
}

/** @brief The previous count controller: an absolute shortfall below afford, raised by every cut not after a stall.
 *
 * The negative control of the transition rows. */
typedef struct { int short_by, clean; } old_count_t;

/** @brief The previous controller's plan: afford less the shortfall, at least 1.
 * @param c The controller.
 * @param afford The scenes afforded.
 * @return The scene count. */
static int old_count_plan(const old_count_t *c, int afford)
{
    const int n = afford - c->short_by;
    return n < 1 ? 1 : n;
}

/** @brief The previous controller's step: a cut adds one, COUNT_RUNS clean spans with COUNT_SPARE room remove one.
 * @param c The controller.
 * @param cut Nonzero for a cut span, whatever its cause.
 * @param spare The room left, in scenes. */
static void old_count_step(old_count_t *c, int cut, int spare)
{
    if (cut) { if (c->short_by < OP_EAGER_LADDER_MAX_AFFORD) c->short_by++; c->clean = 0; return; }
    if (spare < COUNT_SPARE) { c->clean = 0; return; }
    if (++c->clean < COUNT_RUNS) return;
    c->clean = 0;
    if (c->short_by > 0) c->short_by--;
}

/** @brief Replay the count rows: the guard-window price as planned cuts 10% or more, the sum price under the count controller 4% or less at 18 or more scenes per span.
 * @param print Nonzero to print the table. */
static void test_count(int print)
{
    if (print) printf("level          window(cut%% scenes)  count(cut%% scenes)\n");
    for (size_t i = 0; i < sizeof k_count / sizeof k_count[0]; i++) {
        int cut_w = 0, cut_c = 0, made_w = 0, made_c = 0;
        op_eager_count_t c;
        memset(&c, 0, sizeof c);
        for (int q = 0; q < COUNT_SPANS; q++) {
            const int cap = k_count[i].cap[q];
            const int w = k_count[i].win[q];
            cut_w += w > cap; made_w += w < cap ? w : cap;
            const int plan = op_eager_count_plan(&c, k_count[i].sum[q]);
            cut_c += plan > cap; made_c += plan < cap ? plan : cap;
            if (plan > cap) count_cut(&c, OP_EAGER_CUT_DEADLINE, k_count[i].sum[q], cap, 0);
            else count_clean(&c, k_count[i].sum[q], cap - plan);
        }
        if (print) printf("%-14s %6.1f %6.2f          %6.1f %6.2f\n", k_count[i].level,
                          100.0 * cut_w / COUNT_SPANS, (double)made_w / COUNT_SPANS,
                          100.0 * cut_c / COUNT_SPANS, (double)made_c / COUNT_SPANS);
        char what[112];
        snprintf(what, sizeof what, "count %s: cut on %d of %d spans, at most 4%%", k_count[i].level, cut_c, COUNT_SPANS);
        check(cut_c * 25 <= COUNT_SPANS, what);
        snprintf(what, sizeof what, "count %s: %d scenes over %d spans, at least 18 per span", k_count[i].level, made_c, COUNT_SPANS);
        check(made_c >= 18 * COUNT_SPANS, what);
        snprintf(what, sizeof what, "control %s: the guard-window price cut on %d of %d spans", k_count[i].level, cut_w, COUNT_SPANS);
        check(cut_w * 10 >= COUNT_SPANS, what);
    }
}

/* Transition rows: one-VBL spans of the Spyro 1 attract demo through the
 * level 11 load (VBLs 3111-3177), afford and the scenes each produced; every
 * plan above that count was cut by a full scene ring. */
static const unsigned char k_trans_afford[] = {
    128, 128, 128, 128, 128, 128, 128, 122, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
    128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
};
static const unsigned char k_trans_made[] = {
    55, 46, 46, 46, 46, 46, 46, 48, 46, 46, 46, 46, 46, 46, 46, 46, 46,
    46, 46, 46, 49, 46, 46, 46, 52, 46, 46, 46, 46, 51, 46, 48, 53, 55,
    54, 49, 49, 51, 59, 53, 49, 49, 51, 54, 53, 48, 51, 48, 53, 50, 53,
    49, 52, 53, 49, 48, 52, 54, 49, 47, 51, 52, 52, 51, 51, 50, 50,
};
#define TRANS_SPANS ((int)sizeof k_trans_made)
/* The span lengths (one VBL plus the 3.26 ms lag; one tick plus it) and the
 * gameplay spans that follow: afford 12, of which 10 fit. */
#define TRANS_VBL_LEN (19940 * 1000LL)
#define TRANS_TICK_LEN (36690 * 1000LL)
#define TRANS_AFFORD 12
#define TRANS_CAP 10
#define TRANS_LEVEL_SPANS 120

/** @brief Plan one free span from the controller: its count at afford, at most one per drain floor over len.
 * @param c The controller.
 * @param afford The scenes afforded.
 * @param len The span's length in nanoseconds.
 * @return The scenes planned. */
static int count_span(const op_eager_count_t *c, int afford, int64_t len)
{
    int n = 0;
    op_eager_ladder_fill(len, op_eager_count_plan(c, afford), op_eager_count_floor(c), 1, &n);
    return n;
}

/** @brief Replay the transition rows into gameplay: the controller plans the level within one scene of its steady count at once and never one scene; the previous controller strands at one.
 *
 * The present thread's drain is the recorded scene count per transition
 * span (a plan above it is a ring cut); the gameplay spans cut at the
 * deadline above TRANS_CAP and leave cap - plan scenes of room. The steady
 * count there is TRANS_CAP: the controller holds a plan with less than
 * COUNT_SPARE room. Under the previous controller every cut raised the
 * absolute shortfall, which leaves afford 12 planning 1.
 * @param print Nonzero to print the per-span plans. */
static void test_count_transition(int print)
{
    op_eager_count_t c;
    old_count_t o = { 0, 0 };
    memset(&c, 0, sizeof c);
    int ring = 0, ring_old = 0, made = 0, made_old = 0;
    for (int q = 0; q < TRANS_SPANS; q++) {
        const int a = k_trans_afford[q], cap = k_trans_made[q];
        const int n = count_span(&c, a, TRANS_VBL_LEN), no = old_count_plan(&o, a);
        ring += n > cap; ring_old += no > cap; made += n < cap ? n : cap; made_old += no < cap ? no : cap;
        if (n > cap) count_cut(&c, OP_EAGER_CUT_RING, a, cap, TRANS_VBL_LEN);
        else count_clean(&c, a, OP_EAGER_LADDER_MAX_AFFORD);   /* the walk ends before the update VBL */
        old_count_step(&o, no > cap, OP_EAGER_LADDER_MAX_AFFORD);
    }
    char what[128];
    snprintf(what, sizeof what, "transition: %d of %d spans ring-cut against the previous controller's %d, at most 10%%",
             ring, TRANS_SPANS, ring_old);
    check(ring * 10 <= TRANS_SPANS && ring_old * 2 >= TRANS_SPANS, what);
    snprintf(what, sizeof what, "transition: %d scenes made against the previous controller's %d, at least 90%%", made, made_old);
    check(made * 10 >= made_old * 9, what);
    if (print) printf("transition: ring cuts %d (previous %d), scenes %d (previous %d)\n", ring, ring_old, made, made_old);
    int first_near = -1, ones = 0, old_ones = 0, cuts = 0;
    for (int q = 0; q < TRANS_LEVEL_SPANS; q++) {
        const int n = count_span(&c, TRANS_AFFORD, TRANS_TICK_LEN), no = old_count_plan(&o, TRANS_AFFORD);
        if (print) printf("level span %3d: plan %2d (previous controller %2d)\n", q, n, no);
        if (first_near < 0 && n >= TRANS_CAP - 1 && n <= TRANS_CAP) first_near = q;
        ones += n == 1; old_ones += no == 1; cuts += n > TRANS_CAP;
        if (n > TRANS_CAP) count_cut(&c, OP_EAGER_CUT_DEADLINE, TRANS_AFFORD, TRANS_CAP, TRANS_TICK_LEN);
        else count_clean(&c, TRANS_AFFORD, TRANS_CAP - n);
        old_count_step(&o, no > TRANS_CAP, TRANS_CAP - no);
    }
    snprintf(what, sizeof what, "transition: the level plans within one of %d from span %d (bound 2)", TRANS_CAP, first_near);
    check(first_near >= 0 && first_near <= 2, what);
    snprintf(what, sizeof what, "transition: %d level span(s) plan one scene at afford %d", ones, TRANS_AFFORD);
    check(ones == 0, what);
    snprintf(what, sizeof what, "transition: %d deadline cut(s) in the level, at most the two to the edge", cuts);
    check(cuts <= 2, what);
    snprintf(what, sizeof what, "control: the previous controller plans one scene on %d of %d level spans", old_ones, TRANS_LEVEL_SPANS);
    check(old_ones == TRANS_LEVEL_SPANS, what);
}

/** @brief Run the controller from a deficit against a fixed afford and capacity, and return the first span planning within [lo, hi].
 *
 * A span plans count_span over a tick span; it is cut at the deadline when
 * it plans more than cap and otherwise leaves cap - plan scenes of room.
 * @param afford The scenes afforded.
 * @param cap The scenes that fit.
 * @param deficit The controller's starting deficit.
 * @param lo The lowest steady plan.
 * @param hi The highest steady plan.
 * @param spans The spans to run.
 * @param left Receives 1 when a span after the first in range plans outside it.
 * @return The first span in range, or -1. */
static int count_settle(int afford, int cap, int deficit, int lo, int hi, int spans, int *left)
{
    op_eager_count_t c;
    memset(&c, 0, sizeof c);
    c.deficit = deficit;
    int reached = -1;
    *left = 0;
    for (int q = 0; q < spans; q++) {
        const int n = count_span(&c, afford, TRANS_TICK_LEN);
        const int in = n >= lo && n <= hi;
        if (in && reached < 0) reached = q;
        *left |= !in && reached >= 0;
        if (n > cap) count_cut(&c, OP_EAGER_CUT_DEADLINE, afford, cap, TRANS_TICK_LEN);
        else count_clean(&c, afford, cap - n);
    }
    return reached;
}

/** @brief Check the recovery bound: from either extreme deficit, every afford and capacity reaches the steady count within its bound and stays.
 *
 * The steady count is the plan the controller holds: cap or cap - 1 (less
 * than COUNT_SPARE room), or the deficit's floor round(afford / 2) when cap
 * lies below it. From a zero deficit the plan falls one scene per span; from
 * the largest deficit it rises one scene per COUNT_RUNS spans with room. The
 * bound is COUNT_RUNS * ceil(afford / 2) + afford spans. */
static void test_count_bound(void)
{
    int worst = 0, fails = 0;
    for (int a = 1; a <= OP_EAGER_LADDER_MAX_AFFORD; a++) {
        const int floor_n = (a + 1) / 2, bound = COUNT_RUNS * ((a + 1) / 2) + a;
        for (int cap = 1; cap <= a; cap++) {
            const int lo = cap - 1 > floor_n ? cap - 1 : floor_n, hi = cap > floor_n ? cap : floor_n;
            int left_z, left_f;
            const int rz = count_settle(a, cap, 0, lo, hi, bound + 4 * COUNT_RUNS, &left_z);
            const int rf = count_settle(a, cap, OP_EAGER_COUNT_MAX_DEFICIT, lo, hi, bound + 4 * COUNT_RUNS, &left_f);
            const int r = rz > rf ? rz : rf;
            worst = r > worst ? r : worst;
            const int bad = rz < 0 || rf < 0 || r > bound || left_z || left_f;
            fails += bad;
            if (bad && fails <= 4) {
                char what[112];
                snprintf(what, sizeof what, "bound: afford %d cap %d reached %d / %d (bound %d), left %d / %d",
                         a, cap, rz, rf, bound, left_z, left_f);
                check(0, what);
            }
        }
    }
    char what[80];
    snprintf(what, sizeof what, "bound: %d case(s) outside, worst %d spans", fails, worst);
    check(fails == 0, what);
}

/* Vsync rows: level, budget, cost mean and deviation (ns), slots, the guard-window price's afford. */
static const struct { const char *level; int64_t budget, ema, dev; int slots, win; } k_vsync[] = {
    { "s1 artisans", 29467016, 1617053, 147642, 17, 16 }, /* 47 such spans; 6 below the slots under both prices */
    { "s1 stone_hill", 29610348, 1690336, 134932, 16, 15 }, /* 1 such spans; 0 below the slots under both prices */
    { "s1 town_square", 29995630, 1896535, 171202, 14, 13 }, /* 17 such spans; 0 below the slots under both prices */
    { "s2 autumn_plains", 28813499, 1745028, 57675, 16, 14 }, /* 356 such spans; 812 below the slots under both prices */
    { "s2 glimmer", 29122517, 1654951, 133751, 16, 15 }, /* 43 such spans; 0 below the slots under both prices */
    { "s2 hurricos", 27811836, 1458175, 146656, 17, 16 }, /* 1 such spans; 0 below the slots under both prices */
    { "s2 summer_forest", 29049638, 1238888, 95358, 16, 15 }, /* 324 such spans; 0 below the slots under both prices */
    { "s3 cloud_spires", 28817531, 1707839, 96489, 16, 14 }, /* 435 such spans; 626 below the slots under both prices */
    { "s3 sunny_villa", 28414182, 1946463, 144050, 14, 12 }, /* 134 such spans; 1062 below the slots under both prices */
    { "s3 sunrise_spring", 27709429, 1572376, 118516, 16, 15 }, /* 659 such spans; 515 below the slots under both prices */
};

/** @brief Check the vsync rows: the sum price keeps every span the guard-window price put below its slot count at need 1. */
static void test_vsync(void)
{
    for (size_t i = 0; i < sizeof k_vsync / sizeof k_vsync[0]; i++) {
        const int a = op_eager_ladder_afford(k_vsync[i].budget, k_vsync[i].ema, k_vsync[i].dev);
        op_eager_ladder_t s = fresh();
        char what[112];
        snprintf(what, sizeof what, "vsync %s: afford %d keeps %d slots at need 1", k_vsync[i].level, a, k_vsync[i].slots);
        check(op_eager_ladder_step(&s, k_vsync[i].slots, a, 0, WINDOW) == 1, what);
        snprintf(what, sizeof what, "control %s: the guard-window price %d was below %d slots", k_vsync[i].level, k_vsync[i].win, k_vsync[i].slots);
        check(k_vsync[i].win < k_vsync[i].slots, what);
    }
}

/* The stall rows: 110 consecutive spans of a windowed Xvfb run (--vsync 1,
 * --render-fps 480, the spyro-3 icy-peak bk2, llvmpipe) with 25 ms spent in
 * every intermediate scene from VBL 2200 to 2320
 * (OPENPETE_PACE_INJECT_PRESENT_US, OPENPETE_PACE_INJECT_PRESENT_VBL): per
 * span its VBL, slots, afford, cut as the ladder receives it (the record's
 * cut flag unless the span before it was late) and the level the run logged.
 * Replaying them through op_eager_ladder_step reproduces every logged level.
 * The controls step the same rows through a ladder that falls one level per
 * headroom run and one that never falls; they are open loop (the afford and
 * cut inputs are the ones the real ladder's levels produced). */
static const struct { int f, ns, afford, cut, level; } k_stall[] = {
    {2161, 2, 128, 0, 1}, {2163, 2, 128, 0, 1}, {2165, 2, 128, 0, 1}, {2167, 3, 128, 0, 1}, {2169, 2, 128, 0, 1}, {2171, 2, 128, 0, 1},
    {2173, 3, 128, 0, 1}, {2175, 2, 128, 0, 1}, {2177, 2, 128, 0, 1}, {2179, 2, 128, 0, 1}, {2181, 3, 128, 0, 1}, {2183, 2, 128, 0, 1},
    {2185, 2, 128, 0, 1}, {2187, 2, 128, 0, 1}, {2189, 2, 128, 0, 1}, {2191, 2, 128, 0, 1}, {2193, 2, 128, 0, 1}, {2195, 2, 128, 0, 1},
    {2197, 2, 128, 0, 1}, {2199, 2, 128, 0, 1}, {2201, 2, 128, 0, 1}, {2203, 2,   9, 0, 1}, {2205, 2,   5, 0, 1}, {2207, 2,   3, 0, 1},
    {2209, 2,   2, 0, 1}, {2211, 2,   2, 0, 1}, {2213, 2,   1, 0, 2}, {2215, 2,   2, 0, 1}, {2217, 2,   2, 1, 2}, {2219, 2,   2, 0, 2},
    {2221, 2,   2, 0, 2}, {2223, 2,   3, 0, 2}, {2225, 2,   3, 0, 2}, {2227, 3,   3, 0, 2}, {2229, 1,   1, 0, 2}, {2231, 2,   2, 0, 2},
    {2233, 3,   3, 0, 1}, {2235, 1,   3, 1, 1}, {2237, 3,   3, 0, 1}, {2239, 1,   1, 0, 1}, {2241, 2,   2, 0, 1}, {2243, 2,   3, 1, 2},
    {2245, 2,   3, 0, 2}, {2247, 2,   3, 0, 2}, {2249, 3,   3, 0, 2}, {2251, 2,   4, 1, 3}, {2253, 3,   4, 0, 3}, {2255, 3,   4, 0, 3},
    {2257, 3,   5, 0, 3}, {2259, 3,   5, 0, 3}, {2261, 3,   6, 0, 3}, {2263, 3,   6, 0, 3}, {2265, 3,   7, 0, 3}, {2267, 3,   7, 0, 1},
    {2269, 2,   3, 1, 1}, {2271, 2,   4, 1, 2}, {2273, 3,   4, 0, 2}, {2275, 1,   1, 0, 2}, {2277, 2,   3, 0, 2}, {2279, 2,   3, 0, 2},
    {2281, 2,   3, 0, 2}, {2283, 2,   3, 0, 2}, {2285, 3,   4, 0, 2}, {2287, 2,   4, 1, 3}, {2289, 3,   4, 0, 3}, {2291, 3,   5, 0, 3},
    {2293, 3,   5, 0, 3}, {2295, 3,   6, 0, 3}, {2297, 3,   6, 0, 3}, {2299, 3,   7, 0, 3}, {2301, 3,   7, 0, 3}, {2303, 3,   8, 0, 1},
    {2305, 2,   3, 1, 1}, {2307, 2,   4, 1, 2}, {2309, 3,   4, 0, 2}, {2311, 1,   1, 0, 2}, {2313, 2,   3, 0, 2}, {2315, 2,   3, 0, 2},
    {2317, 2,   3, 0, 2}, {2319, 2,   3, 0, 2}, {2321, 3,   4, 0, 2}, {2323, 2,   4, 1, 3}, {2325, 3,   4, 0, 3}, {2327, 3,   5, 0, 3},
    {2329, 3,   5, 0, 3}, {2331, 2,   5, 0, 3}, {2333, 3,   6, 0, 3}, {2335, 3,   7, 0, 3}, {2337, 2,   7, 0, 3}, {2339, 2,   8, 0, 1},
    {2341, 3,   9, 0, 1}, {2343, 2,  12, 0, 1}, {2345, 2,  14, 0, 1}, {2347, 2,  16, 0, 1}, {2349, 2,  19, 0, 1}, {2351, 2,  21, 0, 1},
    {2353, 2,  24, 0, 1}, {2355, 2,  27, 0, 1}, {2357, 2,  30, 0, 1}, {2359, 2,  34, 0, 1}, {2361, 2,  37, 0, 1}, {2363, 2,  40, 0, 1},
    {2365, 2,  43, 0, 1}, {2367, 2,  48, 0, 1}, {2369, 2,  49, 0, 1}, {2371, 2,  54, 0, 1}, {2373, 2,  58, 0, 1}, {2375, 2,  62, 0, 1},
    {2377, 2,  63, 0, 1}, {2379, 2,  68, 0, 1},
};

/* The stall row's window end, and the spans the level has to settle in after
 * it: the cut the last stale estimate reports, then one headroom run. */
#define STALL_END 2320
#define STALL_MAX_SPANS 10
#define STALL_SETTLE 16

/** @brief Step a ladder variant: the current one (fall 0), one that falls one level per headroom run (1), or one that never falls (2).
 * @param s The ladder state.
 * @param ns The span's slot count.
 * @param afford The scenes afforded.
 * @param cut Nonzero after a cut span.
 * @param fall The variant.
 * @return The level the span plans at. */
static int stall_step(op_eager_ladder_t *s, int ns, int afford, int cut, int fall)
{
    if (fall == 0) return op_eager_ladder_step(s, ns, afford, cut, WINDOW);
    const int before = s->level;
    op_eager_ladder_t t = *s;
    const int lvl = op_eager_ladder_step(&t, ns, afford, cut, WINDOW);
    if (t.level < before) {   /* the current ladder fell: the variant falls one level, or holds */
        t.level = fall == 1 ? before - 1 : before;
        *s = t;
        return lvl < t.level ? t.level : lvl;
    }
    *s = t;
    return lvl;
}

/** @brief Replay the stall rows through a ladder variant and return the spans from STALL_END until the level settles at 1.
 * @param fall The variant (see stall_step).
 * @param match Receives the rows whose replayed level equals the logged one.
 * @param raised Receives the spans from VBL 2200 on planned above level 1.
 * @return The spans to settle, or -1 when the rows end first. */
static int stall_replay(int fall, int *match, int *raised)
{
    enum { N = sizeof k_stall / sizeof k_stall[0] };
    int lvl[N];
    op_eager_ladder_t s = fresh();
    *match = *raised = 0;
    for (int i = 0; i < N; i++) {
        lvl[i] = stall_step(&s, k_stall[i].ns, k_stall[i].afford, k_stall[i].cut, fall);
        *match += lvl[i] == k_stall[i].level;
        *raised += k_stall[i].f >= 2200 && lvl[i] > 1;
    }
    int end = 0;
    while (end < N && k_stall[end].f < STALL_END) end++;
    for (int i = end; i + STALL_SETTLE <= N; i++) {
        int ok = 1;
        for (int q = i; q < i + STALL_SETTLE; q++) ok &= lvl[q] <= 1;
        if (ok) return i - end;
    }
    return -1;
}

/** @brief Check the stall rows: the replay reproduces the logged levels, rises, and settles within STALL_MAX_SPANS; the controls do not. */
static void test_stall(void)
{
    enum { N = sizeof k_stall / sizeof k_stall[0] };
    int match, raised;
    char what[112];
    const int rec = stall_replay(0, &match, &raised);
    snprintf(what, sizeof what, "stall: replay matches %d of %d logged levels", match, (int)N);
    check(match == N, what);
    snprintf(what, sizeof what, "stall: %d span(s) raised during the stall", raised);
    check(raised > 0, what);
    snprintf(what, sizeof what, "stall: settles %d span(s) after the stall (bound %d)", rec, STALL_MAX_SPANS);
    check(rec >= 0 && rec <= STALL_MAX_SPANS, what);
    const int one = stall_replay(1, &match, &raised);
    snprintf(what, sizeof what, "control: a one-level fall settles in %d span(s), past the bound", one);
    check(one < 0 || one > STALL_MAX_SPANS, what);
    const int never = stall_replay(2, &match, &raised);
    check(never < 0, "control: a ladder that never falls does not settle");
}

/* Cost-mean rows: a 2 ms steady cost, one 2 s stall, and a sustained rise to
 * 20 ms. Bounds in samples. */
#define COST_STEADY_NS   2000000
#define COST_STALL_NS    2000000000
#define COST_RISE_NS     20000000
#define COST_STALL_MAX   12   /* samples after the stall until the mean is within 10% of the steady cost */
#define COST_RISE_MAX    20   /* samples after the rise until the mean is within 10% of the new cost */

/** @brief The previous update rule: every sample enters the 8-sample mean whole. */
static int64_t old_cost_ema(int64_t ema, int64_t sample)
{
    return ema ? (ema * 7 + sample) / 8 : sample;
}

/** @brief Feed a steady cost, then one stall, then the steady cost again; return the samples after the stall until the mean is within 10% of the steady cost.
 * @param rule The update rule.
 * @param peak Receives the mean right after the stall.
 * @param print Print the mean after each sample.
 * @return The sample count, or -1 when the mean does not return within 200 samples. */
static int cost_stall_recovery(int64_t (*rule)(int64_t, int64_t), int64_t *peak, int print)
{
    int64_t ema = 0;
    for (int i = 0; i < 32; i++) ema = rule(ema, COST_STEADY_NS);
    ema = rule(ema, COST_STALL_NS);
    *peak = ema;
    if (print) printf("  stall: %.3f", (double)ema / 1e6);
    for (int n = 1; n <= 200; n++) {
        ema = rule(ema, COST_STEADY_NS);
        if (print && n <= 40) printf(" %.3f", (double)ema / 1e6);
        if (ema <= COST_STEADY_NS + COST_STEADY_NS / 10) {
            if (print) printf("  (n=%d)\n", n);
            return n;
        }
    }
    if (print) printf("\n");
    return -1;
}

/** @brief Feed a steady cost, then a sustained higher one; return the samples until the mean is within 10% of the new cost.
 * @param rule The update rule.
 * @param print Print the mean after each sample.
 * @return The sample count, or -1 when the mean does not converge within 200 samples. */
static int cost_rise_convergence(int64_t (*rule)(int64_t, int64_t), int print)
{
    int64_t ema = 0;
    for (int i = 0; i < 32; i++) ema = rule(ema, COST_STEADY_NS);
    if (print) printf("  rise:");
    for (int n = 1; n <= 200; n++) {
        ema = rule(ema, COST_RISE_NS);
        if (print) printf(" %.3f", (double)ema / 1e6);
        if (ema >= COST_RISE_NS - COST_RISE_NS / 10) {
            if (print) printf("  (n=%d)\n", n);
            return n;
        }
    }
    if (print) printf("\n");
    return -1;
}

/** @brief Check the bounded cost mean: one stall moves it by at most (cap - 1) / 8 and it returns within COST_STALL_MAX samples, a sustained rise converges within COST_RISE_MAX; the unbounded rule is the control.
 * @param print Print the mean sequences. */
static void test_cost_ema(int print)
{
    char what[160];
    int64_t peak = 0, old_peak = 0;
    const int rec = cost_stall_recovery(op_eager_cost_ema, &peak, print);
    const int old_rec = cost_stall_recovery(old_cost_ema, &old_peak, print);
    snprintf(what, sizeof what, "cost: one stall peaks the mean at %.3f ms, at most %.3f",
             (double)peak / 1e6, (double)(COST_STEADY_NS * (7 + OP_EAGER_COST_SAMPLE_CAP) / 8) / 1e6);
    check(peak <= (int64_t)COST_STEADY_NS * (7 + OP_EAGER_COST_SAMPLE_CAP) / 8, what);
    snprintf(what, sizeof what, "cost: the mean returns %d sample(s) after the stall (bound %d)", rec, COST_STALL_MAX);
    check(rec >= 0 && rec <= COST_STALL_MAX, what);
    snprintf(what, sizeof what, "control: unbounded, the stall peaks at %.3f ms and returns after %d sample(s)",
             (double)old_peak / 1e6, old_rec);
    check(old_rec < 0 || old_rec > COST_STALL_MAX, what);
    const int rise = cost_rise_convergence(op_eager_cost_ema, print);
    const int old_rise = cost_rise_convergence(old_cost_ema, print);
    snprintf(what, sizeof what, "cost: a sustained 10x rise converges in %d sample(s) (unbounded %d, bound %d)",
             rise, old_rise, COST_RISE_MAX);
    check(rise >= 0 && rise <= COST_RISE_MAX, what);
    check(op_eager_cost_ema(0, COST_STALL_NS) == COST_STALL_NS, "cost: the first sample seeds the mean whole");
}

/** @brief Return 0 when slots[0..k) is a spread of k scenes over ns slots, else a code naming the first property it breaks.
 *
 * The properties: increasing, the first slot 0, the last ns - 1 when k is
 * at least 2 and below ns, consecutive differences within one of each
 * other, and, when 2 * (k - 1) >= ns - 1, no two skipped slots adjacent.
 * @param slots The chosen slots.
 * @param k Their count.
 * @param ns The slot count.
 * @return 0, or 1 order, 2 first, 3 last, 4 uneven, 5 adjacent skips. */
static int spread_fault(const int *slots, int k, int ns)
{
    if (slots[0] != 0) return 2;
    if (k >= 2 && k < ns && slots[k - 1] != ns - 1) return 3;
    int lo = ns, hi = 0;
    for (int q = 1; q < k; q++) {
        const int d = slots[q] - slots[q - 1];
        if (d < 1) return 1;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    if (k >= 2 && hi - lo > 1) return 4;
    if (2 * (k - 1) >= ns - 1)
        for (int q = 1; q < k; q++)
            if (slots[q] - slots[q - 1] > 2) return 5;
    return 0;
}

/** @brief Fill slots with op_eager_spread_slot for k scenes of ns slots, at most ns of them.
 * @param slots Receives the slots.
 * @param k The scenes planned.
 * @param ns The slot count.
 * @return The number of slots written, min(k, ns). */
static int spread(int *slots, int k, int ns)
{
    const int n = k < ns ? k : ns;
    for (int q = 0; q < n; q++) slots[q] = op_eager_spread_slot(q, k, ns);
    return n;
}

/** @brief Return the skipped slots of a spread as a bit mask (slots below 64).
 * @param k The scenes planned.
 * @param ns The slot count, at most 64.
 * @return Bit s set for each slot s no scene takes. */
static unsigned long long skipped_mask(int k, int ns)
{
    int slots[OP_EAGER_LADDER_MAX_AFFORD];
    const int n = spread(slots, k, ns);
    unsigned long long m = ns >= 64 ? ~0ULL : (1ULL << ns) - 1;
    for (int q = 0; q < n; q++) m &= ~(1ULL << slots[q]);
    return m;
}

/** @brief Check the fill cadence's slot spread on the named cases and on every k of every ns up to the ladder's bound; the first-k placement is the control. */
static void test_spread(void)
{
    char what[160];
    /* 11 of 13: skips 3 and 9, six apart, and 9 to the next span's 3 is seven. */
    check(skipped_mask(11, 13) == ((1ULL << 3) | (1ULL << 9)), "spread: 11 of 13 skips slots 3 and 9");
    /* 13 of 15: skips 3 and 10 (q * 14 / 12, halves up: 3.5 rounds to 4, 10.5 to 11). */
    check(skipped_mask(13, 15) == ((1ULL << 3) | (1ULL << 10)), "spread: 13 of 15 skips slots 3 and 10");
    /* 16 of 17: one skip, mid-span. */
    check(skipped_mask(16, 17) == (1ULL << 8), "spread: 16 of 17 skips slot 8");
    check(skipped_mask(1, 13) == (((1ULL << 13) - 1) & ~1ULL), "spread: 1 of 13 keeps slot 0 only");
    check(skipped_mask(13, 13) == 0, "spread: 13 of 13 keeps every slot");
    check(skipped_mask(20, 13) == 0 && op_eager_spread_slot(12, 20, 13) == 12,
          "spread: k above ns keeps every slot once");
    int bad = 0, first_ns = 0, first_k = 0, code = 0;
    for (int ns = 1; ns <= OP_EAGER_LADDER_MAX_AFFORD; ns++)
        for (int k = 1; k <= ns + 2; k++) {
            int slots[OP_EAGER_LADDER_MAX_AFFORD];
            const int n = spread(slots, k, ns);
            const int f = spread_fault(slots, n, ns);
            if (f && !bad++) { first_ns = ns; first_k = k; code = f; }
        }
    snprintf(what, sizeof what, "spread: every k of every ns up to %d (%d bad; first ns %d k %d fault %d)",
             OP_EAGER_LADDER_MAX_AFFORD, bad, first_ns, first_k, code);
    check(bad == 0, what);
    /* Control: the first k slots skip the span's tail in one run. */
    int first[11];
    for (int q = 0; q < 11; q++) first[q] = q;
    check(spread_fault(first, 11, 13) != 0, "control: the first 11 of 13 slots are refused");
}

/** @brief Run a stall through a grid fill controller and return the spans from the stall's end until the plan is full again.
 *
 * ns 16 slots. Ten spans of the stall afford 2 and are cut each time; then
 * every span affords 99 with room. The free span's count step is the
 * control: the same cuts given back one scene per run.
 * @param fill Nonzero: op_eager_fill_step with window 8; zero: op_eager_count_step with window 16.
 * @return The spans until the plan is 16, or -1 within 200 spans. */
static int fill_stall_recovery(int fill)
{
    op_eager_count_t c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < 10; i++) {
        if (fill) op_eager_fill_step(&c, OP_EAGER_CUT_DEADLINE, 2, 0, WINDOW, 2);
        else op_eager_count_step(&c, OP_EAGER_CUT_DEADLINE, 2, 0, 0, 0, 16, 2, 0);
    }
    for (int span = 0; span < 200; span++) {
        const int k = op_eager_fill_count(&c, 99, 16);
        if (k == 16) return span;
        if (fill) op_eager_fill_step(&c, OP_EAGER_CUT_NONE, 16, 99, WINDOW, 2);
        else op_eager_count_step(&c, OP_EAGER_CUT_NONE, 16, 99, 0, 0, 16, 2, 0);
    }
    return -1;
}

/** @brief Check the fill count and its controller: a full plan at afford >= ns, one scene fewer per cut, no oscillation at a fixed capacity, the clean run's one-move recovery, ring cuts counted. */
static void test_fill_count(void)
{
    char what[200];
    op_eager_count_t c;
    memset(&c, 0, sizeof c);
    /* At deficit 0 the count is min(afford, ns): the ladder's level-1 plan when afford >= ns. */
    check(op_eager_fill_count(&c, 20, 13) == 13, "fill: afford 20 of 13 slots plans 13");
    check(op_eager_fill_count(&c, 13, 13) == 13, "fill: afford 13 of 13 plans 13");
    check(op_eager_fill_count(&c, 11, 13) == 11, "fill: afford 11 of 13 plans 11");
    check(op_eager_fill_count(&c, 1, 13) == 1, "fill: afford 1 plans the canonical scene");
    op_eager_ladder_t l = fresh();
    check(op_eager_ladder_step(&l, 13, 20, 0, WINDOW) == 1, "fill: the ladder plans level 1 there too");
    /* Capacity 11 of 13 slots, the estimate affording 13 every span, the guard's room
     * capacity - planned: two cuts, then 11 for good (no room, so no clean run). */
    int cuts = 0, above = 0, k = 0;
    for (int span = 0; span < 400; span++) {
        k = op_eager_fill_count(&c, 13, 13);
        const int cut = k > 11;
        cuts += cut;
        if (span >= 2 && k > 11) above++;
        op_eager_fill_step(&c, cut ? OP_EAGER_CUT_DEADLINE : OP_EAGER_CUT_NONE, 13, cut ? 0 : 11 - k, WINDOW, 2);
    }
    snprintf(what, sizeof what, "fill: capacity 11 of 13 at afford 13: %d cut(s) in 400 spans, final plan %d, %d plan(s) above", cuts, k, above);
    check(cuts == 2 && k == 11 && above == 0, what);
    /* Capacity rises to 15: room 4 at 11; the clean run gives both scenes back in one move. */
    int n13 = -1;
    for (int span = 0; span < 64; span++) {
        k = op_eager_fill_count(&c, 13, 13);
        if (k == 13 && n13 < 0) n13 = span;
        op_eager_fill_step(&c, OP_EAGER_CUT_NONE, 13, 15 - k, WINDOW, 2);
    }
    snprintf(what, sizeof what, "fill: room gives the slots back, full again after %d spans (window %d)", n13, WINDOW);
    check(n13 == WINDOW, what);
    /* Capacity 12 of 13 with an afford of 13: a cut, then 12; room 0 keeps it there. */
    memset(&c, 0, sizeof c);
    cuts = 0;
    for (int span = 0; span < 200; span++) {
        k = op_eager_fill_count(&c, 13, 13);
        cuts += k > 12;
        op_eager_fill_step(&c, k > 12 ? OP_EAGER_CUT_DEADLINE : OP_EAGER_CUT_NONE, 13, k > 12 ? 0 : 12 - k, WINDOW, 2);
    }
    snprintf(what, sizeof what, "fill: capacity 12 of 13: %d cut(s) in 200 spans, plan %d", cuts, k);
    check(cuts == 1 && k == 12, what);
}

/** @brief The fill count after a stall and under an afford that alternates: one window to recover, and the plan follows the afford. */
static void test_fill_recovery(void)
{
    char what[200];
    op_eager_count_t c;
    /* A stall's cuts at afford 2 cost one window once the spans clear; the free step is the control. */
    const int rec = fill_stall_recovery(1), old = fill_stall_recovery(0);
    snprintf(what, sizeof what, "fill: full %d spans after a 10-span stall (window %d); the free step takes %d", rec, WINDOW, old);
    check(rec == WINDOW, what);
    snprintf(what, sizeof what, "control: the free span's step gives the stall's deficit back after %d spans", old);
    check(old < 0 || old > 4 * WINDOW, what);
    /* The span's afford alternating 11 and 13 with room: the plan follows it, no cut, no deficit. */
    memset(&c, 0, sizeof c);
    int follow = 1;
    for (int span = 0; span < 64; span++) {
        const int a = span % 2 ? 11 : 13;
        follow &= op_eager_fill_count(&c, a, 13) == a;
        op_eager_fill_step(&c, OP_EAGER_CUT_NONE, a, 4, WINDOW, 2);
    }
    check(follow && c.deficit == 0, "fill: an afford alternating 11 and 13 plans 11 and 13");
}

/** @brief The fill step per cut cause: a ring cut plans one fewer, withheld and late cuts change nothing, a span without room ends the clean run. */
static void test_fill_cut_causes(void)
{
    op_eager_count_t c;
    /* A ring cut is a cut on a grid; withheld and late cuts carry no measurement. */
    memset(&c, 0, sizeof c);
    op_eager_fill_step(&c, OP_EAGER_CUT_RING, 13, 0, WINDOW, 2);
    check(op_eager_fill_count(&c, 13, 13) == 12 && c.drain_left == 0, "fill: a ring cut plans one fewer, no drain floor");
    op_eager_fill_step(&c, OP_EAGER_CUT_WITHHELD, 13, 0, WINDOW, 2);
    op_eager_fill_step(&c, OP_EAGER_CUT_LATE, 13, 0, WINDOW, 2);
    check(op_eager_fill_count(&c, 13, 13) == 12, "fill: withheld and late cuts change nothing");
    /* A span without room ends the clean run. */
    for (int i = 0; i < WINDOW - 1; i++) op_eager_fill_step(&c, OP_EAGER_CUT_NONE, 13, 2, WINDOW, 2);
    op_eager_fill_step(&c, OP_EAGER_CUT_NONE, 13, 1, WINDOW, 2);
    check(op_eager_fill_count(&c, 13, 13) == 12, "fill: a span with room 1 ends the run before the window");
    /* Control: the free span's step answers a ring cut with a drain floor and no fewer scenes. */
    memset(&c, 0, sizeof c);
    op_eager_count_step(&c, OP_EAGER_CUT_RING, 13, 0, 10, 20000000, 16, 2, 32);
    check(op_eager_fill_count(&c, 13, 13) == 13, "control: op_eager_count_step keeps 13 after a ring cut");
}

/* The re-plan rows: a scripted cost sequence on the span geometry the
 * Windows pace logs of Skelos Badlands (vsync off, uncapped) measure. The
 * plan is made at 0; the first window ends 0.5 ms before the update VBL's
 * deadline D1; the update VBL runs for RP_VBL; the second window ends 0.5
 * ms before D2. Scene costs are RP_COST times a per-span factor in
 * [0.92, 1.08] times a per-scene factor in [0.88, 1.12], from a fixed LCG.
 * The cost mean and deviation follow eager_cost_note's rules outside the
 * re-warm window. A span's capacity is the scenes the produce guard starts
 * with no plan limit on the same costs; its planned count is the count it
 * finished with. The control is the count controller's plan without the
 * re-plan, which stays more than half a scene below capacity. */
#define RP_COST 1570000LL
#define RP_D1 11990000LL
#define RP_D2 (RP_D1 + 16715000LL)
#define RP_VBL 300000LL
#define RP_SPAN 33433000LL
#define RP_MAXT 128

/* The cost sequence's state: the LCG and the span's factor. */
typedef struct { uint32_t lcg; double span_k; } rp_costs_t;

/** @brief Return the LCG's next value in [0, 1).
 * @param r The sequence state.
 * @return The value. */
static double rp_unit(rp_costs_t *r)
{
    r->lcg = r->lcg * 1664525u + 1013904223u;
    return (double)(r->lcg >> 8) / 16777216.0;
}

/* The producer's cost estimate, as eager_cost_note keeps it. */
typedef struct { int64_t ema, dev; } rp_est_t;

/** @brief Fold one scene cost into the estimate by eager_cost_note's rules (bounded sample, deviation step capped at ema + 4 dev).
 * @param e The estimate.
 * @param c The scene's cost in nanoseconds. */
static void rp_note(rp_est_t *e, int64_t c)
{
    if (!e->ema) { e->ema = c; return; }
    const int64_t s = op_eager_cost_sample(e->ema, c);
    int64_t d = s > e->ema ? s - e->ema : e->ema - s;
    const int64_t cap = e->ema + 4 * e->dev;
    if (d > cap) d = cap;
    e->dev = (e->dev * 31 + d) / 32;
    e->ema = (e->ema * 7 + s) / 8;
}

/* One span's producer: the cost estimate, the span's scene costs, the plan
 * and the walk through it, as eager_produce keeps them. */
typedef struct {
    rp_est_t e;
    const int64_t *cost;
    int64_t *target;     /* the plan's instants; NULL for a plan of n unplaced scenes */
    int64_t now;         /* the producer's clock, from the plan at 0 */
    int i, n;            /* instants produced, instants planned */
    int64_t floor_ns;    /* the re-plan's least spacing */
    int floor_bad;       /* a re-planned instant other than the last lies closer than floor_ns to its predecessor */
} rp_walk_t;

/** @brief Produce one window of the span as eager_produce does: re-plan at each boundary when asked, stop when the tail cost would pass end.
 * @param w The span's producer.
 * @param end The window's deadline.
 * @param replan Nonzero to re-plan the unproduced instants at each scene boundary.
 * @return The guard's room when the plan's last scene finished in this window, else -1. */
static int rp_window(rp_walk_t *w, int64_t end, int replan)
{
    while (w->i < w->n) {
        const int64_t tail = w->e.ema + 3 * w->e.dev;
        if (replan) {
            const int before = w->i;
            w->n = op_eager_replan(w->target, w->i, w->n, op_eager_guard_room(end - w->now, w->e.ema, tail),
                                   w->floor_ns, RP_MAXT);
            for (int q = before; q < w->n - 1; q++) w->floor_bad |= w->target[q] - w->target[q - 1] < w->floor_ns;
        }
        if (w->now + tail > end) return -1;
        w->now += w->cost[w->i];
        rp_note(&w->e, w->cost[w->i]);
        w->i++;
    }
    return op_eager_guard_room(end - w->now, w->e.ema, w->e.ema + 3 * w->e.dev);
}

/** @brief Produce one span: the canonical scene, the first window, the update VBL, the second window (re-planned when asked).
 * @param w The span's producer at the plan.
 * @param replan Nonzero to re-plan after the update VBL.
 * @return The guard's room when the plan finished: OP_EAGER_LADDER_MAX_AFFORD when it finished in the first window, 0 when cut. */
static int rp_span(rp_walk_t *w, int replan)
{
    w->now += w->cost[0];
    rp_note(&w->e, w->cost[0]);
    w->i = 1;
    if (w->i >= w->n || rp_window(w, RP_D1 - 500000, 0) >= 0) return OP_EAGER_LADDER_MAX_AFFORD;
    w->now = (w->now > RP_D1 ? w->now : RP_D1) + RP_VBL;
    const int spare = rp_window(w, RP_D2 - 500000, replan);
    return spare < 0 ? 0 : spare;
}

/** @brief Return how many scenes the produce guard starts in the span with no plan limit, on the span's costs.
 * @param e The estimate at the span's plan; not changed.
 * @param cost The span's scene costs.
 * @return The span's capacity. */
static int rp_capacity(const rp_est_t *e, const int64_t *cost)
{
    rp_walk_t w = { *e, cost, NULL, 0, 0, RP_MAXT, 0, 0 };
    rp_span(&w, 0);
    return w.i;
}

/* A re-plan run's means over its measured spans. */
typedef struct { double planned, capacity, seed; int cuts, floor_bad, ends_bad, spans; } rp_run_t;

/** @brief Run spans spans of the scripted costs through the seed plan and, when replan is set, the re-plan; measure after 64 spans.
 * @param replan Nonzero to re-plan after the update VBL.
 * @param floor_ns The least spacing.
 * @param spans The spans to run.
 * @return The run's means. */
static rp_run_t rp_run(int replan, int64_t floor_ns, int spans)
{
    rp_costs_t r = { 12345u, 1.0 };
    rp_est_t e = { 0, 0 };
    op_eager_count_t c;
    memset(&c, 0, sizeof c);
    rp_run_t out = { 0, 0, 0, 0, 0, 0, 0 };
    for (int s = 0; s < spans; s++) {
        int64_t cost[RP_MAXT], target[RP_MAXT];
        r.span_k = 0.92 + 0.16 * rp_unit(&r);
        for (int q = 0; q < RP_MAXT; q++) cost[q] = (int64_t)((double)RP_COST * r.span_k * (0.88 + 0.24 * rp_unit(&r)));
        const int afford = e.ema ? op_eager_ladder_afford(RP_D2 - RP_VBL - 1000000, e.ema, e.dev) : 8;
        int n = 0;
        const int64_t p = op_eager_ladder_fill(RP_SPAN, op_eager_count_plan(&c, afford), floor_ns, 1, &n);
        for (int q = 0; q < n; q++) target[q] = p * (q + 1);
        const int cap = rp_capacity(&e, cost);
        rp_walk_t w = { e, cost, target, 0, 0, n, floor_ns, 0 };
        const int end_spare = rp_span(&w, replan);
        e = w.e;
        int spare = 0;
        const op_eager_cut_t cause = op_eager_seed_view(w.i < w.n ? OP_EAGER_CUT_DEADLINE : OP_EAGER_CUT_NONE,
                                                        n, w.i, end_spare, &spare);
        op_eager_count_step(&c, cause, afford, spare, w.i, RP_SPAN, COUNT_RUNS, 1, 32);
        if (s < 64) continue;
        out.spans++;
        out.planned += w.n; out.capacity += cap; out.seed += n;
        out.cuts += w.i < w.n; out.floor_bad += w.floor_bad;
        out.ends_bad += target[0] != p || target[w.n - 1] != p * n;
    }
    out.planned /= out.spans; out.capacity /= out.spans; out.seed /= out.spans;
    return out;
}

/** @brief Check the re-plan: first and last instants kept, spacing at or above the floor, planned count within half a scene of capacity where the count controller's plan sits below it.
 * @param print Nonzero to print the runs' means. */
static void test_replan(int print)
{
    char what[160];
    /* op_eager_replan on a hand case: 3 produced of 6, room for 5 more, last kept. */
    int64_t t[16] = { 0, 10, 20, 30, 40, 60 };
    int n = op_eager_replan(t, 3, 6, 5, 0, 16);
    check(n == 8 && t[2] == 20 && t[3] == 28 && t[7] == 60 && t[0] == 0, "replan: 3 of 6 produced, 5 more spaced 8 to the kept last 60");
    n = op_eager_replan(t, 3, 8, 5, 15, 16);
    check(n == 5 && t[3] == 40 && t[4] == 60, "replan: a 15 floor keeps 2 of the 40 left");
    n = op_eager_replan(t, 3, 5, 0, 0, 16);
    check(n == 5 && t[4] == 60, "replan: no room leaves the plan for the guard to cut");
    check(op_eager_guard_room(5000, 1000, 2000) == 4 && op_eager_guard_room(1999, 1000, 2000) == 0,
          "guard room: tail for the last, the mean for each other");
    /* The seed view: unchanged on a span the re-plan did not reshape. */
    int sp = -1;
    check(op_eager_seed_view(OP_EAGER_CUT_NONE, 12, 12, 2, &sp) == OP_EAGER_CUT_NONE && sp == 2, "seed view: a plan run to its end keeps its spare");
    check(op_eager_seed_view(OP_EAGER_CUT_DEADLINE, 12, 9, 0, &sp) == OP_EAGER_CUT_DEADLINE, "seed view: a cut seed stays cut");
    check(op_eager_seed_view(OP_EAGER_CUT_NONE, 12, 15, 0, &sp) == OP_EAGER_CUT_NONE && sp == 3, "seed view: a plan grown by 3 gives the seed 3 spare");
    check(op_eager_seed_view(OP_EAGER_CUT_NONE, 12, 11, 0, &sp) == OP_EAGER_CUT_DEADLINE, "seed view: a plan shrunk below the seed cuts the seed");
    check(op_eager_seed_view(OP_EAGER_CUT_RING, 12, 5, 0, &sp) == OP_EAGER_CUT_RING, "seed view: a ring cut passes through");

    const rp_run_t re = rp_run(1, 500000, 1200), old = rp_run(0, 500000, 1200);
    const rp_run_t fl = rp_run(1, 3000000, 1200);
    if (print) {
        printf("replan    : planned %.2f capacity %.2f seed %.2f cuts %d/%d\n", re.planned, re.capacity, re.seed, re.cuts, re.spans);
        printf("seed only : planned %.2f capacity %.2f seed %.2f cuts %d/%d\n", old.planned, old.capacity, old.seed, old.cuts, old.spans);
        printf("floor 3 ms: planned %.2f capacity %.2f seed %.2f cuts %d/%d\n", fl.planned, fl.capacity, fl.seed, fl.cuts, fl.spans);
    }
    snprintf(what, sizeof what, "replan: planned %.2f within 0.5 of capacity %.2f", re.planned, re.capacity);
    check(re.planned - re.capacity <= 0.5 && re.capacity - re.planned <= 0.5, what);
    snprintf(what, sizeof what, "replan: first and last instants kept (%d spans moved one), spacing at the floor (%d below)",
             re.ends_bad, re.floor_bad);
    check(re.ends_bad == 0 && re.floor_bad == 0, what);
    snprintf(what, sizeof what, "replan floor 3 ms: ends kept (%d), spacing at the floor (%d below), planned %.2f <= %d",
             fl.ends_bad, fl.floor_bad, fl.planned, (int)(RP_SPAN / 3000000));
    check(fl.ends_bad == 0 && fl.floor_bad == 0 && fl.planned <= RP_SPAN / 3000000, what);
    snprintf(what, sizeof what, "control: the seed plan %.2f sits more than 0.5 below capacity %.2f", old.planned, old.capacity);
    check(old.capacity - old.planned > 0.5, what);
}

/** @brief Run the checks; `replay` also prints the recorded-row table.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on pass, 1 on a failed check. */
/** @brief The presenter cap's backlog gate: arming on backlog evidence only, loosening on a capped span's starved picks, disarming with backoff on an uncapped starved pick, and the cap count.
 *
 * A presenter keeping up (a starved pick in every span, as a vsync-480
 * presenter at its slot rate) never arms the gate; one behind (no starved
 * pick, two retired scenes per span) arms after the base need; a starved
 * pick while capped raises the slack, and one while uncapped disarms and
 * doubles the next need. */
/* op_eager_cap_step() rows: picks, starved, retired, capped, then busy as
 * service and wall time; S10 = 10 ms service, so 3 picks over W30 = 30 ms
 * is busy 1.0. */
#define S10 (10 * MS)
#define W30 (30 * MS)

/** @brief Check the presenter cap gate's arming: behind blocks arm it, a keeping-up presenter or no retired scene never does. */
static void test_cap_gate_arming(void)
{
    op_eager_cap_t c = {0};
    /* Keeping up: three of four picks starved (the least starved fraction of
     * a presenter that keeps up at 480 Hz), a scene retired per span. */
    for (int k = 0; k < 400; k++) op_eager_cap_step(&c, 12, 9, 1, 0, 2 * MS, 25 * MS, 16, 256);
    check(!c.armed && c.arms == 0, "cap gate: a presenter that starves three picks in four never arms");
    memset(&c, 0, sizeof c);
    for (int k = 0; k < 400; k++) op_eager_cap_step(&c, 12, 0, 0, 0, 2 * MS, 25 * MS, 16, 256);
    check(!c.armed, "cap gate: no retired scene, no arming");
    memset(&c, 0, sizeof c);
    /* Behind: about one pick in ten starved (an acquire with no image-fence
     * wait, a pick ahead of the nearest scene), three scenes retired per span. */
    for (int k = 0; k < 15; k++) op_eager_cap_step(&c, 3, k % 3 == 0, 3, 0, S10, W30, 16, 256);
    check(!c.armed, "cap gate: 15 spans of backlog do not arm at need 16");
    op_eager_cap_step(&c, 0, 0, 0, 0, S10, W30, 16, 256);
    check(!c.armed && c.b_spans == 15, "cap gate: a span with no pick carries no evidence");
    op_eager_cap_step(&c, 3, 0, 3, 0, S10, W30, 16, 256);
    check(c.armed && c.arms == 1 && c.slack == 0, "cap gate: the 16th backlog span arms despite isolated starved picks");
    memset(&c, 0, sizeof c);
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, k < 13, 3, 0, S10, W30, 16, 256);
    check(!c.armed, "cap gate: a block with more than a quarter of its picks starved does not arm");
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, 0, 3, 0, S10, W30, 16, 256);
    check(c.armed, "cap gate: the next behind block arms");
}

/** @brief Check the armed presenter cap gate: capped blocks judged by busy, uncapped ones by starved picks, the slack and the need. */
static void test_cap_gate_armed(void)
{
    op_eager_cap_t c = {0};
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, 0, 3, 0, S10, W30, 16, 256);
    check(c.armed, "cap gate: armed for the armed-block rows");
    /* A saturated presenter under the cap: 9.4 picks per span at 3.6 ms
     * service over 34 ms, busy 1.0, two picks in three starved. */
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 9 + (k % 5 < 2), 6, 0, k < 14, 3600000, 34 * MS, 16, 256);
    check(c.armed && c.disarms == 0 && c.slack == 0,
          "cap gate: a capped saturated block stays armed whatever its starved picks, slack unchanged");
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, 3, 0, 1, S10, 35 * MS, 16, 256);
    check(c.armed && c.slack == 1 && c.loosened == 1,
          "cap gate: a capped block busy 0.86 raises the slack by one");
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, 2, 1, 1, S10, W30, 16, 256);
    check(c.armed && c.slack == 0,
          "cap gate: a saturated capped block retiring a scene per span gives one back whatever its starved picks");
    c.slack = 1;
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, 2, 0, 1, S10, W30, 16, 256);
    check(c.slack == 1, "cap gate: a saturated capped block retiring under a scene per span keeps the slack");
    c.slack = 0;
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, k < 16, 0, 0, S10, W30, 16, 256);
    check(!c.armed && c.disarms == 1 && c.need == 32 && c.slack == 0,
          "cap gate: an uncapped block with a third of its picks starved disarms and doubles the need");
    for (int k = 0; k < 31; k++) op_eager_cap_step(&c, 2, 0, 2, 0, S10, 20 * MS, 16, 256);
    check(!c.armed, "cap gate: after a disarm 31 spans do not arm at need 32");
    op_eager_cap_step(&c, 2, 0, 2, 0, S10, 20 * MS, 16, 256);
    check(c.armed, "cap gate: the 32nd span re-arms");
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 2, 0, 1, 1, S10, 40 * MS, 16, 256);
    check(!c.armed && c.disarms == 2 && c.need == 64,
          "cap gate: a capped idle block (busy 0.5) disarms with no starved pick");
    memset(&c, 0, sizeof c);
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 3, 0, 3, 0, S10, W30, 16, 256);
    check(c.armed, "cap gate: armed for the keep-up row");
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 4, 3, 0, 0, 2 * MS, 8 * MS + MS / 3, 16, 256);
    check(!c.armed && c.disarms == 1,
          "cap gate: an uncapped keep-up block (three picks in four starved, busy 0.96) disarms");
    memset(&c, 0, sizeof c);
    for (int k = 0; k < 16; k++) op_eager_cap_step(&c, 2, 0, 2, 0, S10, 20 * MS, 16, 256);
    c.need = 64;
    for (int k = 0; k < 256; k++) op_eager_cap_step(&c, 2, 0, 1, 1, S10, 20 * MS, 16, 256);
    check(c.armed && c.need == 16, "cap gate: a long armed run restores the base need");
}

/** @brief Check the presenter cap count: consumption over the span at the service time, plus the slack, plus one. */
static void test_cap_count(void)
{
    int64_t sp = 0;
    check(op_eager_cap_count(33 * MS, 15 * MS, 0, 48, &sp) == 4 && sp == 11 * MS,
          "cap count: 33 ms at 15 ms service consumes 3, cap 4, spacing 11 ms");
    check(op_eager_cap_count(33 * MS, 15 * MS, 2, 48, &sp) == 6, "cap count: the slack adds scenes");
    check(op_eager_cap_count(33 * MS, 1, 0, 48, &sp) == 49, "cap count: bounded at the span maximum plus one");
}

int main(int argc, char **argv)
{
    test_afford();
    test_steps();
    test_replay(argc > 1 && !strcmp(argv[1], "replay"));
    test_fill();
    test_free(argc > 1 && !strcmp(argv[1], "replay"));
    test_chain();
    test_retarget();
    test_count_steps();
    test_count(argc > 1 && !strcmp(argv[1], "replay"));
    test_count_transition(argc > 1 && !strcmp(argv[1], "replay"));
    test_count_bound();
    test_vsync();
    test_stall();
    test_cost_ema(argc > 1 && !strcmp(argv[1], "replay"));
    test_spread();
    test_fill_count();
    test_fill_recovery();
    test_fill_cut_causes();
    test_replan(argc > 1 && !strcmp(argv[1], "replay"));
    test_cap_gate_arming();
    test_cap_gate_armed();
    test_cap_count();
    printf("eager_ladder_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
