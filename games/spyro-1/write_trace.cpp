/* games/spyro-1/write_trace.cpp — frame-gated write_word trace for one
 * recompiled function. Used to bisect the missing-geometry bug — see
 * docs/missing-geometry-bug.md. Activated by setting:
 *
 *   OPENPETE_WRITE_TRACE_FRAMES=A-B   (inclusive openpete_frame range)
 *   OPENPETE_WRITE_TRACE_PATH=path    (default /tmp/write-trace.csv)
 *
 * The override hook in game_extras_impl.c calls openpete_trace_writes_begin
 * on entry to func_80023AC4 and openpete_trace_writes_end on return. Inside
 * the window, cpu->write_word is replaced with a logging wrapper that emits
 * one CSV row per write (frame,addr,value), then forwards to the original
 * write_word. Outside the window the hooks are no-ops.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psx_runtime_internal.h"

extern "C" {
extern unsigned openpete_frame;
}

namespace {

void (*g_orig_write_word)(uint32_t, uint32_t) = nullptr;
FILE*  g_trace_fp = nullptr;
int    g_trace_lo = -2;
int    g_trace_hi = -1;
bool   g_trace_active = false;
unsigned g_call_idx = 0;
CPUState* g_trace_cpu = nullptr;

void trace_write_word(uint32_t addr, uint32_t value) {
    if (g_trace_active && g_trace_fp && g_trace_cpu) {
        /* Log addr/value plus the two destination-base registers so we can
         * recover which source line emitted the write. Most writes in the
         * renderer are `write_word(t9 + N, ...)` or `write_word(at + N, ...)`
         * — diff (addr - t9) or (addr - at) against the source line list. */
        std::fprintf(g_trace_fp, "%u,%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
                     openpete_frame, g_call_idx, addr, value,
                     g_trace_cpu->t9, g_trace_cpu->at, g_trace_cpu->t8);
    }
    if (g_orig_write_word) g_orig_write_word(addr, value);
}

void init_window_once() {
    if (g_trace_lo != -2) return;
    const char* env = std::getenv("OPENPETE_WRITE_TRACE_FRAMES");
    if (!env || !*env) { g_trace_lo = -1; g_trace_hi = -1; return; }
    int a = -1, b = -1;
    if (std::sscanf(env, "%d-%d", &a, &b) == 2 && b >= a) {
        g_trace_lo = a; g_trace_hi = b;
    } else if (std::sscanf(env, "%d", &a) == 1) {
        g_trace_lo = a; g_trace_hi = a;
    } else {
        g_trace_lo = -1; g_trace_hi = -1;
    }
}

bool in_window() {
    if (g_trace_lo < 0) return false;
    int f = (int)openpete_frame;
    return f >= g_trace_lo && f <= g_trace_hi;
}

void open_file_once() {
    if (g_trace_fp) return;
    const char* path = std::getenv("OPENPETE_WRITE_TRACE_PATH");
    if (!path || !*path) path = "/tmp/write-trace.csv";
    g_trace_fp = std::fopen(path, "w");
    if (g_trace_fp) {
        std::fprintf(g_trace_fp, "frame,call,addr,value,t9,at,t8\n");
    }
}

} /* namespace */

extern "C" void openpete_trace_writes_begin(CPUState* cpu) {
    init_window_once();
    if (!in_window()) return;
    open_file_once();
    if (!g_trace_fp) return;
    g_orig_write_word = cpu->write_word;
    cpu->write_word = trace_write_word;
    g_trace_cpu = cpu;
    g_trace_active = true;
    ++g_call_idx;
}

extern "C" void openpete_trace_writes_end(CPUState* cpu) {
    if (!g_trace_active) return;
    cpu->write_word = g_orig_write_word;
    g_orig_write_word = nullptr;
    g_trace_cpu = nullptr;
    g_trace_active = false;
    if (g_trace_fp) std::fflush(g_trace_fp);
}
