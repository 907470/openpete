// core/runtime/libco_shim.c — Phase 1b libco placeholder.
//
// The vendored libco at third_party/libco/ only ships libco.c + libco.h
// (the per-arch sources weren't fetched in Phase 0b). Phase 1b doesn't
// actually create any fibers — it only calls co_active() to capture the
// host fiber for instrumentation. This shim returns a stable sentinel so
// the call works; Phase 1c will replace it with the real libco build by
// dropping in amd64.c / arm.c / ... before any co_create / co_switch.

#include "libco.h"

#include <stdio.h>
#include <stdlib.h>

static const char k_host_sentinel = 'h';

cothread_t co_active(void) {
    return (cothread_t)&k_host_sentinel;
}

cothread_t co_create(unsigned int sz, void (*entry)(void)) {
    (void)sz; (void)entry;
    fprintf(stderr,
            "openpete: co_create called but real libco isn't built yet "
            "(see core/runtime/libco_shim.c).\n");
    abort();
}

cothread_t co_derive(void* mem, unsigned int sz, void (*entry)(void)) {
    (void)mem; (void)sz; (void)entry;
    fprintf(stderr,
            "openpete: co_derive called but real libco isn't built yet.\n");
    abort();
}

void co_delete(cothread_t t) { (void)t; }

void co_switch(cothread_t t) {
    (void)t;
    fprintf(stderr,
            "openpete: co_switch called but real libco isn't built yet.\n");
    abort();
}

int co_serializable(void) { return 0; }
