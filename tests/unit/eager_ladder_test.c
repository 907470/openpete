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
 * The window rows are cut spans of the same levels after the free-span
 * sizing (Windows, the same flags; 17-18% of spans cut). A replay of the
 * produce loop's rule (a scene starts only while start + tail is at or
 * before its window's end) on each span's recorded scene costs reproduces
 * the count it produced; the logged afford, the sum price over one budget,
 * exceeds it, and the window price does not.
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

/* Window records: one cut level-1 span per level (its plan's implied
 * deviation, (tail - mean cost) / 3, inside the runs' logged 0.05-0.12 ms):
 * budget, update-VBL cost, tail, this span's delay from the update VBL's
 * deadline to its next scene, the run's median of that delay, the logged
 * afford, the scenes produced, and the scene costs in order, ms. */
static const struct {
    const char *level; double budget, vbl, tail, resume, resume_med; int afford, produced, n; double cost[32];
} k_win[] = {
    { "s1 artisans", 29.721, 0.058, 1.653, 0.170, 0.172, 20, 18, 18, { 1.559, 1.513, 1.486, 1.472, 1.592, 1.489, 1.443, 1.401, 1.516, 1.567, 1.521, 1.517, 1.455, 1.439, 1.452, 1.398, 1.471, 1.399 } },
    { "s1 stone_hill", 29.749, 0.057, 1.511, 0.172, 0.178, 23, 20, 20, { 1.221, 1.331, 1.292, 1.345, 1.378, 1.349, 1.351, 1.425, 1.457, 1.467, 1.437, 1.408, 1.292, 1.283, 1.270, 1.273, 1.254, 1.284, 1.268, 1.304 } },
    { "s1 town_square", 29.966, 0.047, 1.440, 0.157, 0.162, 24, 23, 23, { 1.218, 1.082, 1.057, 1.087, 1.090, 1.075, 1.066, 1.222, 1.168, 1.308, 1.303, 1.300, 1.265, 1.311, 1.290, 1.224, 1.257, 1.235, 1.240, 1.171, 1.142, 1.136, 1.126 } },
    { "s2 autumn_plains", 28.780, 0.054, 2.059, 0.361, 0.366, 15, 14, 14, { 1.836, 1.785, 1.721, 1.740, 1.725, 1.826, 1.929, 1.813, 1.782, 1.746, 1.794, 1.738, 1.760, 1.749 } },
    { "s2 glimmer", 29.299, 0.047, 1.646, 0.366, 0.368, 20, 19, 19, { 1.397, 1.266, 1.313, 1.399, 1.330, 1.433, 1.513, 1.518, 1.475, 1.538, 1.476, 1.471, 1.457, 1.501, 1.412, 1.448, 1.455, 1.306, 1.263 } },
    { "s2 hurricos", 29.072, 0.048, 1.342, 0.390, 0.402, 25, 24, 24, { 1.104, 1.124, 1.133, 1.107, 1.098, 1.186, 1.204, 1.178, 1.202, 1.204, 1.184, 1.223, 1.196, 1.227, 1.180, 1.230, 1.139, 1.109, 1.168, 1.100, 1.103, 1.122, 1.213, 1.144 } },
    { "s2 summer_forest", 29.105, 0.055, 1.372, 0.463, 0.396, 24, 23, 23, { 1.391, 1.107, 1.095, 1.076, 1.131, 1.150, 1.135, 1.182, 1.254, 1.239, 1.254, 1.140, 1.125, 1.203, 1.207, 1.148, 1.054, 1.055, 1.122, 1.124, 1.084, 1.128, 1.111 } },
    { "s3 cloud_spires", 28.798, 0.047, 1.845, 0.553, 0.464, 17, 16, 16, { 1.733, 1.572, 1.577, 1.560, 1.590, 1.619, 1.727, 1.820, 1.714, 1.586, 1.616, 1.594, 1.594, 1.694, 1.729, 1.519 } },
    { "s3 sunny_villa", 28.465, 0.054, 2.075, 0.451, 0.467, 15, 14, 14, { 1.778, 1.860, 1.893, 1.871, 1.955, 1.937, 2.001, 2.049, 2.137, 2.003, 1.835, 1.769, 1.828, 1.814 } },
    { "s3 sunrise_spring", 27.808, 0.074, 1.881, 0.505, 0.469, 17, 16, 16, { 1.983, 1.602, 1.521, 1.521, 1.535, 1.510, 1.677, 1.693, 1.616, 1.556, 1.537, 1.995, 1.828, 1.575, 1.546, 1.489 } },
};

