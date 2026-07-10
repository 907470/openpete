/* raw-probe — PoC rung 2: raw-tier override, cpu-context register access.
 *
 * Target: func_80023AC4 — Spyro's renderer (hand-written asm in the
 * original, un-decompiled, so no natural signature exists). We hook it
 * by bare vaddr, read the MIPS register file straight off the cpu
 * context, and pass through with base(). Fires every frame once a level
 * (or the attract demo) is rendering Spyro. Log-only: sim-neutral.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <psx_runtime.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_calls;

static void on_spyro_render(CPUState* cpu) {
    g_calls++;
    if (g_calls == 1u || (g_calls % 500u) == 0u)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "raw probe #%u: a0=0x%08X a1=0x%08X sp=0x%08X ra=0x%08X",
                   g_calls, cpu->a0, cpu->a1, cpu->sp, cpu->ra);
    g_api->base(cpu);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    return api->override_addr(self, OP_FNADDR_func_80023AC4, on_spyro_render);
}
