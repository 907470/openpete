/* tests/unit/openpete_time_test.c - verify openpete_time.c contract:
 *   1. op_now() is monotonically non-decreasing across calls.
 *   2. op_sleep_until(t) wakes at or after t (never undershoots).
 *   3. Overshoot is small on a quiet host (sanity bound, not a hard
 *      SLA - CI runners are noisy, so the bound is generous; the
 *      `pacing_quality` slow target is where we gate real numbers).
 *   4. op_pacer_slack_ns honours OPENPETE_PACER_SLACK_US.
 *   5. The exact NTSC period arithmetic (op_period_t / op_deadline_t):
 *      deadlines land on floor(n * period) forever, 2 VBL == 1 tick, a
 *      multi-step slip advance == the same number of single steps, and
 *      every --record-cfr subdivision reconstitutes the tick exactly.
 *   6. The init group's seal: a re-read before op_seal(time) counts no
 *      failure, one after it counts exactly one, op_time_init after the
 *      seal returns before its check, and op_time_shutdown runs once and
 *      leaves the clock initialized.
 *   7. The virtual clock: op_now returns its value, op_sleep_until moves it
 *      forward to a later deadline and never back.
 *
 * Compiled directly from the .c source (no openpete_core link) per the
 * pattern in psx_region_test.c - keeps the test binary SDL-free. */

#include "../../core/include/openpete_time.h"
#include "../../core/include/openpete_thread_roles.h"

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
    /* Generous overshoot bound - 10 ms - to survive CI scheduler noise.
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

/* setenv/unsetenv are POSIX; MinGW only ships _putenv_s. Use the
 * platform primitive directly so the test stays portable. _putenv_s
 * with an empty value also serves as unset on the Win32 CRT. */
#if defined(_WIN32)
#  include <stdlib.h>
static int env_set(const char* k, const char* v)   { return _putenv_s(k, v); }
static int env_unset(const char* k)                 { return _putenv_s(k, ""); }
#else
static int env_set(const char* k, const char* v)   { return setenv(k, v, 1); }
static int env_unset(const char* k)                 { return unsetenv(k); }
#endif

static int test_slack_env_override(void) {
    /* The knobs are re-read after the configuration is materialized; this
     * runs before the seal, as the options parse does. */
    if (env_set("OPENPETE_PACER_SLACK_US", "3300") != 0) {
        FAIL("env_set failed\n");
    }
    op_time_reload_env();
    op_time_t got = op_pacer_slack_ns();
    if (got != 3300 * OP_NS_PER_US) {
        FAIL("slack override expected 3300 us, got %" PRId64 " ns\n",
             (int64_t)got);
    }
    env_unset("OPENPETE_PACER_SLACK_US");
    op_time_reload_env();   /* a fixed slack returns to adaptive */
    got = op_pacer_slack_ns();
    if (got <= 0 || got > 2500 * OP_NS_PER_US) {
        FAIL("unset slack expected the adaptive default, got %" PRId64 " ns\n",
             (int64_t)got);
    }
    return 0;
}

/* The seal of the init group. Run after every case that re-reads the env. */
static int test_time_seal(void) {
    const unsigned long long f0 = op_role_failures();
    op_time_reload_env();
    if (op_role_failures() != f0)
        FAIL("a re-read before the seal counted %llu failures\n", op_role_failures() - f0);
    op_seal(time);
    if (!op_seal_is_sealed(&op_seal_obj_time))
        FAIL("op_seal(time) did not seal\n");
    op_time_init();   /* initialized: returns before its check */
    if (op_role_failures() != f0)
        FAIL("op_time_init after the seal counted a failure\n");
    op_time_reload_env();
    if (op_role_failures() != f0 + 2)   /* op_time_reload_env and parse_census_env */
        FAIL("a re-read after the seal counted %llu failures, want 2\n",
             op_role_failures() - f0);
    op_time_shutdown();
    op_time_shutdown();   /* runs once */
    op_time_init();       /* still initialized: no write, no failure */
    if (op_role_failures() != f0 + 2)
        FAIL("shutdown then init after the seal counted a failure\n");
    op_time_t a = op_now(), b = op_now();
    if (a <= 0 || b < a) FAIL("op_now after shutdown: %" PRId64 " then %" PRId64 "\n",
                              (int64_t)a, (int64_t)b);
    return 0;
}

