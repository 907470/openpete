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
 * A span is cut when it plans more than its capacity. The vsync rows are
 * spans of the vsync-on runs with that price that it planned below the
 * slot count, where the sum price stays at or above it.
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

/** @brief Check the count controller's transitions: a cut costs one scene, a clean run with room gives one back, a run at the edge does not. */
static void test_count_steps(void)
{
    op_eager_count_t c;
    memset(&c, 0, sizeof c);
    check(op_eager_count_plan(&c, 20) == 20, "count: plans afford with no shortfall");
    op_eager_count_step(&c, 1, 0, 16, 2);
    check(op_eager_count_plan(&c, 20) == 19, "count: a cut plans one fewer");
    for (int i = 0; i < 15; i++) op_eager_count_step(&c, 0, 2, 16, 2);
    check(c.short_by == 1, "count: fifteen clean spans give nothing back");
    op_eager_count_step(&c, 0, 2, 16, 2);
    check(c.short_by == 0, "count: the sixteenth gives one back");
    op_eager_count_step(&c, 1, 0, 16, 2);
    for (int i = 0; i < 40; i++) op_eager_count_step(&c, 0, 1, 16, 2);
    check(c.short_by == 1, "count: spans finishing at the guard's edge hold the count");
    c.short_by = 30;
    check(op_eager_count_plan(&c, 20) == 1, "count: at least one scene");
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
            op_eager_count_step(&c, plan > cap, cap - plan, 16, 2);
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

/** @brief Run the checks; `replay` also prints the recorded-row table.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on pass, 1 on a failed check. */
int main(int argc, char **argv)
{
    test_afford();
    test_steps();
    test_replay(argc > 1 && !strcmp(argv[1], "replay"));
    test_fill();
    test_free(argc > 1 && !strcmp(argv[1], "replay"));
    test_count_steps();
    test_count(argc > 1 && !strcmp(argv[1], "replay"));
    test_vsync();
    test_stall();
    printf("eager_ladder_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
