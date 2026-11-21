/* tests/unit/live_latch_test.c - a live setting latched once per present reads
 * one value through the whole present while another thread keeps setting it.
 *
 * The setter thread stores alternating values of an int setting (the raster
 * multiplier's shape) and a float setting (the aspect's shape) through
 * op_live_set / op_live_set_f, and checks after each store that the getter
 * form (op_live_get) returns the value it chose. The present thread runs
 * presents: op_live_latch on both settings, then several reads of the
 * latched values with yields between them, the way raster_quantize_prepend,
 * psx_dither_append and native_postfx_point_active read the latched statics
 * in one present. Every read in a present must equal the first, and the
 * latched values must have changed in many presents (engagement).
 *
 * The negative control runs the same presents with the read pattern the
 * renderer used before the latch: each read loads the setter's variable
 * itself. It must see two values inside one present, or the run did not
 * reach the interleaving the latch guards. `live_latch_test old` makes that
 * pattern the code under test, which fails; `old-producer` does so for the
 * producer case below alone.
 *
 * The producer case runs the same idiom on the other thread. A setter
 * thread (the dev overlay on the present thread) stores alternating values
 * of an int setting (the LOD mode's shape) and a float setting (the fog
 * scale's shape); the sim thread runs ticks: a latch before the guest
 * resume, then several hook reads with yields between them, then a
 * re-extract that latches again and reads again, and between ticks it also
 * sets the int setting itself (the settings menu, which runs on the sim
 * thread). Every read of one tick body, and every read of one re-extract,
 * must equal the first, and a value the sim thread set must be the one its
 * next latch applies unless the setter thread overwrote it in between. The
 * negative control reads the setter's variable at each hook.
 *
 * A third thread calls the getters of all four settings throughout (a
 * settings row or the settings save), each reading only the atomic wanted
 * value; every value must be one a setter or an initializer stored. Under
 * ThreadSanitizer this shows the getters race with no latch; `old-getter`
 * makes the getter read the latched values instead, the fallback the
 * getters had while a wanted value could be unset, which ThreadSanitizer
 * reports as a race (a plain run passes it).
 *
 * The test covers the latch idiom of openpete_live_latch.h. Whether every
 * renderer reader reads the latched value, and whether the latch runs before
 * the first reader of each present or tick, is a property of native_render.c
 * and no_lod.c that this binary does not link. */

#include "openpete_live_latch.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PRESENTS 20000
#define READS    6
/* A run continues past PRESENTS (or TICKS) until its engagement counts are
 * met, so a setter thread the host schedules rarely lengthens the run
 * instead of making it vacuous. RUN_CAP_S bounds the wall time of one run;
 * only a run that reaches it is reported vacuous. */
#define RUN_CAP_S 120

static _Atomic(int) s_mult_want = 0;     /* s_mult's initializer */
static _Atomic(int) s_aspect_want = 0;   /* s_aspect's initializer, float bits */
static int   s_mult = 0;       /* latched; the present thread's only */
static float s_aspect = 0.0f;  /* latched; the present thread's only */
static atomic_int s_stop;
static atomic_int s_getter_wrong;

#define TICKS 20000
static _Atomic(int) s_lod_want = 0;               /* s_lod's initializer */
static _Atomic(int) s_fog_want = OP_LIVE_F_ONE;   /* s_fog's initializer, float bits */
static int   s_lod = 0;        /* latched; the sim thread's only */
static float s_fog = 1.0f;     /* latched; the sim thread's only */
/* Stores of s_lod_want by each thread, counted once before and once after
 * the store: a thread that reads the other's count even and unchanged
 * across a window knows no store of the other thread landed in it. */
static atomic_uint s_lod_sets;
static atomic_uint s_sim_sets;

/** @brief Set both settings in a loop and check the getter returns each chosen value.
 *
 * @param unused Unused.
 * @return NULL. */
static void* setter(void* unused)
{
    (void)unused;
    static const int mults[] = { 0, 1, 2, 4 };
    static const float aspects[] = { 0.0f, 4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f };
    unsigned i = 0;
    while (!atomic_load(&s_stop)) {
        const int m = mults[i & 3];
        const float a = aspects[(i >> 1) & 3];
        op_live_set(&s_mult_want, m);
        op_live_set_f(&s_aspect_want, a);
        if (op_live_get(&s_mult_want) != m ||
            op_live_get_f(&s_aspect_want) != a)
            atomic_fetch_add(&s_getter_wrong, 1);
        i++;
    }
    return NULL;
}

/* 1 while the getter thread reads the latched values the way a getter with
 * a latched fallback did (the negative control for ThreadSanitizer). */
