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
 * @brief Attach @p fn to the running game's once-per-gameplay-tick camera update.
 *
 * Spyro 1 binds CameraUpdate by name. Spyro 2 and 3 name no such function
 * yet, so their arms bind the main-executable address: Spyro 2
 * func_8001D87C (src/camera.c), Spyro 3 func_8001204C (src/camera.c,
 * called from the UPDATE_CAMERA arm of func_80055294). Each is called once
 * per gameplay tick; the Spyro 2 and 3 calls stop during a portal exit and
 * level load and resume a few ticks into the fade-in.
 * @param api The engine table.
 * @param self This mod's handle.
 * @param fn The override body.
 * @return The override call's result, 0 on success.
 */
static int bind_tick_clock(const openpete_mod_api_t* api, openpete_mod_t* self,
                           openpete_mod_override_fn fn) {
#if OPENPETE_GAME_SPYRO1
    return api->override_name(self, "CameraUpdate", fn);
#elif OPENPETE_GAME_SPYRO2
    return api->override_addr(self, 0x8001D87Cu, fn);
#elif OPENPETE_GAME_SPYRO3
    return api->override_addr(self, 0x8001204Cu, fn);
#else
#error "sfx-play-probe: no camera clock binding for this game"
#endif
}

/**
 * @brief Mod entry point: registers the CameraUpdate pre-hook.
 * @param api  The engine's function table.
 * @param self This mod's handle.
 * @return The override registration's result; 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    return bind_tick_clock(api, self, on_camera_update);
}
