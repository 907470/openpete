/* sched.c - the scheduler runtime, run inside each forked child.
 *
 * The baton is `running`, guarded by s.mx: a thread runs user code only
 * while running equals its id, and touches scheduler state only then. A
 * thread at a yield records its pending operation and the blocking
 * predicate it waits on, then the yielding thread itself makes the next
 * scheduling decision and hands the baton over. See op_sched.h for the cost
 * model. */
#define _GNU_SOURCE
#include "op_sched_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MAX_THREADS = 32, MAX_EVENTS = 256, MAX_NAMES = 256, SITE_LEN = 96 };

/** What a thread at a yield waits for. */
typedef enum { BLK_NONE, BLK_MUTEX, BLK_COND, BLK_SLEEP, BLK_JOIN, BLK_PRED } blk_kind;

/** One scheduled thread. */
typedef struct thr {
    pthread_t pt;
    pthread_cond_t cv;
    int used, done;
    int (*fn)(void *);
    void *arg;
    int ret;
    const char *name;
    sched_op_kind op;
    const void *obj;
    char site[SITE_LEN];
    blk_kind blk;
    sched_mutex *bm;
    sched_cond *bc;
    int64_t deadline;      /* -1 = none */
    int woken;
    int join_id;
    uint64_t wait_seq;
    double prio;           /* PCT priority */
    int starter;           /* creator waiting for this thread's first yield, or -1 */
    int (*pred)(void *);   /* BLK_PRED: wake predicate */
    void *pred_arg;
    const void *ra;        /* pending operation's code address, for the step hook */
} thr;

/** A pending timed event. */
typedef struct sched_event { int64_t t; void (*fn)(void *); void *arg; } sched_event;

/** Alternative kinds at a scheduling decision. */
typedef enum { ALT_THREAD, ALT_SPURIOUS, ALT_ADVANCE, ALT_TO_DEADLINE } alt_kind;

/** One alternative at a scheduling decision. */
typedef struct alt { alt_kind kind; int val; uint8_t dp, dd; } alt;

static struct {
    pthread_mutex_t mx;
    pthread_cond_t all_done;
    int finished;
    thr t[MAX_THREADS];
    int n;
    int running, cur;
    int64_t clock;
    uint64_t wait_seq;
    long steps;
    const sched_child_plan *plan;
    uint16_t *log;
    int nlog;
    char *trace;
    size_t trace_len, trace_cap;
    uint8_t *rep;          /* choice records, sent with the end record */
    size_t rep_len, rep_cap;
    sched_event ev[MAX_EVENTS];
    int nev;
    struct { const void *p; const char *name; } names[MAX_NAMES];
    int nnames, anon_next;
    uint64_t rng;
    long change_at[16];
    int nchange;
    int active;
    sched_step_hook hook;
    void *hook_arg;
    int in_hook;
    int64_t advanced;
} s;

static __thread int tl_id = -1;

/** @brief Advance a splitmix64 state and return its next value.
 * @param st The state.
 * @return 64 random bits. */
static uint64_t splitmix(uint64_t *st)
{
    uint64_t z = (*st += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/** @brief Draw a uniform double in [0, 1) from the PCT generator.
 * @return The draw. */
static double rng_unit(void) { return (double)(splitmix(&s.rng) >> 11) / 9007199254740992.0; }

int sched_write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w; n -= (size_t)w;
    }
    return 0;
}

/** @brief Append bytes to the buffered choice report; fails the schedule when memory runs out.
 * @param p Bytes.
 * @param n Length. */
static void rep_add(const void *p, size_t n)
{
    if (s.rep_len + n > s.rep_cap) {
        size_t cap = s.rep_cap ? s.rep_cap * 2 : 65536;
        while (cap < s.rep_len + n) cap *= 2;
        uint8_t *nb = realloc(s.rep, cap);
        if (!nb) { fputs("sched: report buffer exhausted\n", stderr); _exit(3); }
        s.rep = nb; s.rep_cap = cap;
    }
    memcpy(s.rep + s.rep_len, p, n);
    s.rep_len += n;
}

