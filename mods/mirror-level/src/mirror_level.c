/* mirror-level — PoC rung 12: a NEW level slot (the TC keystone).
 *
 * Registers "mirror-artisans": the engine claims the free level slot
 * (id 65 / per-level index 35 in Spyro 1), clones Artisans' WAD TOC
 * entry into it (the stock async loader then streams Artisans' REAL
 * disc sectors for both the code overlay and the data blob), and
 * aliases the id-keyed load functions to the donor so the clone loads
 * and plays hub-faithful. The slot keeps its OWN per-level state index:
 * visited flag, gem count, and the killed-moby mask all start fresh —
 * a genuinely separate level that happens to look like Artisans.
 *
 * The PoC waits for gameplay in any retail level, triggers a
 * transition into the slot (the game's own level-select cheat path),
 * and then asserts the outcome structurally from guest state:
 *   - g_LevelId == 65 while g_Gamestate == GS_Playing (we are IN it)
 *   - g_LevelIndex == 35 (own state index, not the donor's 0)
 *   - overlay magic word == 11 (Artisans' code overlay is resident)
 *   - g_IsFlightLevel == 0 (the 65%10==5 flight fork was neutralized —
 *     the clone is hub-faithful, not flight-flavored)
 *
 * Pattern to copy: query level_slot_id() each tick until the claim
 * lands (it needs the boot TOC read), and only enter_level() from live
 * gameplay — the engine refuses otherwise.
 */

#include <openpete_mod_api.h>

#define ADDR_G_GAMESTATE   0x800757D8u
#define ADDR_G_LEVELID     0x8007596Cu
#define ADDR_G_LEVELINDEX  0x80075964u
#define ADDR_G_ISFLIGHT    0x80075690u
#define ADDR_OVERLAY_MAGIC 0x8007AA38u

#define SETTLE_TICKS 90 /* let the first level breathe before warping */

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static int g_ticks_in_play;
static int g_triggered;
static int g_verified;

static void on_gamestate_update(CPUState* cpu) {
    g_api->base(cpu);
    if (g_verified) return;

    uint32_t gs = op_read_u32(g_api, ADDR_G_GAMESTATE);
    uint32_t id = op_read_u32(g_api, ADDR_G_LEVELID);

    if (!g_triggered) {
        if (gs != 0) { g_ticks_in_play = 0; return; } /* GS_Playing only */
        int slot = g_api->level_slot_id(g_self, "mirror-artisans");
        if (slot <= 0) return; /* claim lands after the boot TOC read */
        if (++g_ticks_in_play < SETTLE_TICKS) return;
        if (g_api->enter_level(g_self, cpu, slot) == 0) {
            g_triggered = 1;
            g_api->log(g_self, OP_MOD_LOG_INFO,
                       "mirror-level: entering slot %d (from level %u)",
                       slot, id);
        }
        return;
    }

    /* Wait until we are PLAYING inside the slot, then verify. */
    if (gs != 0 || id != 65u) return;
    g_verified = 1;
    uint32_t index  = op_read_u32(g_api, ADDR_G_LEVELINDEX);
    uint32_t magic  = op_read_u32(g_api, ADDR_OVERLAY_MAGIC);
    uint32_t flight = op_read_u32(g_api, ADDR_G_ISFLIGHT);
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "mirror-level: reached gameplay id=%u index=%u magic=%u "
               "flight=%u", id, index, magic, flight);
    if (index == 35u && magic == 11u && flight == 0u)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "mirror-level: clone verified (own index, donor "
                   "overlay, hub-faithful)");
    else
        g_api->log(g_self, OP_MOD_LOG_ERROR,
                   "mirror-level: clone MISMATCH — see fields above");
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->api_version != OPENPETE_MOD_API_VERSION) return -1;
    if (api->register_level_slot(self, "mirror-artisans", 10) != 0)
        return -1;
    return api->override_name(self, "GamestateUpdate", on_gamestate_update);
}
