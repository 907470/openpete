/* arena-canary - the arena is timeline state, measured.
 *
 * One mutable arena block holds a mirror of the game's tick counter
 * (g_GameTick, incremented once per game update and zeroed by a level
 * load). Each tick the hook compares the guest counter with the mirror:
 * the guest value may equal the mirror (a tick the game did not
 * advance) or exceed it, but a guest value BELOW the mirror means guest
 * RAM went back in time and the arena did not - a rollback, rewind or
 * load that restored one and not the other. That is logged as DESYNC.
 * Zero is excluded: a level load legitimately resets the counter.
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

static void on_camera_update(CPUState* cpu) {
    uint32_t g = op_read_u32(g_api, OP_GADDR_g_GameTick);
    if (!g_armed_logged) {
        g_armed_logged = 1;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "arena-canary: armed (%u B mutable @0x%08X, guest tick %u)",
                   g_bytes, g_va, g);
    }
    if (g && g < g_mirror->last_tick) {
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
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
