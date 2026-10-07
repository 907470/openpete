/**
 * @file probe.c
 * @brief sfx-play-probe: plays assets/sfx/own/probe.wav every 150th
 *        CameraUpdate call, logging each play.
 */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;   /**< Engine table, saved at entry. */
static openpete_mod_t*           g_self;  /**< This mod's handle, saved at entry. */
static unsigned                  g_calls; /**< CameraUpdate calls seen this session. */

/**
 * @brief CameraUpdate pre-hook: every 150th call plays the clip and logs the call count.
 * @param cpu The guest register file at the call; passed through to base().
 */
static void on_camera_update(CPUState* cpu) {
    g_calls++;
    if (g_calls % 150u == 0u) {
        const int rc = g_api->sfx_play(g_self, "probe", 1.0f);
        g_api->log(g_self, OP_MOD_LOG_INFO, "sfx-play-probe: play at CameraUpdate call #%u rc=%d",
                   g_calls, rc);
    }
    g_api->base(cpu);
}

/**
 * @brief Mod entry point: registers the CameraUpdate pre-hook.
 * @param api  The engine's function table.
 * @param self This mod's handle.
 * @return The override registration's result; 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
