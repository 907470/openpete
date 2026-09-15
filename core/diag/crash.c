// core/diag/crash.c — SIGSEGV/SIGABRT handler that dumps minimal state.
//
// Installed once at startup; on crash, prints where the fault happened
// and the saved CPU's pc/ra/sp before re-raising the default disposition
// so the OS can produce a core if configured.
//
// SA_SIGINFO + ucontext_t — recompiled code runs on a libco
// fiber, so libc's `backtrace()` walks the wrong stack chain and returns
// 2 frames (just the signal handler). The sigcontext gives us the real
// host RIP, which `addr2line -e openpete-spyro1 <hex>` resolves to the
// compiled-MIPS function that faulted.

#define _GNU_SOURCE
#include "openpete_psx.h"
#include "openpete_log.h"
#include "openpete_mods.h"   /* crash attribution: name the offending mod */

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

    /* Rung 6: if the sim was inside a mod's override chain, say so —
     * the crash is in that mod or in code it called. */
    {
        uint32_t maddr = 0;
        const char* mod = openpete_mods_crash_attribution(&maddr);
        if (mod)
            fprintf(stderr,
                    "  MOD ATTRIBUTION: inside override of 0x%08X "
                    "registered by mod '%s'\n", maddr, mod);
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
    /* The log file mirrors stderr through a pipe, so the dump we just
     * wrote is still in flight; re-raising would kill the process with it
     * unread. Drain before dying — a crash report with no crash in it is
     * the one case the file exists for. */
    openpete_log_file_sync();

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

#else  /* _WIN32 — POSIX sigaction unavailable; use Win32 SEH instead */

#include <windows.h>

static CPUState* g_diag_cpu = NULL;
void openpete_psx_set_diag_cpu(CPUState* cpu) { g_diag_cpu = cpu; }

static const char* seh_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:    return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:      return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:    return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_BREAKPOINT:          return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_INT_OVERFLOW:        return "EXCEPTION_INT_OVERFLOW";
        case 0xC0000409u:                   return "STATUS_STACK_BUFFER_OVERRUN";
        default:                            return "?";
    }
}

/* Thread that installed the handler — i.e. main. Lets the banner say whether
 * the fault was on main or a worker without the reader having to guess. */
static DWORD g_main_tid;
static DWORD openpete_crash_main_tid(void) { return g_main_tid; }

static LONG WINAPI on_seh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    /* Name the thread. Once the host loop runs off the main thread, "where did
     * it crash" is unanswerable without this: markers logged from one thread
     * cannot attribute a fault raised on another, and a fault anywhere kills
     * the process identically. */
    fprintf(stderr, "\n=== openpete crash: %s (0x%08lX) thread=%lu%s ===\n",
            seh_name(code), (unsigned long)code,
            (unsigned long)GetCurrentThreadId(),
            (GetCurrentThreadId() == openpete_crash_main_tid()) ? " (main)" : "");
    if (code == EXCEPTION_ACCESS_VIOLATION
        && ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "  fault addr=0x%llx kind=%s\n",
                (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1],
                ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" :
                ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" :
                ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "DEP" : "?");
    }
    CONTEXT* cx = ep->ContextRecord;
    fprintf(stderr,
            "  host RIP=0x%llx RSP=0x%llx RBP=0x%llx\n",
            (unsigned long long)cx->Rip,
            (unsigned long long)cx->Rsp,
            (unsigned long long)cx->Rbp);
    if (g_diag_cpu) {
        fprintf(stderr,
                "  pc=0x%08X ra=0x%08X sp=0x%08X gp=0x%08X\n"
                "  v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X\n",
                g_diag_cpu->pc, g_diag_cpu->ra, g_diag_cpu->sp, g_diag_cpu->gp,
                g_diag_cpu->v0, g_diag_cpu->v1, g_diag_cpu->a0, g_diag_cpu->a1);
    } else {
        fprintf(stderr, "  (no CPUState registered)\n");
    }
    {
        uint32_t maddr = 0;
        const char* mod = openpete_mods_crash_attribution(&maddr);
        if (mod)
            fprintf(stderr,
                    "  MOD ATTRIBUTION: inside override of 0x%08X "
                    "registered by mod '%s'\n", maddr, mod);
    }
    fflush(stderr);
    /* Hand back to Windows so the OS still terminates / can produce a
     * dump. EXECUTE_HANDLER would swallow the exception. */
    return EXCEPTION_CONTINUE_SEARCH;
}

void openpete_psx_install_crash_handler(void) {
    g_main_tid = GetCurrentThreadId();
    SetUnhandledExceptionFilter(on_seh);
    OPL_INFO_F("boot", "crash handler installed (Win32 SEH unhandled-exception filter)");
}

#endif  /* !_WIN32 */
