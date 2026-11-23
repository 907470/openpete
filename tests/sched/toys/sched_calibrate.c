/* sched_calibrate.c - calibration of the scheduler on toy programs with
 * known bugs (and known bug-free variants).
 *
 * Each check runs an exploration and compares its outcome with the known
 * truth; the program exits 1 when any check disagrees. Checks:
 *   lost update through a counter the scheduler does not watch: not found;
 *   the same counter watched: found;
 *   two threads of two yields each: exactly 6 schedules at unbounded preemption;
 *   A-B/B-A lock order: deadlock found;
 *   condition signal before the wait: deadlock found; a timed wait: none;
 *   a bug behind one data choice: not found at (0,0), found at (0,1);
 *   a bug behind one time advance: not found at (0,0), found at (0,1);
 *   replay of a failing DFS and a failing PCT choice string reproduces the
 *   failing trace byte for byte.
 * Usage: sched_calibrate <output directory> [jobs] */
#include "op_sched.h"
#include "toys.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RA __builtin_return_address(0)

static const char *s_dir;
static int s_jobs = 1;
static int s_bad;
static int s_marker;   /* object of the explicit yields */

/** @brief Report one check's verdict and count a disagreement.
 * @param name Check name.
 * @param ok Nonzero when the outcome matched the known truth.
 * @param detail What was measured. */
