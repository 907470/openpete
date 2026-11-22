// core/psx/fiber.c - cooperative scheduler globals.
//
// See core/include/openpete_fiber.h. The actual fiber spawn/loop lives in
// run.c; this TU only owns the shared state so PSYQ thunks (in games/<g>/)
// can call `openpete_yield_to_host()` without dragging in libco directly.

#include "openpete_fiber.h"
#include "openpete_func_trace.h"
#include "openpete_thread_roles.h"

#include <libco.h>
#include <stddef.h>

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer) && !defined(__SANITIZE_THREAD__)
#    define __SANITIZE_THREAD__ 1
#  endif
#endif
#if defined(__SANITIZE_THREAD__)
#  define OPENPETE_FIBER_TSAN 1
#  include <SDL3/SDL_mutex.h>
#  include <sanitizer/tsan_interface.h>
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#  define OPENPETE_FIBER_GUARD 1
#  include <sys/mman.h>
#  include <unistd.h>
#endif

void*        openpete_host_fiber       = NULL;
void*        openpete_game_fiber       = NULL;
SDL_AtomicInt openpete_vbl             = {0};
SDL_AtomicInt openpete_vbl_device      = {0};
SDL_AtomicInt openpete_game_running    = {0};
int openpete_speculative               = 0;
int openpete_yield_final               = 0;
openpete_park_site_t openpete_yield_site = {0u, 0u};
uint32_t     openpete_yield_loop_header = 0u;
int openpete_replaying                 = 0;
int openpete_ra_input_edge             = 0;
unsigned     openpete_frame            = 0u;
static SDL_AtomicU32 s_frame_pub;   /* openpete_frame as last written; see openpete_frame_published */
unsigned     openpete_canon_tick       = 0u;
unsigned     openpete_consume          = 0u;
unsigned     openpete_consume_write     = 0u;
unsigned     openpete_primitives_drawn = 0u;
int          openpete_present_pending  = 0;
int          openpete_fast_boot_active = 1;
int          openpete_cd_load_delay    = 0;
int          openpete_skip_present_until_level = 0;
int          openpete_skip_ffwd_windowed       = 0;
/* openpete_no_lod lives in core/renderer/no_lod.c. */

#if defined(OPENPETE_FIBER_TSAN)
/* ThreadSanitizer keeps one shadow call stack per thread; a libco switch it
 * is not told about corrupts it. Each libco context is paired with a TSan
 * fiber: created with the context, or, for a thread's own context, taken
 * from the thread at its first switch away (tsan_fiber_of). Built only
 * under -fsanitize=thread. */
#  define TSAN_FIBERS 16
static struct { void* co; void* tsan; } s_tsan_fibers[TSAN_FIBERS];
static SDL_Mutex* s_tsan_mx;

/** @brief Return the TSan fiber paired with libco context `co`, pairing `co` with `own` (when not NULL) if it has none.
 * @param co The libco context.
 * @param own The TSan fiber to pair with an unpaired context, or NULL.
 * @return The paired TSan fiber, or NULL. */
static void* tsan_fiber_of(void* co, void* own) {
    if (!s_tsan_mx) s_tsan_mx = SDL_CreateMutex();
    SDL_LockMutex(s_tsan_mx);
    void* found = NULL;
    int free_slot = -1;
    for (int i = 0; i < TSAN_FIBERS; i++) {
        if (s_tsan_fibers[i].co == co) { found = s_tsan_fibers[i].tsan; break; }
        if (!s_tsan_fibers[i].co && free_slot < 0) free_slot = i;
    }
    if (!found && own && free_slot >= 0) {
        s_tsan_fibers[free_slot].co = co;
        s_tsan_fibers[free_slot].tsan = own;
        found = own;
    }
    SDL_UnlockMutex(s_tsan_mx);
    return found;
}

/** @brief Unpair libco context `co` and destroy its TSan fiber.
 * @param co The libco context. */
static void tsan_fiber_drop(void* co) {
    if (!s_tsan_mx) return;
    SDL_LockMutex(s_tsan_mx);
    for (int i = 0; i < TSAN_FIBERS; i++)
        if (s_tsan_fibers[i].co == co) {
            __tsan_destroy_fiber(s_tsan_fibers[i].tsan);
            s_tsan_fibers[i].co = s_tsan_fibers[i].tsan = NULL;
        }
    SDL_UnlockMutex(s_tsan_mx);
}
#  define TSAN_FIBER_CREATED(co) do { if (co) (void)tsan_fiber_of((co), __tsan_create_fiber(0)); } while (0)
#  define TSAN_FIBER_DELETED(co) tsan_fiber_drop(co)
#else
#  define TSAN_FIBER_CREATED(co) ((void)0)
#  define TSAN_FIBER_DELETED(co) ((void)0)
#endif

#if defined(OPENPETE_FIBER_GUARD)
/* The one guarded mapping: the game fiber's stack, guard page first. */
static void*     s_fiber_map;
static size_t    s_fiber_map_len;
static uintptr_t s_guard_lo, s_guard_hi;

