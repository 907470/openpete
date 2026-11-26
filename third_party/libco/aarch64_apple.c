/* Apple arm64 (iOS, macOS on Apple silicon) variant of libco's aarch64.c.
 *
 * Derived from higan-emu/libco aarch64.c (ISC, Copyright byuu and the higan
 * team; see ORIGIN.txt). Upstream stores the context-switch routine as
 * machine-code words in a data array and calls mprotect(PROT_EXEC) on it.
 * iOS refuses that for apps without the JIT entitlement, so here the same
 * routine is assembled into the text section instead. Register layout and
 * instruction sequence are identical to upstream, so the handle layout in
 * co_derive is unchanged. x18 (reserved by Apple) is not touched. */
#define LIBCO_C
#include "libco.h"
#include "settings.h"

#include <stdint.h>

#include "valgrind.h"

#ifdef __cplusplus
extern "C" {
#endif

static thread_local unsigned long co_active_buffer[64];
static thread_local cothread_t co_active_handle = 0;

/* co_swap_function(to, from): x0 = target handle, x1 = current handle. */
void libco_apple_swap(cothread_t to, cothread_t from);
__asm__(
  ".text\n"
  ".p2align 2\n"
  ".private_extern _libco_apple_swap\n"
  ".globl _libco_apple_swap\n"
  "_libco_apple_swap:\n"
  "  mov x16, sp\n"
  "  stp x16, x30, [x1]\n"
  "  ldp x16, x30, [x0]\n"
  "  mov sp, x16\n"
  "  stp x19, x20, [x1, #16]\n"
  "  ldp x19, x20, [x0, #16]\n"
  "  stp x21, x22, [x1, #32]\n"
  "  ldp x21, x22, [x0, #32]\n"
  "  stp x23, x24, [x1, #48]\n"
  "  ldp x23, x24, [x0, #48]\n"
  "  stp x25, x26, [x1, #64]\n"
  "  ldp x25, x26, [x0, #64]\n"
  "  stp x27, x28, [x1, #80]\n"
  "  ldp x27, x28, [x0, #80]\n"
  "  str x29, [x1, #96]\n"
  "  ldr x29, [x0, #96]\n"
  "  stp d8, d9, [x1, #112]\n"
  "  ldp d8, d9, [x0, #112]\n"
  "  stp d10, d11, [x1, #128]\n"
  "  ldp d10, d11, [x0, #128]\n"
  "  stp d12, d13, [x1, #144]\n"
  "  ldp d12, d13, [x0, #144]\n"
  "  stp d14, d15, [x1, #160]\n"
  "  ldp d14, d15, [x0, #160]\n"
  "  br x30\n"
);

cothread_t co_active(void) {
  if(!co_active_handle) co_active_handle = &co_active_buffer;
  return co_active_handle;
}

cothread_t co_derive(void* memory, unsigned int size, void (*entrypoint)(void)) {
  unsigned long* handle;
  if(!co_active_handle) co_active_handle = &co_active_buffer;

  VALGRIND_STACK_REGISTER(memory, memory + size);

  if((handle = (unsigned long*)memory)) {
    unsigned long stack_top = (unsigned long)handle + size;
    unsigned long *p;
    stack_top &= ~((unsigned long) 15);
    p = (unsigned long*)(stack_top);
    handle[0]  = (unsigned long)p;           /* x16 (stack pointer) */
    handle[1]  = (unsigned long)entrypoint;  /* x30 (link register) */
    handle[12] = (unsigned long)p;           /* x29 (frame pointer) */
  }

  return handle;
}

cothread_t co_create(unsigned int size, void (*entrypoint)(void)) {
  void* memory = LIBCO_MALLOC(size);
  if(!memory) return (cothread_t)0;
  return co_derive(memory, size, entrypoint);
}

void co_delete(cothread_t handle) {
  LIBCO_FREE(handle);
}

void co_switch(cothread_t handle) {
  cothread_t co_previous_handle = co_active_handle;
  co_active_handle = handle;
  libco_apple_swap(handle, co_previous_handle);
}

int co_serializable(void) {
  return 1;
}

#ifdef __cplusplus
}
#endif
