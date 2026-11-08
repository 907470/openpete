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

/** @brief Run the checks; `replay` also prints the recorded-row table.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return 0 on pass, 1 on a failed check. */
int main(int argc, char **argv)
{
    test_afford();
    test_steps();
    test_replay(argc > 1 && !strcmp(argv[1], "replay"));
    printf("eager_ladder_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