/** @brief Append formatted text to the in-memory trace.
 * @param fmt printf format. */
static void trace_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trace_add(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    if (s.trace_len + (size_t)n + 1 > s.trace_cap) {
        size_t cap = s.trace_cap ? s.trace_cap * 2 : 65536;
        while (cap < s.trace_len + (size_t)n + 1) cap *= 2;
        char *nb = realloc(s.trace, cap);
        if (!nb) return;
        s.trace = nb; s.trace_cap = cap;
    }
    memcpy(s.trace + s.trace_len, line, (size_t)n);
    s.trace_len += (size_t)n;
    s.trace[s.trace_len] = 0;
}

/** @brief Return the trace name of an object: its registered name, else obj#k by first appearance.
 * @param p The object.
 * @param buf Scratch for an anonymous name.
 * @param cap Its size.
 * @return The name. */
static const char *obj_name(const void *p, char *buf, size_t cap)
{
    if (!p) return "-";
    for (int i = 0; i < s.nnames; i++)
        if (s.names[i].p == p) return s.names[i].name;
    if (s.nnames < MAX_NAMES) {
        snprintf(buf, cap, "obj#%d", s.anon_next++);
        char *keep = strdup(buf);
        s.names[s.nnames].p = p; s.names[s.nnames].name = keep ? keep : "obj";
        s.nnames++;
        return s.names[s.nnames - 1].name;
    }
    return "obj#?";
}

void sched_name(const void *obj, const char *name)
{
    for (int i = 0; i < s.nnames; i++)
        if (s.names[i].p == obj) { s.names[i].name = name; return; }
    if (s.nnames < MAX_NAMES) { s.names[s.nnames].p = obj; s.names[s.nnames].name = name; s.nnames++; }
}

/** @brief Name a code address as symbol+offset or exe+offset, stable across runs of one binary.
 * @param ra The address.
 * @param out Destination.
 * @param cap Its size. */
static void site_of_ra(const void *ra, char *out, size_t cap)
{
    Dl_info di;
    if (ra && dladdr(ra, &di) && di.dli_fbase) {
        const unsigned long off = (unsigned long)((const char *)ra - (const char *)di.dli_fbase);
        if (di.dli_sname)
            snprintf(out, cap, "%s+0x%lx", di.dli_sname,
                     (unsigned long)((const char *)ra - (const char *)di.dli_saddr));
        else
            snprintf(out, cap, "exe+0x%lx", off);
        return;
    }
    snprintf(out, cap, "?");
}

static const char *const k_op_names[SCHED_OP_KINDS] = {
    "start", "point", "read", "write", "atomic", "lock", "trylock", "unlock", "cond-wait",
    "cond-wake", "signal", "broadcast", "sleep", "clock", "create", "join", "device",
};

/** @brief Format the current choice log as a comma-separated string, trailing defaults dropped.
 * @param out Destination.
 * @param cap Its size. */
static void choices_string(char *out, size_t cap)
{
    int n = s.nlog;
    while (n > 0 && s.log[n - 1] == 0) n--;
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len + 8 < cap; i++)
        len += (size_t)snprintf(out + len, cap - len, i ? ",%u" : "%u", (unsigned)s.log[i]);
    if (n == 0) snprintf(out, cap, "-");
}

/** @brief Write the trace to the plan's trace file (a per-process name the explorer collects), or stderr. */
static void trace_flush(void)
{
    const char *path = s.plan->trace_path;
    if (!path) { if (s.trace && !s.plan->cfg->quiet) fputs(s.trace, stderr); return; }
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.%ld.part", path, (long)getpid());
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    if (s.trace) fputs(s.trace, f);
    fclose(f);
}

/** @brief Send the end record (status, steps, choice count, message, choice string) and exit.
 * @param status SCHED_CHILD_PASS or SCHED_CHILD_FAIL.
 * @param msg Failure message, or "". */