static int s_getter_old;
static atomic_long s_getter_reads;
static atomic_int s_getter_bad;

/** @brief Call the getters of all four settings in a loop from a third thread (a settings row or the settings save) and check each value is one a setter or the initializer stores.
 *
 * Under ThreadSanitizer this proves the getters read no value the latching
 * threads write; with s_getter_old set it reads the latched values instead,
 * which ThreadSanitizer reports.
 * @param unused Unused.
 * @return NULL. */
static void* getter(void* unused)
{
    (void)unused;
    while (!atomic_load(&s_stop)) {
        const int m = s_getter_old ? s_mult : op_live_get(&s_mult_want);
        const float a = s_getter_old ? s_aspect : op_live_get_f(&s_aspect_want);
        const int l = s_getter_old ? s_lod : op_live_get(&s_lod_want);
        const float f = s_getter_old ? s_fog : op_live_get_f(&s_fog_want);
        if (!(m == 0 || m == 1 || m == 2 || m == 4) || !(a >= 0.0f && a < 3.0f) ||
            !(l == 0 || l == 1) || !(f == 0.5f || f == 1.0f || f == 2.0f || f == 4.0f))
            atomic_fetch_add(&s_getter_bad, 1);
        atomic_fetch_add(&s_getter_reads, 1);
        sched_yield();
    }
    return NULL;
}

/** @brief Report whether RUN_CAP_S seconds have passed since a start instant.
 *
 * @param start The run's start, CLOCK_MONOTONIC.
 * @return 1 once the cap is reached, else 0. */
static int run_capped(const struct timespec* start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - start->tv_sec >= RUN_CAP_S;
}

/** @brief Report whether a run has its engagement: the change count and, for a control, a torn count reached, and a getter read made.
 *
 * @param n Presents or ticks run so far.
 * @param min_n PRESENTS or TICKS: the run is never shorter.
 * @param changed The run's change count so far.
 * @param need_torn 1 for a control, which runs until one torn read.
 * @param torn The run's torn count so far.
 * @return 1 when the run may stop. */
static int run_engaged(int n, int min_n, int changed, int need_torn, int torn)
{
    return n >= min_n && changed >= min_n / 100 && torn >= need_torn &&
           atomic_load(&s_getter_reads) > 0;
}

/** @brief Set the producer-read settings in a loop from the present thread and check the getter returns each chosen value.
 *
 * @param unused Unused.
 * @return NULL. */
static void* producer_setter(void* unused)
{
    (void)unused;
    static const float fogs[] = { 1.0f, 2.0f, 0.5f, 4.0f };
    unsigned i = 0;
    while (!atomic_load(&s_stop)) {
        const int l = (int)(i & 1);
        const float f = fogs[(i >> 1) & 3];
        const unsigned sim_before = atomic_load(&s_sim_sets);
        atomic_fetch_add(&s_lod_sets, 1);
        op_live_set(&s_lod_want, l);
        atomic_fetch_add(&s_lod_sets, 1);
        op_live_set_f(&s_fog_want, f);
        if (op_live_get(&s_lod_want) != l && !(sim_before & 1u) &&
            atomic_load(&s_sim_sets) == sim_before)
            atomic_fetch_add(&s_getter_wrong, 1);
        if (op_live_get_f(&s_fog_want) != f)
            atomic_fetch_add(&s_getter_wrong, 1);
        i++;
    }
    return NULL;
}

/** @brief Read both producer settings READS times with yields between, the way the hooks of one tick read them, and report whether the reads disagreed.
 *
 * @param old 1 reads the setter's variables directly, 0 the latched values.
 * @param first Receives the first read of the int setting.
 * @return 1 when two reads of one setting disagreed, else 0. */
static int producer_reads(int old, int* first)
{
    int l0 = 0, bad = 0; float f0 = 0.0f;
    for (int r = 0; r < READS; r++) {
        const int l = old ? op_live_get(&s_lod_want) : s_lod;
        const float f = old ? op_live_get_f(&s_fog_want) : s_fog;
        if (r == 0) { l0 = l; f0 = f; }
        else if (l != l0 || f != f0) bad = 1;
        sched_yield();
    }
    *first = l0;
    return bad;
}

/** @brief Run the ticks and re-extracts on the sim thread and count those whose reads disagree.
 *
 * Between ticks the sim thread stores the int setting itself, as the
 * settings menu does; when the setter thread made no store since, the next
 * latch must apply it.
 * @param old 1 reads the setter's variables directly, 0 latches before each tick body and each re-extract.
 * @param changed Receives the ticks whose first read differed from the previous tick's.
 * @param lost Receives the sim-thread stores the next latch did not apply although no other store intervened.
 * @param ran Receives the ticks run: at least TICKS, more until the change count (and for the pattern of `old`, one torn tick) is reached.
 * @return The tick bodies and re-extracts in which two reads of one setting disagreed. */
