/* modsave-probe - counts stock GamestateUpdate executions.
 *
 * The innermost override on GamestateUpdate (first in enabled.txt): it
 * runs only when every later-listed override calls its base, so its count
 * is the number of stock updates. The count is an arena word, rolled back
 * with the timeline. Each `log_every`-th update logs
 * "modsave-probe: stock <n> gs <g>".
 */
#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static uint32_t*                 g_count;
static uint32_t                  g_every;

/** @brief GamestateUpdate override: runs the stock update, then counts and logs it. */
static void on_update(CPUState* cpu) {
    g_api->base(cpu);
    const uint32_t n = ++*g_count;
    if (n % g_every == 0u)
        g_api->log(g_self, OP_MOD_LOG_INFO, "modsave-probe: stock %u gs %u", n,
                   op_read_u32(g_api, OP_GADDR_g_Gamestate));
}

/**
 * @brief Mod entry: allocates the arena counter and attaches the update counter.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0 on success, -1 when the arena or the override is refused.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    void* view = 0;
    g_api = api; g_self = self;
    if (!api->guest_alloc(self, sizeof(uint32_t), 4, OP_GALLOC_NO_DMA, &view) || !view)
        return -1;
    g_count = (uint32_t*)view;
    g_every = (uint32_t)api->config_int(self, "log_every", 1);
    if (g_every == 0u) g_every = 1u;
    return api->override_name(self, "GamestateUpdate", on_update) ? -1 : 0;
}