static void verdict(const char *name, int ok, const char *detail)
{
    printf("calibrate: %-28s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
    fflush(stdout);
    if (!ok) s_bad++;
}

/** @brief Return a DFS configuration for a scenario under the joint bound (p, d).
 * @param fn Scenario.
 * @param arg Its argument.
 * @param p Preemption bound.
 * @param d Deviation bound.
 * @return The configuration. */
static sched_config dfs(void (*fn)(void *), void *arg, int p, int d)
{
    sched_config c;
    memset(&c, 0, sizeof c);
    c.scenario = fn; c.arg = arg; c.mode = SCHED_MODE_DFS;
    c.max_preemptions = p; c.max_deviations = d;
    c.jobs = s_jobs; c.stop_on_fail = 1; c.quiet = 1;
    c.horizon_ns = 10000000000ll;
    return c;
}

/** @brief Thread body: one increment of the counter it is given.
 * @param arg The counter.
 * @return 0. */
static int counter_thread(void *arg)
{
    toy_counter_inc(arg);
    return 0;
}

/** @brief Lost-update scenario: two threads each increment the counter once; fails unless it ends at 2.
 * @param arg The counter (watched when it is g_toy_counter_seamed). */
static void scen_counter(void *arg)
{
    int *c = arg;
    if (c == &g_toy_counter_seamed) sched_watch(c, sizeof *c, "counter");
    const int t = sched_thread_create(counter_thread, c, "inc", RA);
    toy_counter_inc(c);
    sched_thread_join(t, RA);
    if (*c != 2) sched_fail("lost update: counter is %d", *c);
}

/** @brief Thread body: two explicit yields.
 * @param arg Unused.
 * @return 0. */
static int two_points(void *arg)
{
    (void)arg;
    sched_point(SCHED_OP_POINT, &s_marker, "B1");
    sched_point(SCHED_OP_POINT, &s_marker, "B2");
    return 0;
}

/** @brief Two threads of two yields each.
 * @param arg Unused. */
static void scen_six(void *arg)
{
    (void)arg;
    const int t = sched_thread_create(two_points, NULL, "B", RA);
    sched_point(SCHED_OP_POINT, &s_marker, "A1");
    sched_point(SCHED_OP_POINT, &s_marker, "A2");
    sched_thread_join(t, RA);
}

static SDL_Mutex *s_ma, *s_mb;

/** @brief Thread body: take the locks in B-A order.
 * @param arg Unused.
 * @return 0. */
static int ba_thread(void *arg)
{
    (void)arg;
    SDL_LockMutex(s_mb);
    SDL_LockMutex(s_ma);
    SDL_UnlockMutex(s_ma);
    SDL_UnlockMutex(s_mb);
    return 0;
}

/** @brief A-B/B-A lock order through the SDL wraps.
 * @param arg Unused. */
static void scen_deadlock(void *arg)
{
    (void)arg;
    sched_sdl_admit("toy-ba");
    s_ma = SDL_CreateMutex(); s_mb = SDL_CreateMutex();
    sched_name(s_ma, "mutex-a"); sched_name(s_mb, "mutex-b");
    SDL_Thread *t = SDL_CreateThread(ba_thread, "toy-ba", NULL);
    SDL_LockMutex(s_ma);
    SDL_LockMutex(s_mb);
    SDL_UnlockMutex(s_mb);
    SDL_UnlockMutex(s_ma);
    SDL_WaitThread(t, NULL);
}

static SDL_Mutex *s_m;
static SDL_Condition *s_cv;
static int s_ready;

/** @brief Thread body: publish the flag and signal, without the mutex.
 * @param arg Unused.
 * @return 0. */
static int producer(void *arg)
{
    (void)arg;
    sched_point(SCHED_OP_WRITE, &s_ready, "ready=1");
    s_ready = 1;
    SDL_SignalCondition(s_cv);
    return 0;
}

/** @brief Consumer checks the flag once, then waits: a signal between the check and the wait is lost.
 * @param arg Non-NULL for a 10 ms timed wait, which recovers. */
static void scen_signal(void *arg)
{
    sched_sdl_admit("toy-producer");
    s_m = SDL_CreateMutex(); s_cv = SDL_CreateCondition();
    sched_name(s_m, "mutex"); sched_name(s_cv, "cond"); sched_name(&s_ready, "ready");
    SDL_Thread *t = SDL_CreateThread(producer, "toy-producer", NULL);
    SDL_LockMutex(s_m);
    sched_point(SCHED_OP_READ, &s_ready, "if(!ready)");
    if (!s_ready) {
        if (arg) (void)SDL_WaitConditionTimeout(s_cv, s_m, 10);
        else SDL_WaitCondition(s_cv, s_m);
    }
    SDL_UnlockMutex(s_m);
    SDL_WaitThread(t, NULL);
}

/** @brief A bug behind one non-default data choice.
 * @param arg Unused. */
static void scen_data(void *arg)
{
    (void)arg;
    sched_point(SCHED_OP_DEVICE, &s_marker, "call");
    if (sched_choose(3, "toy-result") == 2) sched_fail("result 2 taken");
}

/** @brief A bug behind one time advance: two clock reads 5 ms or more apart fail.
 * @param arg Unused. */
static void scen_time(void *arg)
{
    (void)arg;
    const int64_t t0 = sched_clock_read(RA);
    const int64_t t1 = sched_clock_read(RA);
    if (t1 - t0 >= 5000000) sched_fail("slow by %lld ns", (long long)(t1 - t0));
}

/** @brief Run an exploration, print its engagement line, and return its result.
 * @param label Name.
 * @param c Configuration.
 * @param r Result.
 * @return The exploration's return value. */
static int run(const char *label, const sched_config *c, sched_result *r)
{
    const int e = sched_explore(c, r);
    sched_print_result(label, r);
    if (e == 2) { verdict(label, 0, "explorer error"); }
    return e;
}

/** @brief Compare two files byte for byte.
 * @param a Path.
 * @param b Path.
 * @param na Receives the size of a.
 * @return 1 when both exist, are non-empty and are identical. */
static int same_file(const char *a, const char *b, long *na)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = fa && fb;
    long n = 0;
    while (same) {
        const int ca = fgetc(fa), cb = fgetc(fb);
        if (ca != cb) same = 0;
        if (ca == EOF) break;
        n++;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    *na = n;
    return same && n > 0;
}

/** @brief The lost-update pair: unwatched not found, watched found; the hooks must have seen the accesses. */
static void check_counter(void)
{
    const unsigned long seen0 = sched_watch_counts(NULL);
    toy_counter_inc(&g_toy_counter_unseamed);   /* engagement: the instrumentation calls the hooks */
    const unsigned long seen = sched_watch_counts(NULL) - seen0;
    g_toy_counter_unseamed = 0;
    char d[1024];
    snprintf(d, sizeof d, "instrumented accesses per increment: %lu", seen);
    verdict("instrumentation engaged", seen == 2, d);
    sched_result r;
    sched_config c = dfs(scen_counter, &g_toy_counter_unseamed, 2, 0);
    run("counter-unseamed", &c, &r);
    snprintf(d, sizeof d, "%ld schedules, found=%d", r.schedules, r.found);
    verdict("lost update, unseamed", !r.found && r.complete, d);
    c = dfs(scen_counter, &g_toy_counter_seamed, 2, 0);
    run("counter-seamed", &c, &r);
    snprintf(d, sizeof d, "found=%d at (%d,%d): %s", r.found, r.found_p, r.found_d, r.message);
    verdict("lost update, seamed", r.found && r.found_p == 1, d);
}

/** @brief The schedule count of two threads of two yields each. */
static void check_six(void)
{
    sched_result r;
    sched_config c = dfs(scen_six, NULL, 64, 0);
    c.stop_on_fail = 0;
    run("six", &c, &r);
    char d[1024];
    snprintf(d, sizeof d, "%ld schedules, complete=%d", r.schedules, r.complete);
    verdict("2x2 yields enumerate 6", r.schedules == 6 && r.complete && !r.found, d);
}

/** @brief Deadlock and missed signal found; the timed wait recovers in every schedule. */
static void check_blocking(void)
{
    sched_result r;
    char d[1024];
    sched_config c = dfs(scen_deadlock, NULL, 2, 0);
    run("deadlock", &c, &r);
    snprintf(d, sizeof d, "found=%d at (%d,%d): %s", r.found, r.found_p, r.found_d, r.message);
    verdict("A-B/B-A deadlock", r.found && strstr(r.message, "deadlock") != NULL, d);
    c = dfs(scen_signal, NULL, 2, 1);
    run("missed-signal", &c, &r);
    snprintf(d, sizeof d, "found=%d at (%d,%d): %s", r.found, r.found_p, r.found_d, r.message);
    verdict("missed signal", r.found && strstr(r.message, "deadlock") != NULL, d);
    c = dfs(scen_signal, &s_marker, 2, 1);
    run("missed-signal-timed", &c, &r);
    snprintf(d, sizeof d, "%ld schedules, found=%d complete=%d", r.schedules, r.found, r.complete);
    verdict("timed wait recovers", !r.found && r.complete, d);
}

/** @brief A bug behind one deviation is missed at (0,0) and found at (0,1), for a data choice and a time advance. */
static void check_deviation(void)
{
    static const int64_t adv[] = { 50000, 5000000 };
    sched_result r0, r1;
    char d[1024];
    sched_config c = dfs(scen_data, NULL, 0, 0);
    run("data-(0,0)", &c, &r0);
    c.max_deviations = 1;
    run("data-(0,1)", &c, &r1);
    snprintf(d, sizeof d, "(0,0) found=%d; (0,1) found=%d at (%d,%d)", r0.found, r1.found, r1.found_p, r1.found_d);
    verdict("data choice at (0,1)", !r0.found && r0.complete && r1.found && r1.found_p == 0 && r1.found_d == 1, d);
    c = dfs(scen_time, NULL, 0, 0);
    c.advance = adv; c.n_advance = 2;
    run("time-(0,0)", &c, &r0);
    c.max_deviations = 1;
    run("time-(0,1)", &c, &r1);
    snprintf(d, sizeof d, "(0,0) found=%d; (0,1) found=%d at (%d,%d)", r0.found, r1.found, r1.found_p, r1.found_d);
    verdict("time advance at (0,1)", !r0.found && r0.complete && r1.found && r1.found_d == 1, d);
}

/** @brief Replay the found schedule's choice string and compare traces.
 * @param label Name.
 * @param c The exploration's configuration (its trace_path holds the failing trace).
 * @param found Its result. */
static void check_replay_of(const char *label, const sched_config *c, const sched_result *found)
{
    char path[1024], name[64], d[4400];
    snprintf(path, sizeof path, "%s/%s-replay.trace", s_dir, label);
    snprintf(name, sizeof name, "replay %s", label);
    sched_config rc = *c;
    rc.mode = SCHED_MODE_REPLAY; rc.replay = found->choices; rc.trace_path = path;
    sched_result r;
    run(name, &rc, &r);
    long n = 0;
    const int same = found->found && r.found && same_file(c->trace_path, path, &n);
    snprintf(d, sizeof d, "choices %s, trace %ld bytes, identical=%d", found->choices, n, same);
    verdict(name, same, d);
}

/** @brief Replay of a DFS failure and of a PCT failure. */
static void check_replay(void)
{
    char p1[1024], p2[1024];
    snprintf(p1, sizeof p1, "%s/dfs-deadlock.trace", s_dir);
    snprintf(p2, sizeof p2, "%s/pct-deadlock.trace", s_dir);
    sched_result r;
    sched_config c = dfs(scen_deadlock, NULL, 2, 0);
    c.trace_path = p1;
    run("dfs-deadlock", &c, &r);
    check_replay_of("dfs", &c, &r);
    c.mode = SCHED_MODE_PCT; c.seed = 1; c.pct_runs = 500; c.pct_depth = 3; c.pct_steps = 12;
    c.pct_deviation = 0.05; c.trace_path = p2;
    run("pct-deadlock", &c, &r);
    char d[1024];
    snprintf(d, sizeof d, "found after %ld runs", r.schedules);
    verdict("PCT finds deadlock", r.found, d);
    check_replay_of("pct", &c, &r);
}

/** @brief Run every calibration check.
 * @param argc Argument count.
 * @param argv Output directory, optional jobs.
 * @return 0 when every check agrees with the known truth, else 1. */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: sched_calibrate <outdir> [jobs]\n"); return 2; }
    s_dir = argv[1];
    if (argc > 2) s_jobs = atoi(argv[2]);
    mkdir(s_dir, 0755);
    check_counter();
    check_six();
    check_blocking();
    check_deviation();
    check_replay();
    printf("calibrate: %s (%d disagreement(s))\n", s_bad ? "FAIL" : "PASS", s_bad);
    return s_bad ? 1 : 0;
}
