/* chain-duo-b - proves stacking + order semantics (see
 * chain-duo-a). Logs the nesting once: "B>" before base(), "<B" after. */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int                       g_logged;
static int                       g_calls;

/* Status lines, repainted from the override (per tick). The nesting
 * log is a one-shot per load (hot reload re-arms it). */
static void publish_status(void) {
    g_api->ui_status(g_self, "CameraUpdate calls: %d", g_calls);
    g_api->ui_status(g_self, "half B of the pair: logs \"B>\" before "
                     "base(), \"<B\" after");
    g_api->ui_status(g_self,
                     "enabled-list order decides which half is outermost");
}

static void on_camera_update(CPUState* cpu) {
    int log_this;
    log_this = !g_logged;
    g_logged = 1;
    g_calls++;
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain enter B>");
    g_api->base(cpu);
    if (log_this) g_api->log(g_self, OP_MOD_LOG_INFO, "chain exit  <B");
    publish_status();
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
#error "chain-duo-b: no camera clock binding for this game"
#endif
}

/**
 * @brief Mod entry: attaches the nesting logger to the camera clock.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return The override registration's result, 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    publish_status();
    return bind_tick_clock(api, self, on_camera_update);
}
