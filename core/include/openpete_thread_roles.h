/* core/include/openpete_thread_roles.h - thread-role ownership of mutable
 * state, checked by clang Thread Safety Analysis and at run time.
 *
 * A role names a set of code whose accesses are ordered with each other by
 * construction (an ordering domain), not a thread: the host loop and the
 * game fiber alternate on one thread through the fiber-switch wrapper and
 * are one role, `sim`. Each role is one capability object; state owned by a
 * role carries OP_GUARDED_BY(<role>) and the functions that touch it
 * OP_REQUIRES(<role>) or OP_REQUIRES_SHARED(<role>). Under GCC every
 * attribute macro expands to nothing; under clang to the thread-safety
 * attribute, which scripts/fences/thread_roles.py checks with clang-19
 * -Wthread-safety.
 *
 * At run time each thread holds a role mask in a thread-local private to
 * core/runtime/thread_roles.c. OP_ROLE_BIND ORs a role in at a thread entry,
 * OP_ROLE_ENTER / op_role_leave raise and restore it around an inline
 * creation fallback, OP_ROLE_ASSERT checks it. Binds, enters and asserts are
 * legal only in the functions OP_ROLE_BINDING_TABLE lists, each with the
 * registration form through which another thread or the OS reaches it; the
 * fence refuses any other. Every check site is a static record counting its
 * evaluations and failures; a failure logs once per site, and aborts when
 * OPENPETE_THREAD_ROLE_FATAL=1. OPENPETE_THREAD_ROLE_REPORT=<path> writes
 * every reached site's counts at exit.
 *
 * Boot: main reaches openpete_run through a function pointer, so openpete_run
 * acquires `sim` (op_role_boot_acquire) and releases it at return;
 * run_split_threaded hands it to the sim thread (op_role_boot_handoff) and
 * takes it back after the join or on a failed thread creation
 * (op_role_boot_reacquire). The fence refuses a call to a boot function
 * anywhere else.
 */
#ifndef OPENPETE_THREAD_ROLES_H
#define OPENPETE_THREAD_ROLES_H

