/* tests/unit/openpete_time_test.c — verify openpete_time.c contract:
 *   1. op_now() is monotonically non-decreasing across calls.
 *   2. op_sleep_until(t) wakes at or after t (never undershoots).
 *   3. Overshoot is small on a quiet host (sanity bound, not a hard
 *      SLA — CI runners are noisy, so the bound is generous; the
 *      `pacing_quality` slow target is where we gate real numbers).
 *   4. op_pacer_slack_ns honours OPENPETE_PACER_SLACK_US.
 *
 * Compiled directly from the .c source (no openpete_core link) per the
 * pattern in psx_region_test.c — keeps the test binary SDL-free. */

#include "../../core/include/openpete_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); return 1; } while (0)

static int test_monotonic(void) {
    op_time_t prev = op_now();
    for (int i = 0; i < 10000; ++i) {
        op_time_t now = op_now();
        if (now < prev) {
            FAIL("op_now() went backwards: %" PRId64 " -> %" PRId64 "\n",
                 (int64_t)prev, (int64_t)now);
        }
        prev = now;
    }
    return 0;
}

static int test_sleep_reaches_deadline(void) {
    op_time_t now      = op_now();
    op_time_t deadline = now + 5 * OP_NS_PER_MS;  /* 5 ms */
    op_sleep_until(deadline);
    op_time_t after = op_now();
    if (after < deadline) {
        FAIL("op_sleep_until undershot: deadline=%" PRId64
             " woke at %" PRId64 " (delta %" PRId64 " ns)\n",
             (int64_t)deadline, (int64_t)after,
             (int64_t)(after - deadline));
    }
    /* Generous overshoot bound — 10 ms — to survive CI scheduler noise.
     * The real number on a quiet Linux dev box is < 100 µs. The
     * `pacing_quality` slow CTest target asserts the tight bound. */
    op_time_t overshoot = after - deadline;
    if (overshoot > 10 * OP_NS_PER_MS) {
        FAIL("op_sleep_until overshot by %" PRId64 " ns (>10 ms)\n",
             (int64_t)overshoot);
    }
    return 0;
}

static int test_sleep_in_the_past_returns_fast(void) {
    op_time_t deadline = op_now() - 1 * OP_NS_PER_SEC;
    op_time_t before   = op_now();
    op_sleep_until(deadline);
    op_time_t after    = op_now();
    if (after - before > 1 * OP_NS_PER_MS) {
        FAIL("op_sleep_until with past deadline took %" PRId64 " ns\n",
             (int64_t)(after - before));
    }
    return 0;
}

static int test_slack_env_override(void) {
    /* Reset the lazy-init by simulating a fresh process: setenv then
     * teardown+re-init. shutdown/init are documented idempotent. */
    if (setenv("OPENPETE_PACER_SLACK_US", "3300", 1) != 0) {
        FAIL("setenv failed\n");
    }
    op_time_shutdown();
    op_time_init();
    op_time_t got = op_pacer_slack_ns();
    if (got != 3300 * OP_NS_PER_US) {
        FAIL("slack override expected 3300 us, got %" PRId64 " ns\n",
             (int64_t)got);
    }
    unsetenv("OPENPETE_PACER_SLACK_US");
    op_time_shutdown();
    op_time_init();  /* restore defaults for subsequent runs */
    return 0;
}

int main(void) {
    op_time_init();

    if (test_monotonic())                    return 1;
    if (test_sleep_reaches_deadline())       return 1;
    if (test_sleep_in_the_past_returns_fast()) return 1;
    if (test_slack_env_override())           return 1;

    op_time_shutdown();
    printf("PASS openpete_time_test\n");
    return 0;
}
