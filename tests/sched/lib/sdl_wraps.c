/* sdl_wraps.c - SDL synchronisation, thread and clock calls over the
 * scheduler, linked with -Wl,--wrap=<symbol> for every symbol in
 * SCHED_SDL_WRAPS (tests/sched/CMakeLists.txt).
 *
 * Mutexes, conditions and threads are scheduler objects behind SDL's
 * opaque handles. Every call is a yield inside a scheduled run and runs
 * straight through outside one (the explorer process before its first
 * fork is single-threaded). Clock reads and delays use the virtual clock.
 * A thread is created only when the scenario admitted its name. The SDL
 * atomics publish SDL's sequentially consistent ordering to the race
 * detector of the noise build (sched_tsan_acquire, sched_tsan_release). */
#include "op_sched.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

/** Handle of an unscheduled caller for SDL_GetCurrentThreadID. */
#define SCHED_UNSCHEDULED_TID 0x7ffffu

static const char *s_admit[32];
static int s_nadmit;

/** @brief Admit a thread name for SDL_CreateThreadRuntime; any other name fails the schedule.
 * @param name The SDL thread name (kept by pointer). */
void sched_sdl_admit(const char *name)
{
    if (s_nadmit < 32) s_admit[s_nadmit++] = name;
}

/** @brief Report whether a thread name was admitted.
 * @param name The name.
 * @return 1 when admitted. */
static int admitted(const char *name)
{
    for (int i = 0; i < s_nadmit; i++)
        if (name && !strcmp(s_admit[i], name)) return 1;
    return 0;
}

SDL_Mutex *__wrap_SDL_CreateMutex(void);
/** @brief Create a scheduler mutex behind SDL's handle.
 * @return The handle, or NULL when allocation fails. */
SDL_Mutex *__wrap_SDL_CreateMutex(void)
{
    sched_mutex *m = malloc(sizeof *m);
    if (m) sched_mutex_init(m, NULL);
    return (SDL_Mutex *)m;
}

void __wrap_SDL_DestroyMutex(SDL_Mutex *m);
/** @brief Free a scheduler mutex; destroying a held one fails the schedule.
 * @param m The handle, or NULL. */
void __wrap_SDL_DestroyMutex(SDL_Mutex *m)
{
    if (m && ((sched_mutex *)m)->owner >= 0 && sched_active()) sched_fail("SDL_DestroyMutex on a held mutex");
    free(m);
}

void __wrap_SDL_LockMutex(SDL_Mutex *m);
/** @brief Lock a scheduler mutex (a yield; blocks while another thread holds it).
 * @param m The handle; NULL does nothing, as SDL does. */
void __wrap_SDL_LockMutex(SDL_Mutex *m)
{
    if (m) sched_mutex_lock((sched_mutex *)m, __builtin_return_address(0));
}

bool __wrap_SDL_TryLockMutex(SDL_Mutex *m);
/** @brief Try to lock a scheduler mutex (a yield).
 * @param m The handle; NULL succeeds, as SDL does.
 * @return true when locked. */
bool __wrap_SDL_TryLockMutex(SDL_Mutex *m)
{
    return !m || sched_mutex_trylock((sched_mutex *)m, __builtin_return_address(0));
}

void __wrap_SDL_UnlockMutex(SDL_Mutex *m);
/** @brief Unlock a scheduler mutex (a yield).
 * @param m The handle; NULL does nothing. */
void __wrap_SDL_UnlockMutex(SDL_Mutex *m)
{
    if (m) sched_mutex_unlock((sched_mutex *)m, __builtin_return_address(0));
}

SDL_Condition *__wrap_SDL_CreateCondition(void);
/** @brief Create a scheduler condition behind SDL's handle.
 * @return The handle, or NULL when allocation fails. */
SDL_Condition *__wrap_SDL_CreateCondition(void)
{
    return (SDL_Condition *)calloc(1, sizeof(sched_cond));
}

