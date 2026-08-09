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

/* Last register snapshot, mirrored for the overlay: the log only speaks
 * once every 500 calls, the status section shows every one. */
static unsigned g_last_a0, g_last_a1, g_last_sp, g_last_ra;

static void on_spyro_render(CPUState* cpu) {
    g_calls++;
    g_last_a0 = cpu->a0;
    g_last_a1 = cpu->a1;
    g_last_sp = cpu->sp;
    g_last_ra = cpu->ra;
    if (g_calls == 1u || (g_calls % 500u) == 0u)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "raw probe #%u: a0=0x%08X a1=0x%08X sp=0x%08X ra=0x%08X",
                   g_calls, cpu->a0, cpu->a1, cpu->sp, cpu->ra);
    g_api->base(cpu);
}

/* Overlay status. Render-pure: reads the statics the hook publishes,
 * touches no guest state. */
static void ui_draw(const openpete_mod_ui_t* ui, void* user) {
    (void)user;

    ui->text("func_80023AC4 calls: %u", g_calls);
    if (g_calls == 0u) {
        ui->text_disabled("waiting for Spyro to be rendered");
        return;
    }
    ui->text("a0 = 0x%08X   a1 = 0x%08X", g_last_a0, g_last_a1);
    ui->text("sp = 0x%08X   ra = 0x%08X", g_last_sp, g_last_ra);
    ui->text_disabled("logged on the 1st call and every 500th");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    api->register_ui(self, ui_draw, NULL);
    return api->override_addr(self, OP_FNADDR_func_80023AC4, on_spyro_render);
}
