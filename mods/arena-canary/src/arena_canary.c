/* arena-canary - the arena is timeline state, measured.
 *
 * One mutable arena block holds a mirror of the game's tick counter
 * (g_GameTick, incremented once per game update and zeroed by a level
 * load). Each tick the hook compares the guest counter with the mirror:
 * the guest value may equal the mirror (a tick the game did not
 * advance) or exceed it, but a guest value BELOW the mirror means guest
 * RAM went back in time and the arena did not - a rollback, rewind or
 * load that restored one and not the other. That is logged as DESYNC.
 * On Spyro 1 a level load legitimately zeroes the counter in
 * LoadLevelScene, and GS_Playing increments it before the first
 * CameraUpdate after the load, so a pass-through link on LoadLevelScene
 * zeroes the mirror's last tick, in the arena with the guest RAM it
 * accompanies.
 *
 * Spyro 2 and 3 name no tick counter yet, so on those games the observed
 * value is the hook's own call count, kept in the arena block: it moves
 * with the block, so the DESYNC check cannot fire there, and the run
 * checks only that the block is allocated, written every tick, saved and
 * restored.
 *
 * Nothing here writes canonical guest RAM; the only state is the arena
 * block, so a DESYNC can only come from the engine carrying guest RAM
 * and the arena on different timelines.
 */

#include <openpete_mod_api.h>
#include "openpete_sdk_symbols.h"

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

typedef struct {
    uint32_t last_tick;   /* g_GameTick as last observed */
    uint32_t ticks_seen;  /* hook invocations (advances on every branch) */
    uint32_t desyncs;
} mirror_t;

static uint32_t  g_va;      /* arena block vaddr */
static mirror_t* g_mirror;  /* host view of the same bytes */
static uint32_t  g_bytes;
static int       g_armed_logged;

static void publish_status(void) {
    g_api->ui_status(g_self, "mirror block: 0x%08X (%u B, mutable)", g_va,
                     g_bytes);
    g_api->ui_status(g_self, "ticks %u  last guest tick %u  DESYNC %u",
                     g_mirror ? g_mirror->ticks_seen : 0u,
                     g_mirror ? g_mirror->last_tick : 0u,
                     g_mirror ? g_mirror->desyncs : 0u);
}

/**
 * @brief Read the tick value the mirror is checked against.
 *
 * Spyro 1 reads g_GameTick from guest RAM. Spyro 2 and 3 return the
 * mirror's hook count plus one, so the value advances once per call and
 * rolls back with the arena block.
 * @return The observed tick value.
 */
static uint32_t observed_tick(void) {
#if OPENPETE_GAME_SPYRO1
    return op_read_u32(g_api, OP_GADDR_g_GameTick);
#elif OPENPETE_GAME_SPYRO2 || OPENPETE_GAME_SPYRO3
    return g_mirror->ticks_seen + 1u;
#else
#error "arena-canary: no tick counter binding for this game"
#endif
}

/**
 * @brief Camera-clock pre-hook: checks the observed tick against the mirror, then advances the mirror.
 * @param cpu The guest register file at the call.
 */
static void on_camera_update(CPUState* cpu) {
    uint32_t g = observed_tick();
    if (!g_armed_logged) {
        g_armed_logged = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "arena-canary: armed (%u B mutable @0x%08X, guest tick %u)",
                   g_bytes, g_va, g);
    }
    if (g < g_mirror->last_tick) {
        g_mirror->desyncs++;
        if (g_mirror->desyncs == 1 || (g_mirror->desyncs & 63u) == 0)
            g_api->log(g_self, OP_MOD_LOG_ERROR,
                       "arena-canary: DESYNC #%u (mirror %u, guest tick %u) - "
                       "guest RAM moved back in time, the arena did not",
                       g_mirror->desyncs, g_mirror->last_tick, g);
    }
    g_mirror->last_tick = g;
    g_mirror->ticks_seen++;
    if ((g_mirror->ticks_seen & 255u) == 0) publish_status();
    g_api->base(cpu);
}

#if OPENPETE_GAME_SPYRO1
/**
 * @brief LoadLevelScene pass-through: zeroes the mirror's last tick, then runs the load.
 *
 * LoadLevelScene (0x8001364C, src/loaders.c) is g_GameTick's only reset
 * site; the mirror follows it so the next observed tick is not below it.
 * @param cpu The guest register file at the call.
 */
static void on_load_level_scene(CPUState* cpu) {
    g_mirror->last_tick = 0;
    g_api->base(cpu);
}
#endif

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
#error "arena-canary: no camera clock binding for this game"
#endif
}

/**
 * @brief Mod entry: allocates the mutable mirror block and attaches the check to the camera clock.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return -1 when the allocation fails, else the first non-zero override registration result, 0 on success.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    int64_t slots = api->config_int(self, "slots", 4);
    if (slots < 1) slots = 1;
    if (slots > 64) slots = 64;
    g_bytes = (uint32_t)slots * 16u;
    void* host = 0;
    g_va = api->guest_alloc(self, g_bytes, 16, 0, &host);
    if (!g_va || !host) {
        api->log(self, OP_MOD_LOG_ERROR, "arena-canary: alloc FAILED");
        return -1;
    }
    g_mirror = (mirror_t*)host;
    /* Replayed on re-entry: the block keeps its bytes, so the mirror
     * continues rather than restarting - a restart would itself look
     * like a rollback to the check above. */
    api->log(self, OP_MOD_LOG_INFO,
             "arena-canary: block 0x%08X (%u B) ticks so far %u", g_va,
             g_bytes, g_mirror->ticks_seen);
    publish_status();
#if OPENPETE_GAME_SPYRO1
    int rc = api->override_name(self, "LoadLevelScene", on_load_level_scene);
    if (rc) return rc;
#endif
    return bind_tick_clock(api, self, on_camera_update);
}