static void child_end(int status, const char *msg) __attribute__((noreturn));
static void child_end(int status, const char *msg)
{
    char ch[4096];
    choices_string(ch, sizeof ch);
    if (status != SCHED_CHILD_PASS || s.plan->trace_always) trace_flush();
    const int fd = s.plan->report_fd;
    if (fd >= 0) {
        const uint8_t tag = SCHED_REC_END;
        const int32_t st = status, nc = s.nlog;
        const int64_t steps = s.steps;
        const uint16_t ml = (uint16_t)strnlen(msg, 511), cl = (uint16_t)strnlen(ch, 4095);
        sched_write_all(fd, s.rep, s.rep_len);
        sched_write_all(fd, &tag, 1); sched_write_all(fd, &st, 4); sched_write_all(fd, &steps, 8);
        sched_write_all(fd, &nc, 4); sched_write_all(fd, &ml, 2); sched_write_all(fd, msg, ml);
        sched_write_all(fd, &cl, 2); sched_write_all(fd, ch, cl);
    }
    fflush(NULL);
    _exit(status);
}

void sched_fail(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    trace_add("FAIL %s\n", msg);
    if (!s.plan->cfg->quiet) {
        char ch[4096];
        choices_string(ch, sizeof ch);
        fprintf(stderr, "sched: FAIL: %s\nsched: choices: %s\n", msg, ch);
    }
    child_end(SCHED_CHILD_FAIL, msg);
}

/** @brief Report whether a thread's pending operation can execute now.
 * @param i Thread id.
 * @return 1 when enabled. */
static int enabled(int i)
{
    const thr *t = &s.t[i];
    if (!t->used || t->done) return 0;
    switch (t->blk) {
    case BLK_NONE:  return 1;
    case BLK_MUTEX: return t->bm->owner < 0 || t->bm->owner == i;
    case BLK_COND:  return t->woken || (t->deadline >= 0 && s.clock >= t->deadline);
    case BLK_SLEEP: return s.clock >= t->deadline;
    case BLK_JOIN:  return s.t[t->join_id].done;
    case BLK_PRED:  return t->pred(t->pred_arg) || (t->deadline >= 0 && s.clock >= t->deadline);
    }
    return 0;
}

/** @brief Return the earliest pending deadline of a blocked thread or event, or -1.
 * @return The instant. */
static int64_t next_deadline(void)
{
    int64_t best = -1;
    for (int i = 0; i < s.n; i++) {
        const thr *t = &s.t[i];
        if (!t->used || t->done || enabled(i)) continue;
        if ((t->blk == BLK_COND || t->blk == BLK_SLEEP || t->blk == BLK_PRED) && t->deadline >= 0 &&
            (best < 0 || t->deadline < best)) best = t->deadline;
    }
    for (int k = 0; k < s.nev; k++)
        if (best < 0 || s.ev[k].t < best) best = s.ev[k].t;
    return best;
}

/** @brief Fire every event whose instant the clock has reached, in time then insertion order. */
static void fire_events(void)
{
    for (;;) {
        int pick = -1;
        for (int k = 0; k < s.nev; k++)
            if (s.ev[k].t <= s.clock && (pick < 0 || s.ev[k].t < s.ev[pick].t)) pick = k;
        if (pick < 0) return;
        const sched_event e = s.ev[pick];
        memmove(&s.ev[pick], &s.ev[pick + 1], (size_t)(s.nev - pick - 1) * sizeof s.ev[0]);
        s.nev--;
        e.fn(e.arg);
    }
}

/** @brief Move the virtual clock to t, failing the schedule past the horizon, and fire due events.
 * @param t The new clock. */
static void clock_to(int64_t t)
{
    if (t > s.clock) {
        trace_add("time %lld -> %lld\n", (long long)s.clock, (long long)t);
        s.clock = t;
    }
    const int64_t h = s.plan->cfg->horizon_ns;
    if (h > 0 && s.clock > h) sched_fail("virtual-time horizon %lld passed (livelock)", (long long)h);
    fire_events();
}