void __wrap_SDL_DestroyCondition(SDL_Condition *c);
/** @brief Free a scheduler condition.
 * @param c The handle, or NULL. */
void __wrap_SDL_DestroyCondition(SDL_Condition *c) { free(c); }

void __wrap_SDL_SignalCondition(SDL_Condition *c);
/** @brief Signal a scheduler condition (a yield).
 * @param c The handle; NULL does nothing. */
void __wrap_SDL_SignalCondition(SDL_Condition *c)
{
    if (c) sched_cond_signal((sched_cond *)c, __builtin_return_address(0));
}

void __wrap_SDL_BroadcastCondition(SDL_Condition *c);
/** @brief Broadcast a scheduler condition (a yield).
 * @param c The handle; NULL does nothing. */
void __wrap_SDL_BroadcastCondition(SDL_Condition *c)
{
    if (c) sched_cond_broadcast((sched_cond *)c, __builtin_return_address(0));
}

bool __wrap_SDL_WaitConditionTimeout(SDL_Condition *c, SDL_Mutex *m, Sint32 ms);
/** @brief Wait on a scheduler condition up to ms milliseconds of virtual time (negative = untimed).
 * @param c The condition.
 * @param m Its mutex, held.
 * @param ms Timeout in milliseconds, or negative.
 * @return true when signaled (or woken spuriously), false on timeout. */
bool __wrap_SDL_WaitConditionTimeout(SDL_Condition *c, SDL_Mutex *m, Sint32 ms)
{
    if (!c || !m) return false;
    const int64_t to = ms < 0 ? -1 : (int64_t)ms * 1000000;
    return sched_cond_wait((sched_cond *)c, (sched_mutex *)m, to, __builtin_return_address(0)) != 0;
}

void __wrap_SDL_WaitCondition(SDL_Condition *c, SDL_Mutex *m);
/** @brief Wait on a scheduler condition without a timeout.
 * @param c The condition.
 * @param m Its mutex, held. */
void __wrap_SDL_WaitCondition(SDL_Condition *c, SDL_Mutex *m)
{
    if (c && m) (void)sched_cond_wait((sched_cond *)c, (sched_mutex *)m, -1, __builtin_return_address(0));
}

SDL_Thread *__wrap_SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data,
                                           SDL_FunctionPointer begin, SDL_FunctionPointer end);
/** @brief Create a scheduled thread for an admitted name; any other name fails the schedule.
 * @param fn Thread body.
 * @param name SDL thread name.
 * @param data Its argument.
 * @param begin Unused.
 * @param end Unused.
 * @return The handle (scheduler id + 1). */
SDL_Thread *__wrap_SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data,
                                           SDL_FunctionPointer begin, SDL_FunctionPointer end)
{
    (void)begin; (void)end;
    if (!sched_active()) sched_fail("SDL_CreateThread(\"%s\") outside a scheduled run", name ? name : "");
    if (!admitted(name)) sched_fail("SDL_CreateThread(\"%s\"): thread not admitted by the scenario", name ? name : "");
    const int id = sched_thread_create(fn, data, name, __builtin_return_address(0));
    return (SDL_Thread *)(intptr_t)(id + 1);
}

void __wrap_SDL_WaitThread(SDL_Thread *t, int *status);
/** @brief Join a scheduled thread (a blocking yield).
 * @param t The handle.
 * @param status Receives the body's return value; may be NULL. */
void __wrap_SDL_WaitThread(SDL_Thread *t, int *status)
{
    if (!t) return;
    const int r = sched_thread_join((int)(intptr_t)t - 1, __builtin_return_address(0));
    if (status) *status = r;
}

void __wrap_SDL_DetachThread(SDL_Thread *t);
/** @brief Detaching a scheduled thread is refused: every thread a scenario starts is joined.
 * @param t The handle. */
void __wrap_SDL_DetachThread(SDL_Thread *t)
{
    (void)t;
    sched_fail("SDL_DetachThread: scheduled threads are joined");
}

