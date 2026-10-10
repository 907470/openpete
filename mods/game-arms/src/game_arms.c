/**
 * @file game_arms.c
 * @brief game-arms: one source, one override arm per game, closed by #error.
 *
 * The per-game SDK defines exactly one of OPENPETE_GAME_SPYRO1/2/3 to 1,
 * so a mod whose functions differ between games selects its bindings
 * with `#if OPENPETE_GAME_SPYROn` arms and closes the chain with
 * `#else #error`: a fourth game's SDK then stops the compile instead of
 * building a mod that binds nothing.
 *
 * Every arm binds the same three game functions, each observed with a
 * counting pre-hook:
 * - the once-per-gameplay-tick camera update: Spyro 1 CameraUpdate,
 *   Spyro 2 func_8001D87C, Spyro 3 func_8001204C;
 * - the item-collect function: Spyro 1 CollectItem, Spyro 2
 *   func_80039678, Spyro 3 func_80036E60;
 * - the sine on the 4096-unit circle: Spyro 1 Sin, Spyro 2
 *   func_8001B61C, Spyro 3 func_8004E9E4.
 * Spyro 1 names all three, so its arm binds by name; Spyro 2 and 3 name
 * none of them yet, so theirs bind the main-executable addresses.
 */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;   /**< Engine table, saved at entry. */
static openpete_mod_t*           g_self;  /**< This mod's handle, saved at entry. */
static unsigned g_clock, g_collect, g_sine; /**< Calls seen per override. */

/**
 * @brief Count a call, log the first one, and run the rest of the chain.
 * @param cpu The guest register file at the call.
 * @param n The override's counter.
 * @param what The override's label for the log line.
 */
static void observe(CPUState* cpu, unsigned* n, const char* what) {
    if ((*n)++ == 0u)
        g_api->log(g_self, OP_MOD_LOG_INFO, "%s: first call", what);
    g_api->base(cpu);
}

/** @brief Pre-hook on the camera update. @param cpu The guest register file. */
static void on_clock(CPUState* cpu) { observe(cpu, &g_clock, "camera clock"); }
/** @brief Pre-hook on the item-collect function. @param cpu The guest register file. */
static void on_collect(CPUState* cpu) { observe(cpu, &g_collect, "collect"); }
/** @brief Pre-hook on the sine. @param cpu The guest register file. */
static void on_sine(CPUState* cpu) { observe(cpu, &g_sine, "sine"); }

/**
 * @brief Mod entry point: registers this game's arm of the three overrides.
 * @param api  The engine's function table.
 * @param self This mod's handle.
 * @return 0 when every override registered, else the first failure.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    int rc;
    g_api = api; g_self = self;
#if OPENPETE_GAME_SPYRO1
    rc = api->override_name(self, "CameraUpdate", on_clock);
    if (rc == 0) rc = api->override_name(self, "CollectItem", on_collect);
    if (rc == 0) rc = api->override_name(self, "Sin", on_sine);
#elif OPENPETE_GAME_SPYRO2
    rc = api->override_addr(self, 0x8001D87Cu, on_clock);
    if (rc == 0) rc = api->override_addr(self, 0x80039678u, on_collect);
    if (rc == 0) rc = api->override_addr(self, 0x8001B61Cu, on_sine);
#elif OPENPETE_GAME_SPYRO3
    rc = api->override_addr(self, 0x8001204Cu, on_clock);
    if (rc == 0) rc = api->override_addr(self, 0x80036E60u, on_collect);
    if (rc == 0) rc = api->override_addr(self, 0x8004E9E4u, on_sine);
#else
#error "game-arms: no override arm for this game"
#endif
    return rc;
}