/** @brief Report whether every scheduled thread has finished.
 * @return 1 when all are done. */
static int all_done(void)
{
    for (int i = 0; i < s.n; i++)
        if (s.t[i].used && !s.t[i].done) return 0;
    return 1;
}

/** @brief Collect the alternatives of a scheduling decision.
 * @param a Destination, SCHED_MAX_ALTS entries.
 * @param n_thread Receives the count of thread alternatives.
 * @return The number of alternatives. */
static int collect_alts(alt *a, int *n_thread)
{
    int n = 0;
    const int c = s.cur;
    const int cur_en = c >= 0 && enabled(c);
    if (cur_en) a[n++] = (alt){ ALT_THREAD, c, 0, 0 };
    for (int i = 0; i < s.n && n < SCHED_MAX_ALTS; i++)
        if (i != c && enabled(i)) a[n++] = (alt){ ALT_THREAD, i, (uint8_t)(cur_en ? 1 : 0), 0 };
    *n_thread = n;
    if (n == 0 && next_deadline() >= 0) a[n++] = (alt){ ALT_TO_DEADLINE, 0, 0, 0 };
    if (n == 0) return 0;
    for (int i = 0; i < s.n && n < SCHED_MAX_ALTS; i++)
        if (s.t[i].used && !s.t[i].done && s.t[i].blk == BLK_COND && !enabled(i))
            a[n++] = (alt){ ALT_SPURIOUS, i, 0, 1 };
    const sched_config *cfg = s.plan->cfg;
    for (int k = 0; *n_thread > 0 && k < cfg->n_advance && n < SCHED_MAX_ALTS; k++)
        a[n++] = (alt){ ALT_ADVANCE, k, 0, 1 };
    return n;
}

/** @brief Return the PCT pick among the alternatives: highest-priority thread, or with the deviation probability a uniform deviation.
 * @param a The alternatives.
 * @param n Their count.
 * @param n_thread Thread alternatives at the front.
 * @return The index. */
static int pct_pick(const alt *a, int n, int n_thread)
{
    if (n > n_thread && rng_unit() < s.plan->cfg->pct_deviation)
        return n_thread + (int)(splitmix(&s.rng) % (uint64_t)(n - n_thread));
    int best = 0;
    for (int k = 1; k < n_thread; k++)
        if (s.t[a[k].val].prio > s.t[a[best].val].prio) best = k;
    return best;
}

/** @brief Take one choice: from the prefix, else by PCT or the default, report its alternatives, and log it.
 * @param n Alternatives.
 * @param dp Their preemption costs.
 * @param dd Their deviation costs.
 * @param pct The PCT pick when drawing by PCT.
 * @return The chosen index. */
static int take_choice(int n, const uint8_t *dp, const uint8_t *dd, int pct)
{
    const sched_child_plan *pl = s.plan;
    const int pos = s.nlog;
    if (pos >= SCHED_MAX_CHOICES) sched_fail("choice limit %d reached", SCHED_MAX_CHOICES);
    int idx = 0;
    if (pos < pl->prefix_len) {
        idx = pl->prefix[pos];
        if (idx >= n) sched_fail("choice %d is %d but only %d alternatives exist (stale choice string)", pos, idx, n);
    } else if (pl->pct) {
        idx = pct;
    }
    if (pl->report_fd >= 0 && pos >= pl->prefix_len && !pl->pct) {
        const uint8_t tag = SCHED_REC_CHOICE;
        const uint16_t nn = (uint16_t)n;
        rep_add(&tag, 1);
        rep_add(&nn, 2);
        for (int k = 0; k < n; k++) {
            const uint8_t c[2] = { dp[k], dd[k] };
            rep_add(c, 2);
        }
    }
    s.log[s.nlog++] = (uint16_t)idx;
    return idx;
}

/** @brief Apply PCT's priority change when this decision is a change point. */
static void pct_change_point(void)
{
    if (!s.plan->pct || s.cur < 0) return;
    for (int k = 0; k < s.nchange; k++)
        if (s.change_at[k] == s.steps) s.t[s.cur].prio = -1.0 - k;
}