static int run_ticks(int old, int* changed, int* lost, int* ran)
{
    int torn = 0, prev_l = -1, t = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    *changed = 0;
    *lost = 0;
    for (; !run_engaged(t, TICKS, *changed, old, torn) && !run_capped(&start); t++) {
        if (!old) {   /* game_on_frame_pre: before the guest resume */
            (void)op_live_latch(&s_lod_want, &s_lod);
            (void)op_live_latch_f(&s_fog_want, &s_fog);
        }
        int l0;
        torn += producer_reads(old, &l0);
        if (l0 != prev_l) (*changed)++;
        prev_l = l0;
        if (!old) {   /* openpete_native_scene_begin: a re-extract */
            (void)op_live_latch(&s_lod_want, &s_lod);
            (void)op_live_latch_f(&s_fog_want, &s_fog);
        }
        torn += producer_reads(old, &l0);
        if (!old && (t & 7) == 0) {   /* the settings menu on the sim thread */
            const unsigned before = atomic_load(&s_lod_sets);
            const int want = !s_lod;
            atomic_fetch_add(&s_sim_sets, 1);
            op_live_set(&s_lod_want, want);
            atomic_fetch_add(&s_sim_sets, 1);
            (void)op_live_latch(&s_lod_want, &s_lod);
            if (s_lod != want && !(before & 1u) && atomic_load(&s_lod_sets) == before) (*lost)++;
        }
    }
    *ran = t;
    return torn;
}

/** @brief Run the producer case against a live setter thread on the present side.
 *
 * @param old Passed to run_ticks.
 * @param changed Receives run_ticks' change count.
 * @param lost Receives run_ticks' lost-store count.
 * @param ran Receives the ticks run.
 * @return run_ticks' disagreement count. */
static int run_producer(int old, int* changed, int* lost, int* ran)
{
    pthread_t th, gt;
    atomic_store(&s_stop, 0);
    pthread_create(&th, NULL, producer_setter, NULL);
    pthread_create(&gt, NULL, getter, NULL);
    const int torn = run_ticks(old, changed, lost, ran);
    atomic_store(&s_stop, 1);
    pthread_join(th, NULL);
    pthread_join(gt, NULL);
    return torn;
}

/** @brief Run the presents and count those whose reads disagree.
 *
 * @param old 1 reads the setter's variables directly (the pre-latch pattern),
 *        0 latches once per present and reads the latched values.
 * @param changed Receives the presents whose first read differed from the
 *        previous present's.
 * @param ran Receives the presents run: at least PRESENTS, more until the
 *        change count (and for the pattern of `old`, one torn present) is
 *        reached.
 * @return The presents in which two reads of one setting disagreed. */
static int run_presents(int old, int* changed, int* ran)
{
    int torn = 0, prev_m = -1, p = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    *changed = 0;
    for (; !run_engaged(p, PRESENTS, *changed, old, torn) && !run_capped(&start); p++) {
        if (!old) {
            (void)op_live_latch(&s_mult_want, &s_mult);
            (void)op_live_latch_f(&s_aspect_want, &s_aspect);
        }
        int m0 = 0; float a0 = 0.0f; int bad = 0;
        for (int r = 0; r < READS; r++) {
            const int m = old ? op_live_get(&s_mult_want) : s_mult;
            const float a = old ? op_live_get_f(&s_aspect_want) : s_aspect;
            if (r == 0) { m0 = m; a0 = a; }
            else if (m != m0 || a != a0) bad = 1;
            sched_yield();
        }
        if (m0 != prev_m) (*changed)++;
        prev_m = m0;
        torn += bad;
    }
    *ran = p;
    return torn;
}

/** @brief Run one mode against a live setter thread.
 *
 * @param old Passed to run_presents.
 * @param changed Receives run_presents' change count.
 * @param ran Receives the presents run.
 * @return run_presents' disagreement count. */
static int run_mode(int old, int* changed, int* ran)
{
    pthread_t th, gt;
    atomic_store(&s_stop, 0);
    pthread_create(&th, NULL, setter, NULL);
    pthread_create(&gt, NULL, getter, NULL);
    const int torn = run_presents(old, changed, ran);
    atomic_store(&s_stop, 1);
    pthread_join(th, NULL);
    pthread_join(gt, NULL);
    return torn;
}

/** @brief Check the single-thread semantics: a wanted value seeded with the latched value leaves it, a set shows in the getter before the latch, the latch reports a change once.
 *
 * @return The failed checks. */
