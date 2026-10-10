/* double-gems - PoC: typed guest-RAM write, real divergence.
 *
 * Post-hook + state-diff (the framework's event-detection idiom): let
 * CollectItem run, diff g_GemTotal across base(), and add the delta
 * again. No need to know WHICH item was collected or re-implement any
 * game logic - the state change IS the event.
 *
 * Spyro 1 binds CollectItem and g_GemTotal by name. Spyro 2 and 3 name
 * neither yet, so their arms use the main-executable addresses: Spyro 2
 * collect func_80039678 and total 0x800670CC, Spyro 3 collect
 * func_80036E60 and total 0x8006C71C (g_GemTotal in the decomp's
 * src/hud.c).
 */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>

#if OPENPETE_GAME_SPYRO1
#define GEM_TOTAL_ADDR OP_GADDR_g_GemTotal
#elif OPENPETE_GAME_SPYRO2
#define GEM_TOTAL_ADDR 0x800670CCu
#define COLLECT_ADDR   0x80039678u
#elif OPENPETE_GAME_SPYRO3
#define GEM_TOTAL_ADDR 0x8006C71Cu
#define COLLECT_ADDR   0x80036E60u
#else
#error "double-gems: no collect or gem total binding for this game"
#endif

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

/* Session tallies, host-side only: pickups the hook doubled and the
 * gems it granted on top of the game's own award. */
static uint32_t g_doubled;
static uint32_t g_bonus_gems;

/* Status lines: repainted from the pickup hook (the only event that
 * changes them) and once at entry. */
static void publish_status(void) {
    g_api->ui_status(g_self, "pickups doubled: %u", g_doubled);
    g_api->ui_status(g_self, "bonus gems granted: %u", g_bonus_gems);
    if (!g_doubled)
        g_api->ui_status(g_self, "no gem pickup seen yet this session");
}

static void on_collect_item(CPUState* cpu) {
    uint32_t before = op_read_u32(g_api, GEM_TOTAL_ADDR);
    g_api->base(cpu);
    uint32_t after = op_read_u32(g_api, GEM_TOTAL_ADDR);
    if (after > before) {
        uint32_t delta = after - before;
        op_write_u32(g_api, GEM_TOTAL_ADDR, after + delta);
        g_doubled++;
        g_bonus_gems += delta;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "gem pickup doubled: +%u twice, total %u -> %u",
                   delta, after, after + delta);
    }
    publish_status();
}


/**
 * @brief Mod entry: attaches the doubling post-hook to the running game's collect function.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return The override registration's result, 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    publish_status();
#if OPENPETE_GAME_SPYRO1
    return api->override_name(self, "CollectItem", on_collect_item);
#else
    return api->override_addr(self, COLLECT_ADDR, on_collect_item);
#endif
}
