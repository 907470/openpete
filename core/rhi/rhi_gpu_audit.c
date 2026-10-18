/* See rhi_gpu_audit.h for what this measures and why. */
#include "rhi_gpu_audit.h"

#include <stdio.h>
#include <string.h>

/* The audit asks whether GUEST EXECUTION touched the device. A present
 * thread running concurrently makes Vulkan calls while the guest runs; the
 * flag is per thread, so only the guest's own thread sees it set and counts. */
_Thread_local int g_openpete_guest_running = 0;

#define OP_GPU_AUDIT_MAX_NAMES 32

static unsigned    s_total;
static const char *s_names[OP_GPU_AUDIT_MAX_NAMES];
static int         s_name_n;

void op_gpu_audit_hit(const char *fn)
{
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
    s_map_writes++;
    s_map_bytes += (unsigned long long)bytes;
}

unsigned           op_gpu_audit_map_writes(void) { return s_map_writes; }
unsigned long long op_gpu_audit_map_bytes(void)  { return s_map_bytes; }
