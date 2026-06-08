/*
  libco Emscripten/WASM backend (openpete)

  WebAssembly has no raw register/stack manipulation, so the asm backends
  (amd64.c etc.) cannot be used. Emscripten instead provides cooperative
  fibers built on Asyncify: each fiber owns a normal C stack plus a
  separate "asyncify stack" that holds the spilled local-variable state of
  every active call frame while the fiber is suspended. We map libco's
  co_* surface onto emscripten_fiber_swap.

  REQUIRES linking with -sASYNCIFY. Without it emscripten_fiber_swap is a
  no-op and co_switch never returns control correctly.
*/

#define LIBCO_C
#include "libco.h"
#include "settings.h"

#include <emscripten/fiber.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Asyncify stack size per fiber. This is NOT the C stack — it holds the
 * unwound local state of every frame on the call stack at suspend time.
 * Spyro's recompiled MIPS call chains get deep (PSYQ + recompiler shadow
 * frames), so budget generously. Override at compile time if needed. */
#ifndef LIBCO_EM_ASYNCIFY_STACK
#define LIBCO_EM_ASYNCIFY_STACK (2u * 1024u * 1024u)
#endif

typedef struct {
  emscripten_fiber_t fiber;
  void (*entry)(void);
  void* c_stack;         /* NULL for the primary (current-context) fiber */
  void* asyncify_stack;
} co_em_t;

static co_em_t  co_primary;
static int      co_primary_inited = 0;
static co_em_t* co_running = 0;

static void co_thunk(void* arg) {
  co_em_t* co = (co_em_t*)arg;
  co->entry();
  /* libco entry points are not expected to return. There is no defined
   * fiber to fall back to, so failing loud beats silent corruption. */
  abort();
}

static void co_init_primary(void) {
  if (co_primary_inited) return;
  co_primary_inited = 1;
  co_primary.c_stack = 0;  /* uses the real program stack */
  co_primary.asyncify_stack = malloc(LIBCO_EM_ASYNCIFY_STACK);
  emscripten_fiber_init_from_current_context(
      &co_primary.fiber,
      co_primary.asyncify_stack,
      LIBCO_EM_ASYNCIFY_STACK);
  co_running = &co_primary;
}

cothread_t co_active(void) {
  co_init_primary();
  return (cothread_t)co_running;
}

cothread_t co_derive(void* memory, unsigned int size, void (*entry)(void)) {
  /* Caller-supplied memory isn't supported: emscripten fibers need a
   * separately-managed asyncify stack with its own alignment. openpete
   * uses co_create, never co_derive. */
  (void)memory; (void)size; (void)entry;
  return (cothread_t)0;
}

cothread_t co_create(unsigned int heapsize, void (*entry)(void)) {
  co_init_primary();
  co_em_t* co = (co_em_t*)calloc(1, sizeof(*co));
  if (!co) return 0;
  co->entry = entry;
  co->c_stack = malloc(heapsize);
  co->asyncify_stack = malloc(LIBCO_EM_ASYNCIFY_STACK);
  if (!co->c_stack || !co->asyncify_stack) {
    free(co->c_stack);
    free(co->asyncify_stack);
    free(co);
    return 0;
  }
  emscripten_fiber_init(&co->fiber, co_thunk, co,
                        co->c_stack, heapsize,
                        co->asyncify_stack, LIBCO_EM_ASYNCIFY_STACK);
  return (cothread_t)co;
}

void co_delete(cothread_t handle) {
  co_em_t* co = (co_em_t*)handle;
  if (!co || co == &co_primary) return;
  free(co->c_stack);
  free(co->asyncify_stack);
  free(co);
}

void co_switch(cothread_t handle) {
  co_em_t* co   = (co_em_t*)handle;
  co_em_t* prev = co_running;
  if (!co || co == prev) return;
  co_running = co;
  emscripten_fiber_swap(&prev->fiber, &co->fiber);
}

int co_serializable(void) {
  return 0;
}

#ifdef __cplusplus
}
#endif