static int semantics(void)
{
    int fail = 0;
    int cur = 7;
    float curf = 1.5f;
    _Atomic(int) w = 0;
    _Atomic(int) wf = 0;
    op_live_set(&w, cur);
    op_live_set_f(&wf, curf);
    if (op_live_latch(&w, &cur) != 0 || cur != 7) fail++;
    if (op_live_get(&w) != 7) fail++;
    op_live_set(&w, 3);
    if (op_live_get(&w) != 3 || cur != 7) fail++;
    if (op_live_latch(&w, &cur) != 1 || cur != 3) fail++;
    if (op_live_latch(&w, &cur) != 0 || cur != 3) fail++;
    if (op_live_latch_f(&wf, &curf) != 0 || curf != 1.5f) fail++;
    op_live_set_f(&wf, 0.25f);
    if (op_live_get_f(&wf) != 0.25f || curf != 1.5f) fail++;
    if (op_live_latch_f(&wf, &curf) != 1 || curf != 0.25f) fail++;
    if (fail) fprintf(stderr, "live_latch_test: %d semantic check(s) failed\n", fail);
    return fail;
}

/** @brief Run the producer case and its negative control.
 *
 * @param old_under_test 1 runs the pre-latch reads as the code under test.
 * @return 0 on pass, 1 on a failure, 2 when the run is vacuous. */
static int producer_case(int old_under_test)
{
    int changed = 0, lost = 0, ran = 0;
    const int torn = run_producer(old_under_test, &changed, &lost, &ran);
    const int wrong = atomic_load(&s_getter_wrong);
    printf("producer %s: %d ticks, %d with a changed value, %d torn, %d sim stores lost, getter wrong %d\n",
           old_under_test ? "pre-latch reads" : "latched", ran, changed, torn, lost, wrong);
    if (changed < TICKS / 100) {
        fprintf(stderr, "VACUOUS: the producer setting changed in %d ticks\n", changed);
        return 2;
    }
    if (torn || wrong || lost) return 1;

    int ctl_changed = 0, ctl_lost = 0, ctl_ran = 0;
    const int ctl_torn = run_producer(1, &ctl_changed, &ctl_lost, &ctl_ran);
    printf("producer control (pre-latch reads): %d ticks, %d with a changed value, %d torn\n",
           ctl_ran, ctl_changed, ctl_torn);
    if (ctl_torn == 0) {
        fprintf(stderr, "VACUOUS: the producer control saw no torn tick\n");
        return 2;
    }
    return 0;
}

/** @brief Run the semantics, the present and producer cases with their negative controls; `old` runs the pre-latch pattern as the code under test.
 *
 * @param argc Argument count.
 * @param argv `old` as the first argument selects the pre-latch pattern for
 *        both cases, `old-producer` for the producer case only, `old-getter`
 *        the latched-value getter reads (a ThreadSanitizer control).
 * @return 0 on pass, 1 on a failure, 2 when the run is vacuous. */
int main(int argc, char** argv)
{
    const int old_under_test = argc > 1 && strcmp(argv[1], "old") == 0;
    const int old_producer = old_under_test || (argc > 1 && strcmp(argv[1], "old-producer") == 0);
    s_getter_old = argc > 1 && strcmp(argv[1], "old-getter") == 0;
    if (semantics()) return 1;

    int changed = 0, ran = 0;
    const int torn = run_mode(old_under_test, &changed, &ran);
    const int wrong = atomic_load(&s_getter_wrong);
    printf("%s: %d presents, %d with a changed value, %d torn, getter wrong %d\n",
           old_under_test ? "pre-latch reads" : "latched", ran, changed, torn, wrong);
    if (changed < PRESENTS / 100) {
        fprintf(stderr, "VACUOUS: the setting changed in %d presents\n", changed);
        return 2;
    }
    if (torn || wrong) return 1;

    int ctl_changed = 0, ctl_ran = 0;
    const int ctl_torn = run_mode(1, &ctl_changed, &ctl_ran);
    printf("control (pre-latch reads): %d presents, %d with a changed value, %d torn\n",
           ctl_ran, ctl_changed, ctl_torn);
    if (ctl_torn == 0) {
        fprintf(stderr, "VACUOUS: the control saw no torn present\n");
        return 2;
    }
    const int prc = producer_case(old_producer);
    if (prc) return prc;
    const long greads = atomic_load(&s_getter_reads);
    const int gbad = atomic_load(&s_getter_bad);
    printf("getter thread: %ld reads, %d outside the stored values\n", greads, gbad);
    if (greads == 0) {
        fprintf(stderr, "VACUOUS: the getter thread made no read\n");
        return 2;
    }
    if (gbad) return 1;
    printf("PASS\n");
    return 0;
}
