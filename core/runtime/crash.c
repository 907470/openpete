// core/runtime/crash.c — SIGSEGV/SIGABRT handler that dumps minimal state.
//
// Phase 1b: install once at startup; on crash, print where the fault
// happened and the saved CPU's pc/ra/sp before re-raising the default
// disposition so the OS can produce a core if configured.

#include "openpete_psx.h"
#include "openpete_log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime hands us a CPUState* via openpete_psx_set_diag_cpu (defined
 * here, header-decl optional). On a crash we print whatever we have. */
static CPUState* g_diag_cpu = NULL;

void openpete_psx_set_diag_cpu(CPUState* cpu) { g_diag_cpu = cpu; }

static void on_signal(int sig) {
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
    if (g_diag_cpu) {
        fprintf(stderr,
                "  pc=0x%08X ra=0x%08X sp=0x%08X gp=0x%08X\n"
                "  v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X\n",
                g_diag_cpu->pc, g_diag_cpu->ra, g_diag_cpu->sp, g_diag_cpu->gp,
                g_diag_cpu->v0, g_diag_cpu->v1, g_diag_cpu->a0, g_diag_cpu->a1);
    } else {
        fprintf(stderr, "  (no CPUState registered)\n");
    }
    fflush(stderr);

    /* Restore default handler and re-raise so the OS can dump core etc. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void openpete_psx_install_crash_handler(void) {
    signal(SIGSEGV, on_signal);
    signal(SIGABRT, on_signal);
    signal(SIGFPE,  on_signal);
    signal(SIGILL,  on_signal);
#ifdef SIGBUS
    signal(SIGBUS,  on_signal);
#endif
    OPL_INFO_F("boot", "crash handler installed (SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS)");
}
