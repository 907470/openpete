/**
 * @file pose_report.c
 * @brief pose-report: the player's pose and the level on every game from one source.
 *
 * The source names no game function, address or structure and reads no
 * per-game header, so the same text compiles for each game's SDK and as
 * a "*" object under OPENPETE_MOD_ANY_GAME. Everything game-specific is
 * behind the api's game services: game_id, gameplay_live, player_pose
 * and level_id.
 *
 * With no game function to override, the mod's only per-tick context is
 * a present hook. The services called with a NULL cpu return the record
 * the engine sampled at the last canonical tick boundary, which is legal
 * from a present hook and reads the same on every present of a tick.
 */

#include <openpete_mod_api.h>

#include <stddef.h>

static const openpete_mod_api_t* g_api;   /**< Engine table, saved at entry. */
static openpete_mod_t*           g_self;  /**< This mod's handle, saved at entry. */
static uint32_t                  g_last_tick;   /**< Tick of the last report. */
static int32_t                   g_last_level = -2; /**< Level of the last report; -2 before the first. */

/** Ticks between reports while the level stays the same (about one second). */
#define REPORT_TICKS 30u

/**
 * @brief Report the sampled pose when a second has passed or the level changed.
 * @param ctx The present's context; unused, since the record carries its tick.
 */
static void on_present(const openpete_present_ctx_t* ctx) {
    openpete_mod_pose_t p = { .struct_size = sizeof(openpete_mod_pose_t) };
    (void)ctx;
    if (g_api->player_pose(g_self, NULL, &p) != 0) return;
    if (p.level == g_last_level && p.tick < g_last_tick + REPORT_TICKS) return;
    g_last_tick  = p.tick;
    g_last_level = p.level;
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "pose tick %u live %d level %d/%d pos (%d, %d, %d) yaw %d placeable %u",
               p.tick, g_api->gameplay_live(g_self, NULL), p.level,
               g_api->level_id(g_self, NULL), p.x, p.y, p.z, p.yaw, p.placeable);
    g_api->notify(g_self, "pose", 1500, "LEVEL %d  %d %d %d", p.level,
                  p.x, p.y, p.z);
}

/**
 * @brief Mod entry point: names the game and registers the present hook.
 * @param api  The engine's function table.
 * @param self This mod's handle.
 * @return 0 on success, the registration's result otherwise.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    api->log(self, OP_MOD_LOG_INFO, "running on %s", api->game_id(self));
    return api->register_present_hook(self, on_present);
}
