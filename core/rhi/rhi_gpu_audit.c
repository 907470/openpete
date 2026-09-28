/* See rhi_gpu_audit.h for what this measures and why. */
#include "rhi_gpu_audit.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

int g_openpete_guest_running = 0;

/* The audit asks whether GUEST EXECUTION touched the device. With a present
 * thread running concurrently, a call made on that thread while the guest
 * flag is up is the design working, not the guest reaching the device — so
 * only calls from the guest's own thread count. */
static SDL_ThreadID s_guest_tid;
void op_gpu_audit_bind_thread(void) { s_guest_tid = SDL_GetCurrentThreadID(); }
/** @brief Report whether the calling thread is the bound guest thread.
 *
 * @return 1 when no thread is bound yet or the caller is the bound one, else
 *         0. */
static int on_guest_thread(void) { return !s_guest_tid || SDL_GetCurrentThreadID() == s_guest_tid; }

#define OP_GPU_AUDIT_MAX_NAMES 32

static unsigned    s_total;
static const char *s_names[OP_GPU_AUDIT_MAX_NAMES];
static int         s_name_n;

void op_gpu_audit_hit(const char *fn)
{
    if (!on_guest_thread()) return;
    s_total++;
    /* The name comes from a #fn stringization, so every call site for one
     * entry point passes the same pointer -- compare by address first and
     * only fall back to strcmp for the (impossible in practice) case of a
     * compiler that does not pool identical literals. */
    for (int i = 0; i < s_name_n; i++)
        if (s_names[i] == fn || strcmp(s_names[i], fn) == 0) return;
    if (s_name_n < OP_GPU_AUDIT_MAX_NAMES) s_names[s_name_n++] = fn;
}

unsigned op_gpu_audit_total(void) { return s_total; }

int op_gpu_audit_names(const char **out, int max)
{
    int n = s_name_n < max ? s_name_n : max;
    for (int i = 0; i < n; i++) out[i] = s_names[i];
    return n;
}

static unsigned           s_map_writes;
static unsigned long long s_map_bytes;

void op_gpu_audit_map_write(size_t bytes)
{
    if (!on_guest_thread()) return;
    s_map_writes++;
    s_map_bytes += (unsigned long long)bytes;
}

unsigned           op_gpu_audit_map_writes(void) { return s_map_writes; }
unsigned long long op_gpu_audit_map_bytes(void)  { return s_map_bytes; }
