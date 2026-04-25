// core/runtime/crash.c — SIGSEGV/SIGABRT handler that dumps minimal state.
//
// Phase 1b: install once at startup; on crash, print where the fault
// happened and the saved CPU's pc/ra/sp before re-raising the default
// disposition so the OS can produce a core if configured.
//
// Phase 1f: SA_SIGINFO + ucontext_t — recompiled code runs on a libco
// fiber, so libc's `backtrace()` walks the wrong stack chain and returns
// 2 frames (just the signal handler). The sigcontext gives us the real
// host RIP, which `addr2line -e openpete-spyro1 <hex>` resolves to the
// compiled-MIPS function that faulted.

#define _GNU_SOURCE
#include "openpete_psx.h"
#include "openpete_log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The whole sigaction-based handler is POSIX-only. On MinGW/Windows
 * we fall back to a stub installer (Windows uses SetUnhandledExceptionFilter
 * via different machinery; until that's wired up, crashes just take the
 * default disposition). */
#if !defined(_WIN32)

#if defined(__GLIBC__) || defined(__APPLE__)
#  include <execinfo.h>
#  define OPENPETE_HAS_BACKTRACE 1
#endif

#if defined(__linux__)
#  include <ucontext.h>
#  define OPENPETE_HAS_UCONTEXT 1
#endif

/* The runtime hands us a CPUState* via openpete_psx_set_diag_cpu (defined
 * here, header-decl optional). On a crash we print whatever we have. */
static CPUState* g_diag_cpu = NULL;

void openpete_psx_set_diag_cpu(CPUState* cpu) { g_diag_cpu = cpu; }

static void on_signal(int sig, siginfo_t* info, void* ucontext) {
    const char* name = "?";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGABRT: name = "SIGABRT"; break;
        case SIGFPE:  name = "SIGFPE";  break;
        case SIGILL:  name = "SIGILL";  break;
        case SIGBUS:  name = "SIGBUS";  break;
        default:                        break;
    }
    fprintf(stderr, "\n=== openpete crash: %s ===\n", name);

    if (info) {
        fprintf(stderr, "  fault addr=%p code=%d\n",
                info->si_addr, info->si_code);
    }

#ifdef OPENPETE_HAS_UCONTEXT
    /* Pull the actual host RIP from the sigcontext. backtrace() can't
     * walk the libco fiber stack, but RIP is always correct. Resolve
     * with: addr2line -e openpete-spyro1 -f -C <rip-hex> */
    if (ucontext) {
        ucontext_t* uc = (ucontext_t*)ucontext;
        unsigned long long rip = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
        unsigned long long rsp = (unsigned long long)uc->uc_mcontext.gregs[REG_RSP];
        unsigned long long rbp = (unsigned long long)uc->uc_mcontext.gregs[REG_RBP];
        fprintf(stderr,
                "  host RIP=0x%llx RSP=0x%llx RBP=0x%llx\n",
                rip, rsp, rbp);
        /* Dump stack top to find the caller — *rsp is the return address
         * if we faulted at the very first instruction of a function. */
        if (rsp != 0u) {
            unsigned long long* sp = (unsigned long long*)(uintptr_t)rsp;
            for (int i = 0; i < 8; i++) {
                fprintf(stderr, "  [rsp+%d]=0x%llx\n", i*8, sp[i]);
            }
        }
    }
#endif

    if (g_diag_cpu) {
        fprintf(stderr,
                "  pc=0x%08X ra=0x%08X sp=0x%08X gp=0x%08X\n"
                "  v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X\n",
                g_diag_cpu->pc, g_diag_cpu->ra, g_diag_cpu->sp, g_diag_cpu->gp,
                g_diag_cpu->v0, g_diag_cpu->v1, g_diag_cpu->a0, g_diag_cpu->a1);
    } else {
        fprintf(stderr, "  (no CPUState registered)\n");
    }

#ifdef OPENPETE_HAS_BACKTRACE
    /* Backtrace usually gives 2 frames on a libco fiber, but include it
     * anyway — sometimes it catches a host-fiber-side fault. */
    {
        void* frames[32];
        int n = backtrace(frames, 32);
        fprintf(stderr, "  host backtrace (%d frames):\n", n);
        backtrace_symbols_fd(frames, n, fileno(stderr));
    }
#endif
    fflush(stderr);

    /* Restore default handler and re-raise so the OS can dump core etc. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void openpete_psx_install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
#ifdef SIGBUS
    sigaction(SIGBUS,  &sa, NULL);
#endif
    OPL_INFO_F("boot", "crash handler installed (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS)");
}

#else  /* _WIN32 — POSIX sigaction surface unavailable under MinGW */

void openpete_psx_set_diag_cpu(CPUState* cpu) { (void)cpu; }

void openpete_psx_install_crash_handler(void) {
    OPL_INFO_F("boot", "crash handler: stub (Windows/MinGW)");
}

#endif  /* !_WIN32 */