/** @brief Make one scheduling decision and return the thread whose pending operation runs next.
 *
 * Fails the schedule on a deadlock (no enabled thread and no deadline) or
 * past the step limit. Spurious wakeups and time advances are applied and
 * the decision repeats.
 * @return The thread id. */
static int decide(void)
{
    const long lim = s.plan->cfg->max_steps > 0 ? s.plan->cfg->max_steps : 100000;
    if (++s.steps > lim) sched_fail("step limit %ld passed (livelock)", lim);
    pct_change_point();
    for (;;) {
        alt a[SCHED_MAX_ALTS];
        int n_thread = 0;
        const int n = collect_alts(a, &n_thread);
        if (n == 0) sched_fail("deadlock: no thread enabled and no pending deadline");
        int idx = 0;
        if (n > 1) {
            uint8_t dp[SCHED_MAX_ALTS], dd[SCHED_MAX_ALTS];
            for (int k = 0; k < n; k++) { dp[k] = a[k].dp; dd[k] = a[k].dd; }
            idx = take_choice(n, dp, dd, s.plan->pct ? pct_pick(a, n, n_thread) : 0);
        }
        const alt ch = a[idx];
        if (ch.kind == ALT_THREAD) return ch.val;
        if (ch.kind == ALT_SPURIOUS) { s.t[ch.val].woken = 1; trace_add("spurious-wake T%d\n", ch.val); continue; }
        if (ch.kind == ALT_ADVANCE) {
            s.advanced += s.plan->cfg->advance[ch.val];
            clock_to(s.clock + s.plan->cfg->advance[ch.val]);
            continue;
        }
        clock_to(next_deadline());
    }
}

/** @brief Append the trace line of the operation thread id is about to execute.
 * @param id The thread. */
static void trace_step(int id)
{
    const thr *t = &s.t[id];
    char nb[32];
    trace_add("%ld t=%lld T%d(%s) %s %s @%s\n", s.steps, (long long)s.clock, id,
              t->name ? t->name : "?", k_op_names[t->op], obj_name(t->obj, nb, sizeof nb), t->site);
}

/** @brief Hand the baton to a thread and wake it; s.mx is held.
 * @param next The thread. */
static void hand_to(int next)
{
    s.running = next;
    s.cur = next;
    trace_step(next);
    pthread_cond_signal(&s.t[next].cv);
}

/** @brief Run the step hook, if any, with the scheduler inactive for the caller.
 * @param id The yielding thread.
 * @param kind Its pending operation, or SCHED_OP_KINDS at a finish.
 * @param obj The operation's object.
 * @param ra The operation's code address. */
static void run_hook(int id, sched_op_kind kind, const void *obj, const void *ra)
{
    if (!s.hook || s.in_hook) return;
    s.in_hook = 1;
    s.hook(s.hook_arg, id, kind, obj, ra);
    s.in_hook = 0;
}

void sched_set_step_hook(sched_step_hook fn, void *arg) { s.hook = fn; s.hook_arg = arg; }
long sched_steps(void) { return s.steps; }
int64_t sched_advanced_ns(void) { return s.advanced; }

/** @brief Record the calling thread's pending operation, decide, and run when chosen.
 * @param kind Operation kind.
 * @param obj Object.
 * @param site Site text. */
static void yield_op(sched_op_kind kind, const void *obj, const char *site, const void *ra)
{
    const int me = tl_id;
    thr *t = &s.t[me];
    t->op = kind; t->obj = obj; t->ra = ra;
    snprintf(t->site, sizeof t->site, "%s", site ? site : "?");
    if (t->starter < 0) run_hook(me, kind, obj, ra);
    pthread_mutex_lock(&s.mx);
    if (t->starter >= 0) {
        /* The first yield after creation: the pending operation is now
         * known; give the baton back to the creator without a decision. */
        const int st = t->starter;
        t->starter = -1;
        s.running = st; s.cur = st;
        pthread_cond_signal(&s.t[st].cv);
    } else {
        s.cur = me;
        hand_to(decide());
    }
    while (s.running != me) pthread_cond_wait(&t->cv, &s.mx);
    pthread_mutex_unlock(&s.mx);
}

