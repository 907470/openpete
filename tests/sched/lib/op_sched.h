/* op_sched.h - deterministic scheduler for interleaving tests.
 *
 * Real OS threads run one at a time under a baton. Each visible operation
 * (a lock, a condition wait or signal, a sleep, a clock read, a watched
 * memory access, a modelled device call) is preceded by a yield, at which
 * the scheduler picks the thread whose pending operation executes next. A
 * thread runs without interruption between two yields.
 *
 * Every scheduling decision with more than one alternative, and every data
 * choice (sched_choose), is a numbered choice. Alternative 0 is the default
 * and costs nothing; another thread while the current one is enabled costs
 * one preemption; a data choice other than 0, a spurious condition wakeup
 * and a time advance while a thread is enabled cost one deviation. A run is
 * named by its choice string (the chosen alternative of each choice, in
 * order); the explorer enumerates choice strings under a joint preemption
 * and deviation bound, samples them by PCT, or replays one.
 *
 * Each schedule runs in a forked child process, so all program state starts
 * identical for every schedule. The explorer forks before any scheduled
 * thread exists.
 */
#ifndef OPENPETE_SCHED_H
#define OPENPETE_SCHED_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Kinds of visible operation, printed in the trace. */
typedef enum {
    SCHED_OP_START, SCHED_OP_POINT, SCHED_OP_READ, SCHED_OP_WRITE, SCHED_OP_ATOMIC,
    SCHED_OP_LOCK, SCHED_OP_TRYLOCK, SCHED_OP_UNLOCK, SCHED_OP_COND_WAIT, SCHED_OP_COND_WAKE,
    SCHED_OP_SIGNAL, SCHED_OP_BROADCAST, SCHED_OP_SLEEP, SCHED_OP_CLOCK, SCHED_OP_CREATE,
    SCHED_OP_JOIN, SCHED_OP_DEVICE, SCHED_OP_KINDS
} sched_op_kind;

/** A scheduler mutex: owner thread id, or -1 when free. */
typedef struct sched_mutex { int owner; int depth; const char *name; } sched_mutex;

/** A scheduler condition: the waiters are the threads blocked on it. */
typedef struct sched_cond { const char *name; } sched_cond;

/** Exploration mode. */
typedef enum { SCHED_MODE_DFS, SCHED_MODE_PCT, SCHED_MODE_REPLAY } sched_mode;

/** Configuration of one exploration. */
typedef struct sched_config {
    /** The scenario, run on scheduled thread 0 inside each forked child. */
    void (*scenario)(void *arg);
    void *arg;
    sched_mode mode;
    int max_preemptions;   /**< DFS: highest preemption bound p */
    int max_deviations;    /**< DFS: highest deviation bound d */
    int jobs;              /**< worker processes; 0 or 1 runs one child at a time */
    long max_schedules;    /**< stop after this many schedules; 0 = no limit */
    int stop_on_fail;      /**< stop at the first failing schedule */
    uint64_t seed;         /**< PCT: base seed */
    long pct_runs;         /**< PCT: schedules to run */
    int pct_depth;         /**< PCT: priority-change depth (default 3) */
    double pct_deviation;  /**< PCT: probability of a non-default data choice (default 0.05) */
    int pct_steps;         /**< PCT: step range the change points are drawn from (default 200) */
    const char *replay;    /**< REPLAY: the choice string */
    const char *trace_path;/**< failing (or replayed) schedule's trace; NULL = stderr */
    int64_t start_ns;      /**< virtual clock at the start (default 1e9) */
    int64_t horizon_ns;    /**< virtual time a schedule may not pass; 0 = none */
    long max_steps;        /**< steps a schedule may not pass; 0 = 100000 */
    const int64_t *advance; /**< time advances offered while a thread is enabled (deviations) */
    int n_advance;
    int quiet;             /**< no per-failure output on stderr */
} sched_config;

/** Outcome of an exploration. */
typedef struct sched_result {
    long schedules;        /**< schedules run */
    long failures;         /**< failing schedules */
    long pruned;           /**< branches beyond the bound */
    long steps_total, steps_max;
    long choices_max;      /**< longest choice string */
    int found;             /**< a schedule failed */
    int found_p, found_d;  /**< the bound pair being explored when the first failure was found */
    int reached_p, reached_d; /**< the last bound pair completed */
    int complete;          /**< every schedule under the bound was run */
    char choices[4096];    /**< the first failing schedule's choice string */
    char message[512];     /**< its failure message */
} sched_result;

/** @brief Run an exploration and fill the result.
 * @param cfg The configuration.
 * @param out The outcome.
 * @return 0 when no schedule failed, 1 when one did, 2 on an explorer error. */
int sched_explore(const sched_config *cfg, sched_result *out);

/** @brief Print the engagement line of a result on stdout.
 * @param label Name of the scenario.
 * @param r The outcome. */
void sched_print_result(const char *label, const sched_result *r);