#ifdef __cplusplus
/* C++ linkage even when an includer wraps this header in extern "C". */
extern "C++" {
#  include <atomic>
}
#  define OP_ROLE_ATOMIC(T) std::atomic<T>
extern "C" {
#else
#  include <stdatomic.h>
#  define OP_ROLE_ATOMIC(T) _Atomic(T)
#endif

/* ------------------------------------------------------------------ *
 * Attribute macros                                                    *
 * ------------------------------------------------------------------ */

#if defined(__clang__)
#  define OP_TSA_(x) __attribute__((x))
#else
#  define OP_TSA_(x)
#endif

/* A capability type; `kind` names it in diagnostics ("role", "mutex"). */
#define OP_CAPABILITY_TYPE(kind)       OP_TSA_(capability(kind))

/* Role forms: the argument is a role name from OP_ROLE_LIST (`sim`). */
#define OP_GUARDED_BY(role)            OP_TSA_(guarded_by(op_role_##role))
#define OP_PT_GUARDED_BY(role)         OP_TSA_(pt_guarded_by(op_role_##role))
#define OP_REQUIRES(role)              OP_TSA_(requires_capability(op_role_##role))
#define OP_REQUIRES_SHARED(role)       OP_TSA_(requires_shared_capability(op_role_##role))

/* Capability-object forms: the argument is a capability expression (a
 * module's own lock object, a mutex wrapper's parameter). An annotated
 * lock wrapper carries OP_ACQUIRE_CAP / OP_RELEASE_CAP on every
 * declaration. */
#define OP_GUARDED_BY_CAP(cap)         OP_TSA_(guarded_by(cap))
#define OP_PT_GUARDED_BY_CAP(cap)      OP_TSA_(pt_guarded_by(cap))
#define OP_REQUIRES_CAP(cap)           OP_TSA_(requires_capability(cap))
#define OP_REQUIRES_SHARED_CAP(cap)    OP_TSA_(requires_shared_capability(cap))
#define OP_ACQUIRE_CAP(cap)            OP_TSA_(acquire_capability(cap))
#define OP_RELEASE_CAP(cap)            OP_TSA_(release_capability(cap))
#define OP_ACQUIRE_SHARED_CAP(cap)     OP_TSA_(acquire_shared_capability(cap))
#define OP_RELEASE_SHARED_CAP(cap)     OP_TSA_(release_shared_capability(cap))

/* A partitioned accessor's body; the fence refuses it outside
 * OP_TSA_ACCESSOR_TABLE. */
#define OP_NO_THREAD_SAFETY_ANALYSIS   OP_TSA_(no_thread_safety_analysis)

/* Statement-scoped escape. `reason` is a non-empty string literal the fence
 * reads; the pragmas are clang's, so GCC sees nothing (a bare `#pragma clang`
 * raises GCC -Wunknown-pragmas). */
#if defined(__clang__)
#  define OP_TSA_SUPPRESS_BEGIN(reason) \
    _Pragma("clang diagnostic push") \
    _Pragma("clang diagnostic ignored \"-Wthread-safety\"")
#  define OP_TSA_SUPPRESS_END _Pragma("clang diagnostic pop")
#else
#  define OP_TSA_SUPPRESS_BEGIN(reason)
#  define OP_TSA_SUPPRESS_END
#endif

/* Sealed global: written only by the functions OP_SEAL_TABLE lists for
 * `seal`, before op_seal(seal), read-only afterwards. Written directly after
 * the declared identifier, on every declaration. The fence checks write
 * sites by compiling each TU with the global const-qualified outside the
 * listed writers. */
#if defined(__clang__)
#  define OP_SEALED(seal)              __attribute__((annotate("op_sealed:" #seal)))
#else
#  define OP_SEALED(seal)
#endif

/* Function-pointer parameter whose argument is called only during the call
 * that receives it; a role-annotated function passed there is checked as a
 * call at the passing site. */
#if defined(__clang__)
#  define OP_SYNC_FN                   __attribute__((annotate("op_sync_fn")))
#else
#  define OP_SYNC_FN
#endif

/* Function-pointer slot (a field, a parameter or a file-scope variable):
 * a function stored into it must require no role outside `role`, and a call
 * through it requires `role`. */
#if defined(__clang__)
#  define OP_SLOT(role)                __attribute__((annotate("op_slot:" #role)))
#else
#  define OP_SLOT(role)
#endif

/* ------------------------------------------------------------------ *
 * Roles                                                               *
 * ------------------------------------------------------------------ */

/* Every role in a thread's mask, with its bit. `display` is a capability
 * but not a mask bit: its owner is the display seam's own state. */
#define OP_ROLE_LIST(X) \
    X(sim,        0)    \
    X(main,       1)    \
    X(present,    2)    \
    X(portalsky,  3)    \
    X(workpool,   4)    \
    X(texdec,     5)    \
    X(sfxdec,     6)    \
    X(music,      7)    \
    X(disc,       8)    \
    X(record,     9)    \
    X(pcache,    10)    \
    X(compile,   11)    \
    X(logpump,   12)    \
    X(stall,     13)    \
    X(launcher,  14)    \
    X(crashdump, 15)    \
    X(crashdemo, 16)    \
    X(ingest,    17)

typedef struct OP_CAPABILITY_TYPE("role") op_role_cap { char unused; } op_role_cap_t;

/* A module's own lock object: a capability with no run-time content. The
 * module's lock and unlock wrappers carry OP_ACQUIRE_CAP / OP_RELEASE_CAP of
 * it and are listed in OP_TSA_ACCESSOR_TABLE, since their bodies call an
 * unannotated lock; the state the lock protects carries
 * OP_GUARDED_BY_CAP(<object>). The object is named only in those
 * attributes, which GCC does not see, so the type is marked possibly
 * unused. */
#if defined(__GNUC__)
#  define OP_LOCK_CAP_UNUSED_ __attribute__((unused))
#else
#  define OP_LOCK_CAP_UNUSED_
#endif
typedef struct OP_CAPABILITY_TYPE("mutex") OP_LOCK_CAP_UNUSED_ op_lock_cap { char unused; } op_lock_cap_t;

#define OP_ROLE_DECLARE_CAP_(name, bit) extern const op_role_cap_t op_role_##name;
OP_ROLE_LIST(OP_ROLE_DECLARE_CAP_)
extern const op_role_cap_t op_role_display;
#undef OP_ROLE_DECLARE_CAP_

#define OP_ROLE_ENUM_(name, bit) OP_ROLE_BIT_##name = 1u << (bit),
enum { OP_ROLE_LIST(OP_ROLE_ENUM_) };
#undef OP_ROLE_ENUM_

/* ------------------------------------------------------------------ *
 * The binding table                                                   *
 * ------------------------------------------------------------------ */

/* ROW(usr, form, roles): the function with clang USR `usr` may bind, enter
 * or assert the roles in `roles` (names joined by `|`), and is reached only
 * through `form`, one of:
 *   "arg:<callee>"         an argument of a call to <callee>
 *   "field:<type>.<field>" a store to the named struct field
 *   "init:<type>"          an initializer of the named type
 *   "prop:<key>"           SDL_SetPointerProperty(<props>, <key>, fn)
 * The fence reads this table from the text, one row per line; a change to
 * a row carries a `thread-roles:` reason on its line. */
#define OP_ROLE_BINDING_TABLE(ROW) \
    ROW("c:run.c@F@openpete_sim_thread", "prop:SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER", "sim") /* thread-roles: the sim thread entry binds sim */ \
    ROW("c:run.c@F@game_fiber_entry", "arg:openpete_fiber_create_guarded", "sim") /* thread-roles: the game fiber runs on the thread holding sim */ \
    ROW("c:portal_sky_s23.c@F@psk_worker", "arg:SDL_CreateThreadRuntime", "portalsky") /* thread-roles: the portal-sky build thread binds portalsky */ \
    ROW("c:cyclorama.c@F@on_sky_draw_cyc", "init:feature_dispatch_entry_t", "sim") /* thread-roles: guest dispatch hook, run on the game fiber */ \
    ROW("c:cyclorama.c@F@cyc_pre", "init:hook_bracket_desc", "sim") /* thread-roles: hook-bracket pre step, run inside on_sky_draw_cyc */ \
    ROW("c:cyclorama.c@F@cyc_verify_enabled", "init:hook_bracket_desc", "sim") /* thread-roles: hook-bracket verify latch, run inside on_sky_draw_cyc */ \
    ROW("c:cyclorama.c@F@cyc_apply_options", "init:feature_module_t", "sim") /* thread-roles: feature options, run by run_host_init in the boot window */ \
    ROW("c:portal_spyro23.c@F@on_portal_pass", "init:feature_dispatch_entry_t", "sim") /* thread-roles: guest dispatch hook, run on the game fiber */ \
    ROW("c:portal_spyro23.c@F@on_portal_leaf", "init:feature_dispatch_entry_t", "sim") /* thread-roles: guest dispatch hook, run on the game fiber */ \
    ROW("c:transition.c@F@on_8004F000_crossfade", "init:feature_dispatch_entry_t", "sim") /* thread-roles: guest dispatch hook, run on the game fiber */ \
    ROW("c:reextract.c@F@game_reextract_body", "arg:game_reextract_view_install", "sim") /* thread-roles: each game's sub-tick re-extract, run by the host loop */ \
    ROW("c:reextract_spyro23.c@F@game_reextract_body", "arg:game_reextract_view_install", "sim") /* thread-roles: the Spyro 2/3 sub-tick re-extract, run by the host loop */ \
    ROW("c:@F@game_assets_run_inline", "arg:game_assets_run_inline", "ingest") /* thread-roles: the asset extraction job's headless runner enters ingest for the job's span */ \
    ROW("c:launcher.cpp@aN@F@ingest_main#*v#", "arg:SDL_CreateThreadRuntime", "ingest") /* thread-roles: the launcher's ingest thread entry binds ingest for the asset job */ \
    ROW("c:launcher.cpp@aN@F@ingest_run_inline#", "arg:ingest_run_inline", "ingest") /* thread-roles: the launcher's asset job without screens enters ingest for the job's span */

/* SEAL(seal, usr): the function with USR `usr` may write the globals sealed
 * as `seal` (before op_seal), and calls op_seal_check(seal). */
#define OP_SEAL_TABLE(SEAL) \
    SEAL(time, "c:@F@op_time_init") /* thread-roles: fills the init group */ \
    SEAL(time, "c:@F@op_time_reload_env") /* thread-roles: re-reads the slack and census knobs */ \
    SEAL(time, "c:openpete_time.c@F@parse_slack_env") /* thread-roles: called by the two above */ \
    SEAL(time, "c:openpete_time.c@F@parse_census_env") /* thread-roles: called by the two above */ \
    SEAL(game_layout, "c:@F@openpete_game_runtime_register") /* thread-roles: copies the descriptor's layout */ \
    SEAL(psk, "c:portal_sky_s23.c@F@psk_sched_init") /* thread-roles: creates the portal-sky lock and conditions before the worker */

/* ACCESSOR(usr): a partitioned accessor or a module's lock wrapper, whose
 * body may carry OP_NO_THREAD_SAFETY_ANALYSIS. */
#define OP_TSA_ACCESSOR_TABLE(ACCESSOR) \
    ACCESSOR("c:portal_sky_s23.c@F@psk_lock") /* thread-roles: acquires s_psk_lock through SDL_LockMutex */ \
    ACCESSOR("c:portal_sky_s23.c@F@psk_unlock") /* thread-roles: releases s_psk_lock through SDL_UnlockMutex */

/* ESCAPE(usr, reason): a function that reaches role- or lock-owned state
 * through a pointer the analysis does not follow, with the protocol that
 * orders the accesses. The fence reports each row and the rows no analysed
 * TU defines; TSan checks the accesses. */
#define OP_ESCAPE_TABLE(ESCAPE) \
    ESCAPE("c:portal_sky_s23.c@F@psk_build_entry", "E->pic, E->L and E->level are written by psk_enqueue before the entry is QUEUED; the QUEUED to RUNNING to DONE moves are made under s_psk_lock, and only the thread that moved the entry to RUNNING touches E->tri, E->n and E->cap until DONE") /* thread-roles: the s_cache entry a builder owns */ \
    ESCAPE("c:portal_sky_s23.c@F@psk_append", "E->tri, E->n and E->cap of the entry the calling builder moved to RUNNING") /* thread-roles: the s_cache entry a builder owns */ \
    ESCAPE("c:portal_sky_s23.c@F@psk_decode_sky", "L is an s_lv entry psk_level_sky filled on sim before the entry naming it was QUEUED under s_psk_lock; it is never written again") /* thread-roles: the s_lv entry a queued build reads */ \
    ESCAPE("c:@F@portal_sky_s23_extension", "E->tri and E->n are read after psk_join saw the entry DONE under s_psk_lock; a DONE entry is rewritten only by psk_enqueue on the same thread") /* thread-roles: a finished s_cache entry */

/* ------------------------------------------------------------------ *
 * Run-time check sites                                                *
 * ------------------------------------------------------------------ */

/* One check site: a static record per macro expansion. Counts are relaxed
 * atomics; `next` links the reached sites for the exit report. */
typedef struct op_role_site {
    const char* kind;                      /* "bind", "enter", "assert", "check", "seal" */
    const char* file;
    const char* func;
    int         line;
    unsigned    bits;                      /* role bits the site names */
    OP_ROLE_ATOMIC(unsigned long long) evals;
    OP_ROLE_ATOMIC(unsigned long long) fails;
    OP_ROLE_ATOMIC(int)                linked;
    struct op_role_site* next;
} op_role_site_t;

#define OP_ROLE_SITE_INIT_(kind, bits) { kind, __FILE__, __func__, __LINE__, bits, 0, 0, 0, 0 }

/** @brief OR role bits into the calling thread's mask (a thread entry's bind).
 *
 * @param bits The OP_ROLE_BIT_* values.
 * @param site The site record of the bind. */
void op_role_bind_bits(unsigned bits, op_role_site_t* site);

/** @brief Raise the calling thread's mask by role bits for an inline creation fallback.
 *
 * @param bits The OP_ROLE_BIT_* values the fallback body binds on its own thread.
 * @param site The site record of the enter.
 * @return The prior mask, for op_role_leave. */
unsigned op_role_enter_bits(unsigned bits, op_role_site_t* site);

/** @brief Restore the calling thread's mask saved by op_role_enter_bits.
 *
 * @param prior The value op_role_enter_bits returned. */
void op_role_leave(unsigned prior);

/** @brief Check that the calling thread's mask holds every bit of the site's role bits, counting the evaluation.
 *
 * A failure is counted, logged once per site, and aborts the process under
 * OPENPETE_THREAD_ROLE_FATAL=1.
 * @param site The site record; its `bits` are the roles required.
 * @return 1 when held, else 0. */
int op_role_check_at(op_role_site_t* site);

/** @brief Return the calling thread's role mask.
 *
 * @return The OP_ROLE_BIT_* values held. */
unsigned op_role_mask(void);

/** @brief Return the failures counted over every check site since process start.
 *
 * @return The total of failed role checks and writes after a seal. */
unsigned long long op_role_failures(void);

/* Per-role bind and assert functions: the analysis sees the assert
 * capability, the body checks or sets the mask. Called only through the
 * OP_ROLE_* macros. */
#define OP_ROLE_DECLARE_FNS_(name, bit) \
    void op_role__bind_##name(op_role_site_t* site) OP_TSA_(assert_capability(op_role_##name)); \
    void op_role__assert_##name(op_role_site_t* site) OP_TSA_(assert_capability(op_role_##name)); \
    void op_role__assert_shared_##name(op_role_site_t* site) OP_TSA_(assert_shared_capability(op_role_##name));
OP_ROLE_LIST(OP_ROLE_DECLARE_FNS_)
#undef OP_ROLE_DECLARE_FNS_

/* A role macro is written directly at its site, never inside another
 * macro's body: the fence reads the macro-instantiation cursors. */
#define OP_ROLE_BIND(role) do { \
        static op_role_site_t op_role_site_ = OP_ROLE_SITE_INIT_("bind", OP_ROLE_BIT_##role); \
        op_role__bind_##role(&op_role_site_); } while (0)
#define OP_ROLE_ASSERT(role) do { \
        static op_role_site_t op_role_site_ = OP_ROLE_SITE_INIT_("assert", OP_ROLE_BIT_##role); \
        op_role__assert_##role(&op_role_site_); } while (0)
#define OP_ROLE_ASSERT_SHARED(role) do { \
        static op_role_site_t op_role_site_ = OP_ROLE_SITE_INIT_("assert", OP_ROLE_BIT_##role); \
        op_role__assert_shared_##role(&op_role_site_); } while (0)
/* Stores the prior mask in `prior` (an unsigned lvalue), for op_role_leave.
 * Carries no thread-safety attribute: the inline body is the registered
 * thread function, which opens with its own bind. */
#define OP_ROLE_ENTER(role, prior) do { \
        static op_role_site_t op_role_site_ = OP_ROLE_SITE_INIT_("enter", OP_ROLE_BIT_##role); \
        (prior) = op_role_enter_bits(OP_ROLE_BIT_##role, &op_role_site_); } while (0)
/* Run-time only check (no static capability): counts and reports like an
 * assert. */
#define OP_ROLE_CHECK(role) do { \
        static op_role_site_t op_role_site_ = OP_ROLE_SITE_INIT_("check", OP_ROLE_BIT_##role); \
        (void)op_role_check_at(&op_role_site_); } while (0)

/* ------------------------------------------------------------------ *
 * Boot handoff (openpete_run and run_split_threaded only)             *
 * ------------------------------------------------------------------ */

/** @brief Take `sim` on main at openpete_run's entry, and read the role knobs once. */
void op_role_boot_acquire(void) OP_ACQUIRE_CAP(op_role_sim);

/** @brief Drop `sim` on main at openpete_run's return. */
void op_role_boot_release(void) OP_RELEASE_CAP(op_role_sim);

/** @brief Drop `sim` on main just before the sim thread is created, leaving main holding `main`. */
void op_role_boot_handoff(void) OP_RELEASE_CAP(op_role_sim);

/** @brief Take `sim` back on main after the sim thread's join, or when its creation failed. */
void op_role_boot_reacquire(void) OP_ACQUIRE_CAP(op_role_sim);

/* ------------------------------------------------------------------ *
 * Seals                                                               *
 * ------------------------------------------------------------------ */

typedef struct op_seal {
    const char*        name;
    OP_ROLE_ATOMIC(int) sealed;
} op_seal_t;

#define OP_SEAL_DECLARE(seal) extern op_seal_t op_seal_obj_##seal
#define OP_SEAL_DEFINE(seal)  op_seal_t op_seal_obj_##seal = { #seal, 0 }

/** @brief Seal a group: every later write by its listed writers is counted as a failure.
 *
 * @param seal The seal object. */
void op_seal_apply(op_seal_t* seal);

/** @brief Count one write by a listed writer, failing when it follows the seal.
 *
 * @param seal The seal object.
 * @param site The site record of the writer's check. */
void op_seal_check_at(op_seal_t* seal, op_role_site_t* site);

/** @brief Report whether a group is sealed.
 *
 * @param seal The seal object.
 * @return 1 after op_seal_apply, else 0. */
int op_seal_is_sealed(const op_seal_t* seal);

#define op_seal(seal) op_seal_apply(&op_seal_obj_##seal)
#define op_seal_check(seal) do { \
        static op_role_site_t op_role_site_ = OP_ROLE_SITE_INIT_("seal", 0u); \
        op_seal_check_at(&op_seal_obj_##seal, &op_role_site_); } while (0)

/* ------------------------------------------------------------------ *
 * First-use caches                                                    *
 * ------------------------------------------------------------------ */

#ifndef __cplusplus
/* A first-use cache read by more than one role: `static OP_ONCE_INT s = -1;`
 * then `v = op_once_int_get(&s); if (v < 0) v = op_once_int_fill(&s, x);`. */
#define OP_ONCE_INT _Atomic int

/** @brief Read a first-use cache.
 *
 * @param s The cache.
 * @return Its value; -1 before the first fill. */
static inline int op_once_int_get(OP_ONCE_INT* s) {
    return atomic_load_explicit(s, memory_order_acquire);
}

/** @brief Fill a first-use cache once by compare-and-swap from -1.
 *
 * @param s The cache.
 * @param v The value this caller computed (not -1).
 * @return The value the cache holds: v, or the first filler's. */
static inline int op_once_int_fill(OP_ONCE_INT* s, int v) {
    int expect = -1;
    if (atomic_compare_exchange_strong_explicit(s, &expect, v, memory_order_acq_rel,
                                                memory_order_acquire))
        return v;
    return expect;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_THREAD_ROLES_H */
