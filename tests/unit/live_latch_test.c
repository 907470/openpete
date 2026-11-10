/* tests/unit/live_latch_test.c - a live setting latched once per present reads
 * one value through the whole present while another thread keeps setting it.
 *
 * The setter thread stores alternating values of an int setting (the raster
 * multiplier's shape) and a float setting (the aspect's shape) through
 * op_live_set / op_live_set_f, and checks after each store that the getter
 * form (op_live_wanted) returns the value it chose. The present thread runs
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
 * pattern the code under test, which fails.
 *
 * The test covers the latch idiom of openpete_live_latch.h. Whether every
 * renderer reader reads the latched value, and whether the latch runs before
 * the first reader of each present, is a property of native_render.c that
 * this binary does not link. */

#include "openpete_live_latch.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

#define PRESENTS 20000
#define READS    6

static _Atomic(int) s_mult_want = OP_LIVE_NONE;
static _Atomic(int) s_aspect_want = OP_LIVE_NONE;
static int   s_mult = 0;       /* latched; the present thread's only */
static float s_aspect = 0.0f;  /* latched; the present thread's only */
static atomic_int s_stop;
static atomic_int s_getter_wrong;

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
        if (op_live_wanted(&s_mult_want, -1) != m ||
            op_live_wanted_f(&s_aspect_want, -1.0f) != a)
            atomic_fetch_add(&s_getter_wrong, 1);
        i++;
    }
    return NULL;
}

/** @brief Run the presents and count those whose reads disagree.
 *
 * @param old 1 reads the setter's variables directly (the pre-latch pattern),
 *        0 latches once per present and reads the latched values.
 * @param changed Receives the presents whose first read differed from the
 *        previous present's.
 * @return The presents in which two reads of one setting disagreed. */
static int run_presents(int old, int* changed)
{
    int torn = 0, prev_m = -1;
    *changed = 0;
    for (int p = 0; p < PRESENTS; p++) {
        if (!old) {
            (void)op_live_latch(&s_mult_want, &s_mult);
            (void)op_live_latch_f(&s_aspect_want, &s_aspect);
        }
        int m0 = 0; float a0 = 0.0f; int bad = 0;
        for (int r = 0; r < READS; r++) {
            const int m = old ? op_live_wanted(&s_mult_want, 0) : s_mult;
            const float a = old ? op_live_wanted_f(&s_aspect_want, 0.0f) : s_aspect;
            if (r == 0) { m0 = m; a0 = a; }
            else if (m != m0 || a != a0) bad = 1;
            sched_yield();
        }
        if (m0 != prev_m) (*changed)++;
        prev_m = m0;
        torn += bad;
    }
    return torn;
}

/** @brief Run one mode against a live setter thread.
 *
 * @param old Passed to run_presents.
 * @param changed Receives run_presents' change count.
 * @return run_presents' disagreement count. */
static int run_mode(int old, int* changed)
{
    pthread_t th;
    atomic_store(&s_stop, 0);
    pthread_create(&th, NULL, setter, NULL);
    const int torn = run_presents(old, changed);
    atomic_store(&s_stop, 1);
    pthread_join(th, NULL);
    return torn;
}

/** @brief Check the single-thread semantics: no setter leaves the initializer, the latch reports a change once, -0.0f is stored as +0.0f.
 *
 * @return The failed checks. */
static int semantics(void)
{
    int fail = 0;
    _Atomic(int) w = OP_LIVE_NONE;
    _Atomic(int) wf = OP_LIVE_NONE;
    int cur = 7;
    float curf = 1.5f;
    if (op_live_latch(&w, &cur) != 0 || cur != 7) fail++;
    if (op_live_wanted(&w, cur) != 7) fail++;
    op_live_set(&w, 3);
    if (op_live_wanted(&w, cur) != 3 || cur != 7) fail++;
    if (op_live_latch(&w, &cur) != 1 || cur != 3) fail++;
    if (op_live_latch(&w, &cur) != 0 || cur != 3) fail++;
    if (op_live_latch_f(&wf, &curf) != 0 || curf != 1.5f) fail++;
    op_live_set_f(&wf, -0.0f);
    if (atomic_load(&wf) == OP_LIVE_NONE) fail++;
    if (op_live_latch_f(&wf, &curf) != 1 || curf != 0.0f) fail++;
    if (fail) fprintf(stderr, "live_latch_test: %d semantic check(s) failed\n", fail);
    return fail;
}

/** @brief Run the semantics, the latched mode and the negative control; `old` runs the pre-latch pattern as the code under test.
 *
 * @param argc Argument count.
 * @param argv `old` as the first argument selects the pre-latch pattern.
 * @return 0 on pass, 1 on a failure, 2 when the run is vacuous. */
int main(int argc, char** argv)
{
    const int old_under_test = argc > 1 && strcmp(argv[1], "old") == 0;
    if (semantics()) return 1;

    int changed = 0;
    const int torn = run_mode(old_under_test, &changed);
    const int wrong = atomic_load(&s_getter_wrong);
    printf("%s: %d presents, %d with a changed value, %d torn, getter wrong %d\n",
           old_under_test ? "pre-latch reads" : "latched", PRESENTS, changed, torn, wrong);
    if (changed < PRESENTS / 100) {
        fprintf(stderr, "VACUOUS: the setting changed in %d presents\n", changed);
        return 2;
    }
    if (torn || wrong) return 1;

    int ctl_changed = 0;
    const int ctl_torn = run_mode(1, &ctl_changed);
    printf("control (pre-latch reads): %d presents, %d with a changed value, %d torn\n",
           PRESENTS, ctl_changed, ctl_torn);
    if (ctl_torn == 0) {
        fprintf(stderr, "VACUOUS: the control saw no torn present\n");
        return 2;
    }
    printf("PASS\n");
    return 0;
}