/** @brief Yield with a site named by a code address.
 * @param kind Operation kind.
 * @param obj Object.
 * @param ra Code address. */
static void yield_ra(sched_op_kind kind, const void *obj, const void *ra)
{
    char site[SITE_LEN];
    site_of_ra(ra, site, sizeof site);
    yield_op(kind, obj, site, ra);
}

int sched_active(void) { return s.active && tl_id >= 0 && !s.in_hook; }
int sched_self(void) { return tl_id; }
int64_t sched_now(void) { return s.clock; }

void sched_point(sched_op_kind kind, const void *obj, const char *site)
{
    if (!sched_active()) return;
    yield_op(kind, obj, site, NULL);
}

void sched_point_ra(sched_op_kind kind, const void *obj, const void *ra)
{
    if (!sched_active()) return;
    yield_ra(kind, obj, ra);
}

int sched_choose(int n, const char *site)
{
    if (n <= 1 || !sched_active()) return 0;
    uint8_t dp[SCHED_MAX_ALTS] = {0}, dd[SCHED_MAX_ALTS];
    if (n > SCHED_MAX_ALTS) n = SCHED_MAX_ALTS;
    for (int k = 0; k < n; k++) dd[k] = k ? 1 : 0;
    int pct = 0;
    if (s.plan->pct && rng_unit() < s.plan->cfg->pct_deviation) pct = 1 + (int)(splitmix(&s.rng) % (uint64_t)(n - 1));
    const int idx = take_choice(n, dp, dd, pct);
    trace_add("choose %s = %d/%d\n", site ? site : "?", idx, n);
    return idx;
}

int64_t sched_clock_read(const void *ra)
{
    if (sched_active()) yield_ra(SCHED_OP_CLOCK, NULL, ra);
    return s.clock;
}

void sched_sleep_until(int64_t t, const void *ra)
{
    if (!sched_active()) return;
    thr *me = &s.t[tl_id];
    me->blk = BLK_SLEEP; me->deadline = t;
    yield_ra(SCHED_OP_SLEEP, NULL, ra);
    me->blk = BLK_NONE; me->deadline = -1;
}

int sched_wait_pred(int (*pred)(void *), void *arg, int64_t deadline, const void *obj, const void *ra)
{
    if (!sched_active()) return pred(arg);
    thr *me = &s.t[tl_id];
    me->blk = BLK_PRED; me->pred = pred; me->pred_arg = arg; me->deadline = deadline;
    yield_ra(SCHED_OP_DEVICE, obj, ra);
    const int ok = pred(arg);
    me->blk = BLK_NONE; me->pred = NULL; me->pred_arg = NULL; me->deadline = -1;
    return ok;
}

void sched_mutex_init(sched_mutex *m, const char *name)
{
    m->owner = -1; m->depth = 0; m->name = name;
    if (name) sched_name(m, name);
}

/** @brief Block until the mutex is free or held by the caller, then take depth levels of it.
 * @param m The mutex.
 * @param depth Recursion levels to add.
 * @param ra Code address. */
static void mutex_take(sched_mutex *m, int depth, const void *ra)
{
    thr *me = &s.t[tl_id];
    me->blk = BLK_MUTEX; me->bm = m;
    yield_ra(SCHED_OP_LOCK, m, ra);
    me->blk = BLK_NONE; me->bm = NULL;
    if (m->owner == tl_id) m->depth += depth;
    else { m->owner = tl_id; m->depth = depth; }
}

void sched_mutex_lock(sched_mutex *m, const void *ra)
{
    if (!sched_active()) return;
    mutex_take(m, 1, ra);
}