/** @brief Replay the produce guard over a span's two windows on recorded scene costs and return the scenes it starts.
 *
 * The first scene always starts, at 0; every later one only while its start
 * plus tail is at or before the end of the window it starts in. The second
 * window opens at start2, or when the first window's last scene ends if later.
 * @param end1 The first window's end, from the plan instant.
 * @param start2 The second window's start.
 * @param end2 The second window's end.
 * @param cost The scene costs in order.
 * @param n The number of costs; the replay starts no more scenes than this.
 * @param tail The guard's tail bound.
 * @return The scenes started. */
static int guard_replay(int64_t end1, int64_t start2, int64_t end2, const int64_t *cost, int n, int64_t tail)
{
    int64_t t = cost[0];
    int k = 1;
    while (k < n && t + tail <= end1) t += cost[k++];
    if (t < start2) t = start2;
    while (k < n && t + tail <= end2) t += cost[k++];
    return k;
}

/** @brief Replay the window rows: the guard reproduces the recorded count, the logged afford exceeds it, the window price does not.
 * @param print Nonzero to print the table. */
static void test_windows(int print)
{
    const int64_t vbl_period = 16714882;   /* 37609 / 2250000 s */
    const int64_t guard_ns = 500000;
    if (print) printf("level              produced guard logged-afford windows\n");
    for (size_t i = 0; i < sizeof k_win / sizeof k_win[0]; i++) {
        int64_t cost[32], sum = 0;
        for (int q = 0; q < k_win[i].n; q++) { cost[q] = (int64_t)(k_win[i].cost[q] * MS); if (q) sum += cost[q]; }
        const int64_t ema = sum / (k_win[i].n - 1), tail = (int64_t)(k_win[i].tail * MS);
        const int64_t dev = (tail - ema) / 3;
        const int64_t prod_end = (int64_t)((k_win[i].budget + k_win[i].vbl + 1.0) * MS);
        const int64_t produce_end = prod_end - vbl_period;
        /* A span the guard would let run on gets one more scene at the mean cost. */
        cost[k_win[i].n] = ema;
        const int g = guard_replay(produce_end - guard_ns, produce_end + (int64_t)(k_win[i].resume * MS),
                                   prod_end - guard_ns, cost, k_win[i].n + 1, tail);
        const int win = op_eager_ladder_afford_windows(produce_end - guard_ns,
                                                       prod_end - guard_ns - (produce_end + (int64_t)(k_win[i].resume_med * MS)),
                                                       ema, dev, tail);
        if (print) printf("%-18s %8d %5d %13d %7d\n", k_win[i].level, k_win[i].produced, g, k_win[i].afford, win);
        char what[112];
        snprintf(what, sizeof what, "calibration %s: the guard replay starts the %d scenes recorded (got %d)",
                 k_win[i].level, k_win[i].produced, g);
        check(g == k_win[i].produced, what);
        snprintf(what, sizeof what, "window %s: priced %d, at most the %d produced and at least one fewer",
                 k_win[i].level, win, k_win[i].produced);
        check(win <= k_win[i].produced && win >= k_win[i].produced - 1, what);
        snprintf(what, sizeof what, "control %s: the logged afford %d planned past the %d produced",
                 k_win[i].level, k_win[i].afford, k_win[i].produced);
        check(k_win[i].afford > k_win[i].produced, what);
    }
    check(op_eager_ladder_window(MS, 2 * MS, 0, 2 * MS) == 0, "window: one shorter than the tail admits none");
    check(op_eager_ladder_window(MS, 2 * MS, 0, MS) == 1, "window: one tail long admits the first scene");
    check(op_eager_ladder_afford_windows(0, 0, MS, 0, MS) == 1, "windows: the canonical floor");
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
    test_windows(argc > 1 && !strcmp(argv[1], "replay"));
    printf("eager_ladder_test: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail;
}