SDL_ThreadID __wrap_SDL_GetCurrentThreadID(void);
/** @brief Return the scheduler id + 1 of the caller, a fixed id outside a run.
 * @return The id, never 0. */
SDL_ThreadID __wrap_SDL_GetCurrentThreadID(void)
{
    const int id = sched_self();
    return id >= 0 ? (SDL_ThreadID)(id + 1) : SCHED_UNSCHEDULED_TID;
}

Uint64 __wrap_SDL_GetTicksNS(void);
/** @brief Read the virtual clock (a yield).
 * @return Nanoseconds. */
Uint64 __wrap_SDL_GetTicksNS(void) { return (Uint64)sched_clock_read(__builtin_return_address(0)); }

Uint64 __wrap_SDL_GetTicks(void);
/** @brief Read the virtual clock in milliseconds (a yield).
 * @return Milliseconds. */
Uint64 __wrap_SDL_GetTicks(void) { return (Uint64)sched_clock_read(__builtin_return_address(0)) / 1000000u; }

void __wrap_SDL_DelayNS(Uint64 ns);
/** @brief Sleep ns of virtual time (a blocking yield).
 * @param ns Duration. */
void __wrap_SDL_DelayNS(Uint64 ns) { sched_sleep_until(sched_now() + (int64_t)ns, __builtin_return_address(0)); }

void __wrap_SDL_Delay(Uint32 ms);
/** @brief Sleep ms of virtual time (a blocking yield).
 * @param ms Duration in milliseconds. */
void __wrap_SDL_Delay(Uint32 ms) { sched_sleep_until(sched_now() + (int64_t)ms * 1000000, __builtin_return_address(0)); }

int __wrap_SDL_GetAtomicInt(SDL_AtomicInt *a);
/** @brief Load an SDL atomic (a yield).
 * @param a The atomic.
 * @return Its value. */
int __wrap_SDL_GetAtomicInt(SDL_AtomicInt *a)
{
    sched_point_ra(SCHED_OP_ATOMIC, a, __builtin_return_address(0));
    sched_tsan_acquire(a);
    return a->value;
}

int __wrap_SDL_SetAtomicInt(SDL_AtomicInt *a, int v);
/** @brief Store an SDL atomic (a yield).
 * @param a The atomic.
 * @param v New value.
 * @return The old value. */
int __wrap_SDL_SetAtomicInt(SDL_AtomicInt *a, int v)
{
    sched_point_ra(SCHED_OP_ATOMIC, a, __builtin_return_address(0));
    sched_tsan_acquire(a);
    const int old = a->value;
    a->value = v;
    sched_tsan_release(a);
    return old;
}

int __wrap_SDL_AddAtomicInt(SDL_AtomicInt *a, int v);
/** @brief Add to an SDL atomic (a yield).
 * @param a The atomic.
 * @param v Addend.
 * @return The old value. */
int __wrap_SDL_AddAtomicInt(SDL_AtomicInt *a, int v)
{
    sched_point_ra(SCHED_OP_ATOMIC, a, __builtin_return_address(0));
    sched_tsan_acquire(a);
    const int old = a->value;
    a->value = old + v;
    sched_tsan_release(a);
    return old;
}

bool __wrap_SDL_CompareAndSwapAtomicInt(SDL_AtomicInt *a, int oldval, int newval);
/** @brief Compare-and-swap an SDL atomic (a yield).
 * @param a The atomic.
 * @param oldval Expected value.
 * @param newval Replacement.
 * @return true when swapped. */
bool __wrap_SDL_CompareAndSwapAtomicInt(SDL_AtomicInt *a, int oldval, int newval)
{
    sched_point_ra(SCHED_OP_ATOMIC, a, __builtin_return_address(0));
    sched_tsan_acquire(a);
    if (a->value != oldval) return false;
    a->value = newval;
    sched_tsan_release(a);
    return true;
}