int sched_mutex_trylock(sched_mutex *m, const void *ra)
{
    if (!sched_active()) return 1;
    yield_ra(SCHED_OP_TRYLOCK, m, ra);
    if (m->owner >= 0 && m->owner != tl_id) return 0;
    if (m->owner == tl_id) m->depth++;
    else { m->owner = tl_id; m->depth = 1; }
    return 1;
}

void sched_mutex_unlock(sched_mutex *m, const void *ra)
{
    if (!sched_active()) return;
    yield_ra(SCHED_OP_UNLOCK, m, ra);
    if (m->owner != tl_id) sched_fail("unlock of a mutex T%d does not hold (owner T%d)", tl_id, m->owner);
    if (--m->depth == 0) m->owner = -1;
}

int sched_cond_wait(sched_cond *c, sched_mutex *m, int64_t timeout_ns, const void *ra)
{
    if (!sched_active()) return 1;
    thr *me = &s.t[tl_id];
    yield_ra(SCHED_OP_COND_WAIT, c, ra);
    if (m->owner != tl_id) sched_fail("condition wait without its mutex (T%d, owner T%d)", tl_id, m->owner);
    const int depth = m->depth;
    m->owner = -1; m->depth = 0;
    me->blk = BLK_COND; me->bc = c; me->woken = 0; me->wait_seq = ++s.wait_seq;
    me->deadline = timeout_ns >= 0 ? s.clock + timeout_ns : -1;
    yield_ra(SCHED_OP_COND_WAKE, c, ra);
    const int signaled = me->woken;
    me->blk = BLK_NONE; me->bc = NULL; me->woken = 0; me->deadline = -1;
    mutex_take(m, depth, ra);
    return signaled;
}

/** @brief Wake waiters of a condition: the longest-waiting one, or all.
 * @param c The condition.
 * @param all Nonzero wakes every waiter. */
static void cond_wake(sched_cond *c, int all)
{
    for (;;) {
        int pick = -1;
        for (int i = 0; i < s.n; i++) {
            const thr *t = &s.t[i];
            if (t->used && !t->done && t->blk == BLK_COND && t->bc == c && !t->woken &&
                (pick < 0 || t->wait_seq < s.t[pick].wait_seq)) pick = i;
        }
        if (pick < 0) return;
        s.t[pick].woken = 1;
        if (!all) return;
    }
}

void sched_cond_signal(sched_cond *c, const void *ra)
{
    if (!sched_active()) return;
    yield_ra(SCHED_OP_SIGNAL, c, ra);
    cond_wake(c, 0);
}

void sched_cond_broadcast(sched_cond *c, const void *ra)
{
    if (!sched_active()) return;
    yield_ra(SCHED_OP_BROADCAST, c, ra);
    cond_wake(c, 1);
}

/** @brief Mark the calling thread finished and hand the baton on, or flag the run complete. */
static void thread_finish(void)
{
    const int me = tl_id;
    if (s.t[me].starter < 0) run_hook(me, SCHED_OP_KINDS, NULL, NULL);
    pthread_mutex_lock(&s.mx);
    s.t[me].done = 1;
    s.cur = me;
    if (s.t[me].starter >= 0) {
        const int st = s.t[me].starter;
        s.t[me].starter = -1;
        s.running = st; s.cur = st;
        pthread_cond_signal(&s.t[st].cv);
    } else if (all_done()) {
        s.finished = 1;
        pthread_cond_broadcast(&s.all_done);
    } else {
        hand_to(decide());
    }
    pthread_mutex_unlock(&s.mx);
}

/** @brief Body of every scheduled pthread: wait for the baton, run the thread function, finish.
 * @param p The thread's id, as a pointer-sized integer.
 * @return NULL. */
static void *thread_trampoline(void *p)
{
    const int id = (int)(intptr_t)p;
    tl_id = id;
    pthread_mutex_lock(&s.mx);
    while (s.running != id) pthread_cond_wait(&s.t[id].cv, &s.mx);
    pthread_mutex_unlock(&s.mx);
    s.t[id].ret = s.t[id].fn(s.t[id].arg);
    thread_finish();
    return NULL;
}