/* ---- called from scheduled code ---- */

/** @brief Yield before a visible operation that never blocks.
 * @param kind The operation kind.
 * @param obj The object it touches, or NULL.
 * @param site The call site, printed in the trace. */
void sched_point(sched_op_kind kind, const void *obj, const char *site);

/** @brief Yield before a visible operation, naming the site by a return address.
 * @param kind The operation kind.
 * @param obj The object it touches, or NULL.
 * @param ra A code address in the caller, resolved to an executable offset. */
void sched_point_ra(sched_op_kind kind, const void *obj, const void *ra);

/** @brief Make a data choice among n alternatives; 0 is the default.
 * @param n Number of alternatives (1 returns 0 without a choice).
 * @param site The choice's name, printed in the trace.
 * @return The chosen alternative. */
int sched_choose(int n, const char *site);

/** @brief Make a data choice among n alternatives, uniform under PCT.
 *
 * Under PCT every alternative is equally likely (sched_choose takes a
 * non-default one only with probability pct_deviation); under DFS and replay
 * it is sched_choose. For a scenario that draws its own client sequence.
 * @param n Alternatives (at most SCHED_MAX_ALTS are offered).
 * @param site Trace name of the choice.
 * @return The chosen index, 0 when unscheduled. */
int sched_choose_uniform(int n, const char *site);

/** @brief Report whether the calling thread is scheduled (inside a child's run).
 * @return 1 when it is, else 0. */
int sched_active(void);

/** @brief Return the calling thread's scheduler id, or -1 when unscheduled.
 * @return The id. */
int sched_self(void);

/** @brief Return the virtual clock in nanoseconds, without a yield.
 * @return The clock. */
int64_t sched_now(void);

/** @brief Yield before a clock read and return the virtual clock.
 * @param ra The caller's code address.
 * @return The clock after the yield. */
int64_t sched_clock_read(const void *ra);

/** @brief Block the calling thread until the virtual clock reaches t.
 * @param t Deadline.
 * @param ra The caller's code address. */
void sched_sleep_until(int64_t t, const void *ra);

/** @brief Initialise a mutex.
 * @param m The mutex.
 * @param name Its trace name, or NULL. */
void sched_mutex_init(sched_mutex *m, const char *name);

/** @brief Lock a mutex, blocking while another thread holds it; recursive for its owner.
 * @param m The mutex.
 * @param ra The caller's code address. */
void sched_mutex_lock(sched_mutex *m, const void *ra);

/** @brief Try to lock a mutex without blocking.
 * @param m The mutex.
 * @param ra The caller's code address.
 * @return 1 when locked, else 0. */
int sched_mutex_trylock(sched_mutex *m, const void *ra);

/** @brief Unlock a mutex the calling thread holds; an unheld unlock fails the schedule.
 * @param m The mutex.
 * @param ra The caller's code address. */
void sched_mutex_unlock(sched_mutex *m, const void *ra);

/** @brief Wait on a condition with its mutex held, up to timeout_ns (negative = untimed).
 *
 * Releases the mutex, blocks until a signal, the timeout or a spurious
 * wakeup (a deviation), then relocks the mutex.
 * @param c The condition.
 * @param m The mutex, held by the caller.
 * @param timeout_ns The timeout, or a negative value for none.
 * @param ra The caller's code address.
 * @return 1 when woken by a signal or spuriously, 0 on timeout. */
int sched_cond_wait(sched_cond *c, sched_mutex *m, int64_t timeout_ns, const void *ra);

/** @brief Wake the longest-waiting thread blocked on a condition, if any.
 * @param c The condition.
 * @param ra The caller's code address. */
void sched_cond_signal(sched_cond *c, const void *ra);

/** @brief Wake every thread blocked on a condition.
 * @param c The condition.
 * @param ra The caller's code address. */
void sched_cond_broadcast(sched_cond *c, const void *ra);

/** @brief Create a scheduled thread; it starts at its first scheduling.
 * @param fn Thread body.
 * @param arg Its argument.
 * @param name Trace name.
 * @param ra The caller's code address.
 * @return The new thread's id, or -1 when the scheduler is full. */
int sched_thread_create(int (*fn)(void *), void *arg, const char *name, const void *ra);

/** @brief Block until a scheduled thread has finished.
 * @param id The thread's id.
 * @param ra The caller's code address.
 * @return The thread body's return value. */
int sched_thread_join(int id, const void *ra);

/** @brief Run fn(arg) on the scheduling thread when the virtual clock reaches t.
 * @param t The instant.
 * @param fn The event; it runs between steps and must not yield.
 * @param arg Its argument. */
void sched_at(int64_t t, void (*fn)(void *), void *arg);

/** @brief Give an object a trace name.
 * @param obj The object.
 * @param name The name (kept by pointer). */
void sched_name(const void *obj, const char *name);