/* The virtual clock never turns off, so this case runs last. */
static int test_virtual_clock(void) {
    if (op_virtual_clock_active()) FAIL("virtual clock active before enable\n");
    op_virtual_clock_enable(5 * OP_NS_PER_SEC);
    if (!op_virtual_clock_active()) FAIL("virtual clock not active after enable\n");
    if (op_now() != 5 * OP_NS_PER_SEC)
        FAIL("virtual op_now %" PRId64 ", want 5 s\n", (int64_t)op_now());
    op_sleep_until(6 * OP_NS_PER_SEC);
    if (op_now() != 6 * OP_NS_PER_SEC)
        FAIL("sleep to 6 s left op_now at %" PRId64 "\n", (int64_t)op_now());
    op_sleep_until(5 * OP_NS_PER_SEC + OP_NS_PER_SEC / 2);
    if (op_now() != 6 * OP_NS_PER_SEC)
        FAIL("an earlier deadline moved the clock to %" PRId64 "\n", (int64_t)op_now());
    op_virtual_clock_enable(0);   /* <= 0 starts at one second */
    if (op_now() != OP_NS_PER_SEC)
        FAIL("enable(0) gave %" PRId64 ", want 1 s\n", (int64_t)op_now());
    return 0;
}

/* ---- exact NTSC period arithmetic --------------------------------------
 *
 * The pacer's deadline chain must be the correctly-rounded-down image of the
 * true grid at EVERY step, not merely close on average: a period that sheds
 * its truncated remainder (as the old whole-ns 1001/60000 constant did) walks
 * away from the real rate without bound. */

/* Reference: floor(n * num/den seconds) in ns, computed independently of
 * op_period_t so a bug in the header cannot hide behind itself. Splitting
 * the multiply keeps it exact in 64-bit for any n the test uses. */
static op_time_t ref_deadline_ns(op_time_t num, op_time_t den, uint64_t n) {
    const op_time_t total = num * OP_NS_PER_SEC;   /* ns per period, exact */
    const op_time_t q = total / den, r = total % den;
    return (op_time_t)((uint64_t)q * n + ((uint64_t)r * n) / (uint64_t)den);
}

static int check_grid(const char* what, op_period_t p,
                      op_time_t num, op_time_t den, uint64_t steps) {
    op_deadline_t d;
    op_deadline_anchor(&d, 0);
    for (uint64_t n = 1; n <= steps; ++n) {
        op_deadline_advance(&d, p, 1);
        const op_time_t want = ref_deadline_ns(num, den, n);
        if (d.at != want) {
            FAIL("%s: deadline %" PRIu64 " = %" PRId64 " ns, want %" PRId64 "\n",
                 what, n, (int64_t)d.at, (int64_t)want);
        }
        if (d.acc < 0 || d.acc >= p.den)
            FAIL("%s: accumulator out of range at %" PRIu64 "\n", what, n);
    }
    return 0;
}

static int test_ntsc_periods_are_exact(void) {
    const op_period_t vbl  = OP_VBL_PERIOD();
    const op_period_t tick = OP_TICK_PERIOD();

    /* A VBL is 16715111 + 1/9 ns; a tick is twice that. */
    if (vbl.whole != 16715111 || vbl.rem != 1 || vbl.den != 9)
        FAIL("VBL period is %" PRId64 " + %" PRId64 "/%" PRId64 ", want 16715111 + 1/9\n",
             (int64_t)vbl.whole, (int64_t)vbl.rem, (int64_t)vbl.den);
    if (tick.whole != 33430222 || tick.rem * 9 != 2 * tick.den)
        FAIL("tick period is %" PRId64 " + %" PRId64 "/%" PRId64 ", want 33430222 + 2/9\n",
             (int64_t)tick.whole, (int64_t)tick.rem, (int64_t)tick.den);

    /* One million VBLs is ~4.6 hours of play: long enough that a dropped
     * remainder (0.11 ns/step) would show as a 111 us error. */
    if (check_grid("VBL", vbl, OP_VBL_HZ_DEN, OP_VBL_HZ_NUM, 1000000)) return 1;
    if (check_grid("tick", tick, OP_VBL_HZ_DEN,
                   OP_VBL_HZ_NUM / OP_TICK_VBLS, 1000000)) return 1;

    /* Two VBLs must land exactly on one tick - the sim and the interp
     * denominator would otherwise drift apart. */
    op_deadline_t a, b;
    op_deadline_anchor(&a, 0);
    op_deadline_anchor(&b, 0);
    for (uint64_t n = 0; n < 100000; ++n) {
        op_deadline_advance(&a, vbl, 2);
        op_deadline_advance(&b, tick, 1);
        if (a.at != b.at)
            FAIL("2 VBL != 1 tick at %" PRIu64 ": %" PRId64 " vs %" PRId64 "\n",
                 n, (int64_t)a.at, (int64_t)b.at);
    }
    return 0;
}