/** @brief Claim a thread slot and initialise it.
 * @param name Trace name.
 * @return The id, or -1 when full. */
static int thread_slot(const char *name)
{
    if (s.n >= MAX_THREADS) return -1;
    const int id = s.n++;
    thr *t = &s.t[id];
    memset(t, 0, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
    t->used = 1; t->name = name; t->deadline = -1; t->op = SCHED_OP_START;
    snprintf(t->site, sizeof t->site, "start");
    t->starter = -1;
    t->prio = rng_unit() * 1000.0;   /* PCT: random initial priority */
    return id;
}

int sched_thread_create(int (*fn)(void *), void *arg, const char *name, const void *ra)
{
    if (!sched_active()) return -1;
    (void)ra;
    /* Creation is not a yield: no other thread can observe it before the
     * new thread's first operation. The new thread runs at once to its first
     * yield, so its pending operation is a visible one, then the creator
     * continues. */
    const int me = tl_id;
    const int id = thread_slot(name);
    if (id < 0) sched_fail("thread limit %d reached", MAX_THREADS);
    s.t[id].fn = fn; s.t[id].arg = arg; s.t[id].starter = me;
    trace_add("create T%d(%s) by T%d\n", id, name ? name : "?", me);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&s.t[id].pt, &at, thread_trampoline, (void *)(intptr_t)id) != 0)
        sched_fail("pthread_create failed");
    pthread_attr_destroy(&at);
    pthread_mutex_lock(&s.mx);
    s.running = id;
    pthread_cond_signal(&s.t[id].cv);
    while (s.running != me) pthread_cond_wait(&s.t[me].cv, &s.mx);
    pthread_mutex_unlock(&s.mx);
    return id;
}

int sched_thread_join(int id, const void *ra)
{
    if (!sched_active() || id < 0 || id >= s.n) return 0;
    thr *me = &s.t[tl_id];
    me->blk = BLK_JOIN; me->join_id = id;
    yield_ra(SCHED_OP_JOIN, &s.t[id], ra);
    me->blk = BLK_NONE;
    return s.t[id].ret;
}

void sched_at(int64_t t, void (*fn)(void *), void *arg)
{
    if (s.nev >= MAX_EVENTS) sched_fail("event limit %d reached", MAX_EVENTS);
    s.ev[s.nev++] = (sched_event){ t, fn, arg };
}

/** @brief Draw the PCT priority change points: depth-1 step indices in [1, pct_steps]. */
static void pct_setup(void)
{
    const sched_config *cfg = s.plan->cfg;
    const int depth = cfg->pct_depth > 0 ? cfg->pct_depth : 3;
    const int range = cfg->pct_steps > 0 ? cfg->pct_steps : 200;
    s.nchange = depth - 1 < 16 ? depth - 1 : 16;
    for (int k = 0; k < s.nchange; k++) s.change_at[k] = 1 + (long)(splitmix(&s.rng) % (uint64_t)range);
}

void sched_child_run(const sched_child_plan *plan)
{
    s.plan = plan;
    s.rng = plan->pct_seed ? plan->pct_seed : 1;
    pthread_mutex_init(&s.mx, NULL);
    pthread_cond_init(&s.all_done, NULL);
    s.log = calloc(SCHED_MAX_CHOICES, sizeof *s.log);
    if (!s.log) _exit(3);
    s.clock = plan->cfg->start_ns > 0 ? plan->cfg->start_ns : 1000000000;
    if (plan->pct) pct_setup();
    const int id = thread_slot("main");
    tl_id = id;
    s.running = id; s.cur = id;
    s.active = 1;
    plan->cfg->scenario(plan->cfg->arg);
    thread_finish();
    pthread_mutex_lock(&s.mx);
    while (!s.finished) pthread_cond_wait(&s.all_done, &s.mx);
    pthread_mutex_unlock(&s.mx);
    child_end(SCHED_CHILD_PASS, "");
}
