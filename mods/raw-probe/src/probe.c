/* raw-probe - PoC: raw-tier override, cpu-context register access.
 *
 * Target: the player avatar's draw - hand-written asm in all three games,
 * un-decompiled, so no natural signature exists. It saves $s0..$ra into a
 * fixed spill block and loads the player struct into $ra before drawing
 * the avatar's two part records. We hook it by bare vaddr, read the MIPS
 * register file straight off the cpu context, and pass through with
 * base(). Fires every tick once a level (or the attract demo) is rendering
 * Spyro. Log-only: sim-neutral.
 *
 * Spyro 1 func_80023AC4 (asm/renderers/r_pete.s:12, $ra = g_Spyro); the
 * gameplay draw func_80019698 calls it once a tick while g_IsSpyroHidden
 * is clear (src/gamestates/draw.c:614-615). Spyro 2
 * func_80041DBC (asm/spyrodraw.s:13, $ra = the player struct 0x80069FF0)
 * and Spyro 3 func_8003CDA0 (asm/spyrodraw.s:11, $ra = g_Spyro
 * 0x80070328), each called twice a tick by the game's Spyro-and-mobys
 * draw: the mirrored copy with player+0x2C set, then the avatar.
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>
#include <psx_runtime.h>

#if OPENPETE_GAME_SPYRO1
#define DRAW_SPYRO_ADDR OP_FNADDR_func_80023AC4
#define DRAW_SPYRO_NAME "func_80023AC4"
#elif OPENPETE_GAME_SPYRO2
#define DRAW_SPYRO_ADDR OP_FNADDR_func_80041DBC
#define DRAW_SPYRO_NAME "func_80041DBC"
#elif OPENPETE_GAME_SPYRO3
#define DRAW_SPYRO_ADDR OP_FNADDR_func_8003CDA0
#define DRAW_SPYRO_NAME "func_8003CDA0"
#else
#error "raw-probe: no player draw binding for this game"
#endif

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned                  g_calls;

/* Last register snapshot, mirrored for the overlay: the log only speaks
 * once every 500 calls, the status section shows every one. */
static unsigned g_last_a0, g_last_a1, g_last_sp, g_last_ra;

/**
 * @brief Repaints the overlay status lines from the call counter and the last register snapshot.
 *
 * Called from the draw hook (the values change per call; ui_status is
 * cheap host-side formatting) and once at entry. Reads only the statics
 * the hook publishes and touches no guest state.
 */
static void publish_status(void) {
    g_api->ui_status(g_self, DRAW_SPYRO_NAME " calls: %u", g_calls);
    if (g_calls == 0u) {
        g_api->ui_status(g_self, "waiting for Spyro to be rendered");
        return;
    }
    g_api->ui_status(g_self, "a0 = 0x%08X   a1 = 0x%08X", g_last_a0,
                     g_last_a1);
    g_api->ui_status(g_self, "sp = 0x%08X   ra = 0x%08X", g_last_sp,
                     g_last_ra);
    g_api->ui_status(g_self, "logged on the 1st call and every 500th");
}

/**
 * @brief Counts a player draw, snapshots and logs its registers, and runs the rest of the chain.
 * @param cpu The guest cpu context at the draw's entry.
 */
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
    publish_status();
}

/**
 * @brief Mod entry: attaches the register probe to the running game's player draw by address.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return The override registration's result, 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    publish_status();
    return api->override_addr(self, DRAW_SPYRO_ADDR, on_spyro_render);
}