/* A multi-step advance must land where the same number of single steps
 * would - the slip clamp advances by `skip` periods at once. */
static int test_multi_step_matches_single(void) {
    const op_period_t p = OP_VBL_PERIOD();
    for (op_time_t skip = 1; skip <= 64; ++skip) {
        op_deadline_t one, many;
        op_deadline_anchor(&one, 0);
        op_deadline_anchor(&many, 0);
        for (op_time_t i = 0; i < skip; ++i) op_deadline_advance(&one, p, 1);
        op_deadline_advance(&many, p, skip);
        if (one.at != many.at || one.acc != many.acc)
            FAIL("skip %" PRId64 ": %" PRId64 "/%" PRId64 " vs %" PRId64 "/%" PRId64 "\n",
                 (int64_t)skip, (int64_t)one.at, (int64_t)one.acc,
                 (int64_t)many.at, (int64_t)many.acc);
    }
    return 0;
}

/* --record-cfr divides a tick into `mult` presents. Every divisor must stay
 * exact, and `mult` of them must reconstitute the tick precisely - otherwise
 * an offline capture's presents-per-tick drifts over a long recording. */
static int test_cfr_subdivisions_are_exact(void) {
    const op_period_t tick = OP_TICK_PERIOD();
    for (op_time_t mult = 1; mult <= 16; ++mult) {
        const op_period_t part = op_period_div(tick, mult);
        op_deadline_t a, b;
        op_deadline_anchor(&a, 0);
        op_deadline_anchor(&b, 0);
        for (uint64_t n = 0; n < 100000; ++n) {
            op_deadline_advance(&a, part, mult);
            op_deadline_advance(&b, tick, 1);
            if (a.at != b.at) {
                FAIL("cfr x%" PRId64 ": %" PRIu64 " tick(s) drifted %" PRId64 " ns\n",
                     (int64_t)mult, n + 1, (int64_t)(a.at - b.at));
            }
        }
    }
    return 0;
}

/* The rate the rest of the codebase quotes must agree with the periods the
 * pacer steps, to well inside a part per billion. */
static int test_rate_constants_agree(void) {
    const double vbl_hz  = op_period_hz(OP_VBL_PERIOD());
    const double tick_hz = op_period_hz(OP_TICK_PERIOD());
    if (vbl_hz < OP_VBL_HZ - 1e-6 || vbl_hz > OP_VBL_HZ + 1e-6)
        FAIL("OP_VBL_HZ %.9f disagrees with the stepped period %.9f\n",
             OP_VBL_HZ, vbl_hz);
    if (tick_hz < OP_TICK_HZ - 1e-6 || tick_hz > OP_TICK_HZ + 1e-6)
        FAIL("OP_TICK_HZ %.9f disagrees with the stepped period %.9f\n",
             OP_TICK_HZ, tick_hz);
    /* Guard the actual hardware figure, so a future edit to the constants
     * cannot quietly reintroduce the interlaced 59.94. */
    if (vbl_hz < 59.8261054 || vbl_hz > 59.8261055)
        FAIL("VBL rate %.7f is not the PSX 240p rate 59.8261054\n", vbl_hz);
    return 0;
}

int main(void) {
    /* A failed seal check is counted, not fatal, in this process. */
    env_unset("OPENPETE_THREAD_ROLE_FATAL");
    op_time_init();

    if (test_ntsc_periods_are_exact())       return 1;
    if (test_multi_step_matches_single())    return 1;
    if (test_cfr_subdivisions_are_exact())   return 1;
    if (test_rate_constants_agree())         return 1;
    if (test_monotonic())                    return 1;
    if (test_sleep_reaches_deadline())       return 1;
    if (test_sleep_in_the_past_returns_fast()) return 1;
    if (test_slack_env_override())           return 1;
    if (test_time_seal())                    return 1;
    if (test_virtual_clock())                return 1;

    printf("PASS openpete_time_test\n");
    return 0;
}
