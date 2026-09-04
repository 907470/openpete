/**
 * @file hello.c
 * @brief hello-anchor: the smallest code mod. One named override that
 *        logs, calls the original, and publishes a counter.
 *
 * The override attaches to CameraUpdate, the game's per-tick camera
 * driver (func_80037BD4 in the original binary). It runs before the
 * original on every call and passes control through with base().
 *
 * The mod reads nothing from guest RAM and writes nothing to it, so the
 * game plays byte-identically with the mod enabled. A mod that only
 * observes is the right first mod to write: any divergence the engine's
 * gameplay oracle reports afterwards is then attributable to the mod's
 * first real write, not to scaffolding.
 */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;   /**< Engine table, saved at entry. */
static openpete_mod_t*           g_self;  /**< This mod's handle, saved at entry. */
static unsigned                  g_calls; /**< CameraUpdate calls seen this session. */

/**
 * @brief Rewrite the mod's status lines in the Mods panel.
 *
 * ui_status replaces the whole block on every call, so a mod repaints
 * every line it wants visible rather than appending one. The "waiting"
 * line published at entry stays until the first call replaces it.
 */
static void publish_status(void) {
    g_api->ui_status(g_self, "CameraUpdate calls: %u", g_calls);
    g_api->ui_status(g_self, "%s", g_calls
        ? "observing (logs every 300th call)"
        : "waiting for the first call");
}

/**
 * @brief The override body. Runs in place of CameraUpdate on every call.
 * @param cpu The guest register file at the call. Arguments are in
 *            cpu->a0..a3 and the return value lands in cpu->v0 after
 *            base(); this observer touches neither.
 *
 * Logging every call would flood the log at 29.913 ticks per second, so
 * the mod logs the first call and then every 300th (about ten seconds).
 * base() is called last, which makes this a pre-hook: the mod's code runs
 * first, the original last. Calling base() first and then reading the
 * result is the post-hook shape; natural-gems shows that one.
 */
static void on_camera_update(CPUState* cpu) {
    g_calls++;
    if (g_calls == 1u || (g_calls % 300u) == 0u)
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "hello from the override chain: CameraUpdate call #%u",
                   g_calls);
    g_api->base(cpu);
    publish_status();
}

/**
 * @brief Mod entry point, called once after the object loads.
 * @param api  The engine's function table. Valid for the life of the mod.
 * @param self This mod's handle, passed back to every api call.
 * @return 0 on success. A non-zero return disables the mod and the loader
 *         removes every registration it made.
 *
 * There is no api_version check here: the `api` pin in mod.toml is the
 * minimum version this mod needs, and the loader refuses to load the mod
 * on an older engine before entry runs. Check api_version only for an
 * entry the mod can do without on an older engine.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    publish_status();
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