void* openpete_fiber_create_guarded(unsigned size, void (*entry)(void)) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t len  = page + (((size_t)size + page - 1) & ~(page - 1));
    void* map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return NULL;
    if (mprotect(map, page, PROT_NONE) != 0) {
        munmap(map, len);
        return NULL;
    }
    s_fiber_map = map;
    s_fiber_map_len = len;
    s_guard_lo = (uintptr_t)map;
    s_guard_hi = s_guard_lo + page;
    /* libco's amd64 co_derive keeps the saved context at the low end of
     * the memory it is given, so the context sits just above the guard and
     * the stack grows down toward both. */
    cothread_t co = co_derive((char*)map + page, (unsigned)(len - page), entry);
    TSAN_FIBER_CREATED(co);
    return co;
}

void openpete_fiber_delete_guarded(void* fiber) {
    if (!fiber || !s_fiber_map) return;
    TSAN_FIBER_DELETED(fiber);
    munmap(s_fiber_map, s_fiber_map_len);
    s_fiber_map = NULL;
    s_guard_lo = s_guard_hi = 0;
}

int openpete_fiber_guard_contains(uintptr_t addr) {
    return addr >= s_guard_lo && addr < s_guard_hi;
}
#else
void* openpete_fiber_create_guarded(unsigned size, void (*entry)(void)) {
    cothread_t co = co_create(size, entry);
    TSAN_FIBER_CREATED(co);
    return co;
}

void openpete_fiber_delete_guarded(void* fiber) {
    if (!fiber) return;
    TSAN_FIBER_DELETED(fiber);
    co_delete((cothread_t)fiber);
}

int openpete_fiber_guard_contains(uintptr_t addr) {
    (void)addr;
    return 0;
}
#endif

/* The guest generation (openpete_guest_generation) and the restores among
 * its advances (openpete_guest_restore_count). */
static unsigned s_guest_gen;
static unsigned s_guest_restores OP_GUARDED_BY(sim);   /* thread-roles: restores and allocator hooks run on sim */

unsigned openpete_guest_generation(void) { return s_guest_gen; }

void openpete_guest_generation_bump(void) OP_REQUIRES(sim) { s_guest_gen++; s_guest_restores++; }

unsigned openpete_guest_restore_count(void) OP_REQUIRES_SHARED(sim) { return s_guest_restores; }

void openpete_fiber_switch(void* to) {
    OP_ROLE_CHECK(sim);
    if (to != openpete_host_fiber) s_guest_gen++;   /* guest code is about to run */
#if defined(OPENPETE_FIBER_TSAN)
    (void)tsan_fiber_of(co_active(), __tsan_get_current_fiber());
    void* target = tsan_fiber_of(to, NULL);
    if (target) __tsan_switch_to_fiber(target, 0);
#endif
    co_switch((cothread_t)to);
}

/** @brief Park the game fiber at a yield: clear openpete_game_running, switch to the host fiber, and set it again on resume. */
static void switch_to_host(void) {
    SDL_SetAtomicInt(&openpete_game_running, 0);
    openpete_fiber_switch(openpete_host_fiber);
    SDL_SetAtomicInt(&openpete_game_running, 1);
}

void openpete_yield_to_host(void) {
    if (!openpete_host_fiber) return;  /* not in fiber mode (smoke test path) */
    /* `openpete_vbl` belongs to psyq_VSync, which advances it once per
     * logical frame waited. Yields only bump the host-side render counter
     * so frame-limit / screenshot / bk2 alignment stay yield-driven. */
    openpete_frame++;
    SDL_SetAtomicU32(&s_frame_pub, openpete_frame);
    op_functrace_pause();     /* exclude host render+sleep from func-trace */
    switch_to_host();
    op_functrace_resume();
}

static openpete_feed_key_t s_feed_key = OPENPETE_FEED_KEY_FRAME;

void openpete_feed_key_select(openpete_feed_key_t key) { s_feed_key = key; }

openpete_feed_key_t openpete_feed_key(void) { return s_feed_key; }

unsigned openpete_feed_key_now(void) {
    return s_feed_key == OPENPETE_FEED_KEY_VBL
               ? (unsigned)SDL_GetAtomicInt(&openpete_vbl) : openpete_frame_published();
}

void openpete_frame_set(unsigned frame) {
    openpete_frame = frame;
    SDL_SetAtomicU32(&s_frame_pub, frame);
}

unsigned openpete_frame_published(void) {
    return SDL_GetAtomicU32(&s_frame_pub);
}

int openpete_fiber_in_game(void) {
    return openpete_game_fiber != NULL && co_active() == (cothread_t)openpete_game_fiber;
}

void openpete_yield_to_host_no_tick(void) {
    if (!openpete_host_fiber) return;
    op_functrace_pause();
    switch_to_host();
    op_functrace_resume();
}

int openpete_park_site_matches(const openpete_park_site_t* site) {
    if (!openpete_yield_final) return 0;
    if (site->ra == 0u) return 1;
    return site->ra == openpete_yield_site.ra && site->sp == openpete_yield_site.sp;
}

static const openpete_feed_observer_t* s_feed_obs = NULL;

void openpete_feed_observer_set(const openpete_feed_observer_t* obs) { s_feed_obs = obs; }

void openpete_feed_consume_enter(void) {
    if (s_feed_obs && s_feed_obs->close) s_feed_obs->close(openpete_consume, openpete_consume_write);
    openpete_consume++;
    openpete_consume_write = 0u;
}

void openpete_feed_pad_write(void) {
    if (s_feed_obs && s_feed_obs->write) s_feed_obs->write(openpete_consume, openpete_consume_write);
    openpete_consume_write++;
}
