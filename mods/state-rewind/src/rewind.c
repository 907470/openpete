/* state-rewind — engine-services proof: savestate trigger +
 * participation callbacks. See mod.toml. */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h>

#include <string.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;

static unsigned g_ticks;        /* participates in savestates */
static uint32_t g_gems_at_save; /* participates in savestates */
static int      g_loads;        /* host-side: NOT in the blob, so the
                                 * rewound timeline doesn't re-queue the
                                 * load forever */

static int on_state_save(void* buf, int cap) {
    if (cap < 8) return -1;
    memcpy(buf, &g_ticks, 4);
    memcpy((char*)buf + 4, &g_gems_at_save, 4);
    return 8;
}

static void on_state_load(const void* buf, int len) {
    if (len >= 8) {
        memcpy(&g_ticks, buf, 4);
        memcpy(&g_gems_at_save, (const char*)buf + 4, 4);
    }
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "mod state restored: ticks=%u", g_ticks);
}

static void on_camera_update(CPUState* cpu) {
    g_ticks++;
    if (g_ticks == 300u && g_loads == 0) {
        g_gems_at_save = op_read_u32(g_api, OP_GADDR_g_GemTotal);
        g_api->savestate_request(g_self, 1 /*save*/, 9);
    }
    if (g_ticks == 600u && g_loads == 0) {
        g_loads = 1;
        g_api->savestate_request(g_self, 2 /*load*/, 9);
    }
    if (g_ticks == 301u && g_loads == 1) {
        uint32_t gems = op_read_u32(g_api, OP_GADDR_g_GemTotal);
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "post-rewind gems match=%s (%u vs %u at save)",
                   gems == g_gems_at_save ? "yes" : "NO",
                   gems, g_gems_at_save);
    }
    g_api->base(cpu);
}

int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api;
    g_self = self;
    if (api->register_savestate_hooks(self, on_state_save,
                                      on_state_load) != 0)
        return -1;
    return api->override_name(self, "CameraUpdate", on_camera_update);
}