/** @brief Fail the current schedule: print the message, the choice string and the trace, and end the child.
 * @param fmt printf format. */
void sched_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

/** @brief Register an address range whose instrumented accesses become yields.
 *
 * The harness compiles the code under test with -fsanitize=thread and links
 * its own __tsan_* entry points (tsan_hooks.c); an access whose address
 * lies in a registered range yields before it, every other access runs
 * through.
 * @param addr Start of the range.
 * @param len Its length in bytes.
 * @param name Trace name. */
void sched_watch(const void *addr, size_t len, const char *name);

/** @brief Return the number of instrumented accesses seen and the number that hit a watched range.
 * @param hits Receives the watched hits; may be NULL.
 * @return Every access seen. */
unsigned long sched_watch_counts(unsigned long *hits);

/** @brief Block the calling thread until pred(arg) holds or the virtual clock reaches deadline (a yield).
 *
 * The scheduler evaluates pred at every decision while the thread waits, so
 * pred must only read state that scheduled code or timed events change. Used
 * by modelled blocking device calls.
 * @param pred The wake predicate.
 * @param arg Its argument.
 * @param deadline Timeout instant, or a negative value for none.
 * @param obj The object waited on, for the trace.
 * @param ra The caller's code address.
 * @return 1 when pred held at the wake, 0 on timeout. */
int sched_wait_pred(int (*pred)(void *), void *arg, int64_t deadline, const void *obj, const void *ra);

/** Hook run at every yield with the yielding thread's pending operation, before the decision. */
typedef void (*sched_step_hook)(void *arg, int thread, sched_op_kind kind, const void *obj, const void *ra);

/** @brief Install a hook run at every yield before the scheduling decision, and once when a thread finishes.
 *
 * The hook runs on the yielding thread with the scheduler inactive for it:
 * a seam reached inside the hook runs straight through. It may call
 * sched_fail. A finish calls it with kind SCHED_OP_KINDS.
 * @param fn The hook, or NULL.
 * @param arg Its argument. */
void sched_set_step_hook(sched_step_hook fn, void *arg);

/** Hook run by sched_fail on its formatted message, which it may rewrite in place. */
typedef void (*sched_fail_hook)(char *msg, size_t cap);

/** @brief Install a hook that may rewrite a failure message before it is traced and reported.
 *
 * The scheduler's own failures (a deadlock, a passed horizon or step limit)
 * carry no property; the hook lets the code under test attribute them to
 * one. It runs on the failing thread, at most once per failure.
 * @param fn The hook, or NULL. */
void sched_set_fail_hook(sched_fail_hook fn);

/** @brief Return the virtual time the schedule has advanced by deviation (a time advance while a thread was enabled).
 * @return Nanoseconds. */
int64_t sched_advanced_ns(void);

/** @brief Return the number of the current step of the schedule.
 * @return The step count. */
long sched_steps(void);

/** @brief Register an address range whose instrumented accesses yield unless the calling thread holds a guard mutex.
 *
 * As sched_watch, with *guard_slot read at each access: an access made while
 * the calling thread owns that mutex (an SDL_Mutex created by the scheduler
 * wraps) runs through and counts as guarded. A NULL slot or a NULL mutex
 * guards nothing.
 * @param addr Start of the range.
 * @param len Its length in bytes.
 * @param name Trace name.
 * @param guard_slot Where the guarding mutex pointer is stored, or NULL. */
void sched_watch_guarded(const void *addr, size_t len, const char *name, void *const *guard_slot);

/** @brief Register an address range whose instrumented accesses yield unless a predicate names the calling thread the owner of the address.
 *
 * As sched_watch, with owned(addr, thread) asked at each access: an access
 * by the thread the protocol makes the address's single owner at that
 * moment runs through and counts as guarded.
 * @param addr Start of the range.
 * @param len Its length in bytes.
 * @param name Trace name.
 * @param owned The ownership predicate. */
void sched_watch_owned(const void *addr, size_t len, const char *name, int (*owned)(const void *addr, int thread));

/** @brief Exclude an address range from every watched range: its accesses run through.
 * @param addr Start of the range.
 * @param len Its length in bytes. */
void sched_watch_exclude(const void *addr, size_t len);

/** @brief Start or stop recording the address of the first instrumented atomic store.
 *
 * Used before the first fork to find an atomic the harness cannot name.
 * @param on Nonzero starts (and clears the recording), zero stops.
 * @return The address recorded since the last start, or NULL. */
const void *sched_watch_probe(int on);

/** @brief Return the instrumented accesses that hit a watched range but ran through under its guard mutex.
 * @return The count. */
unsigned long sched_watch_guarded_count(void);

/** @brief Admit a thread name for the SDL_CreateThread wrap; any other name fails the schedule (sdl_wraps.c).
 * @param name The SDL thread name (kept by pointer). */
void sched_sdl_admit(const char *name);

#ifdef __cplusplus
}
#endif
#endif
